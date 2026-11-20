#include "media.hpp"
#include "homesight/clip_frame_sampler.hpp"
#include "homesight/errors.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <fcntl.h>
#include <gst/app/gstappsink.h>
#include <gst/video/video.h>
#include <mutex>
#include <opencv2/imgproc.hpp>
#include <unistd.h>

namespace homesight::detail
{
void initialize_gstreamer()
{
    static std::once_flag initialized;
    std::call_once(initialized,
                   []
                   {
                       gst_init(nullptr, nullptr);
                   });
}
void call_gstreamer_async(GstElement *element, AsyncElementFunction function, gpointer data,
                          GDestroyNotify destroy)
{
#if GST_CHECK_VERSION(1, 28, 0)
    struct Call
    {
        AsyncElementFunction function;
        gpointer data;
        GDestroyNotify destroy;
        ~Call()
        {
            if (destroy)
            {
                destroy(data);
            }
        }
    };
    auto *call = new Call{function, data, destroy};
    gst_object_call_async(
        GST_OBJECT(element),
        [](GstObject *object, gpointer pointer)
        {
            std::unique_ptr<Call> invocation(static_cast<Call *>(pointer));
            invocation->function(GST_ELEMENT(object), invocation->data);
        },
        call);
#else
    gst_element_call_async(element, function, data, destroy);
#endif
}
std::string new_id()
{
    auto *value = g_uuid_string_random();
    std::string result(value);
    g_free(value);
    return result;
}
std::int64_t utc_epoch_ms()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}
void sync_file(const std::filesystem::path &path)
{
    int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0)
    {
        throw RecorderError("Unable to open file for durable synchronization");
    }
    int result = fsync(fd);
    close(fd);
    if (result != 0)
    {
        throw RecorderError("Unable to synchronize file");
    }
}
void sync_directory(const std::filesystem::path &path)
{
    sync_file(path.empty() ? std::filesystem::path(".") : path);
}
void create_directories_durable(const std::filesystem::path &path)
{
    auto absolute = std::filesystem::absolute(path).lexically_normal();
    std::vector<std::filesystem::path> missing;
    auto ancestor = absolute;
    while (!std::filesystem::exists(ancestor))
    {
        missing.push_back(ancestor);
        ancestor = ancestor.parent_path();
    }
    if (!std::filesystem::is_directory(ancestor))
    {
        throw RecorderError("A required recording directory is not a directory");
    }
    for (auto entry = missing.rbegin(); entry != missing.rend(); ++entry)
    {
        std::filesystem::create_directory(*entry);
        sync_directory(*entry);
        sync_directory(entry->parent_path());
    }
}
void remove_file_durable(const std::filesystem::path &path)
{
    std::filesystem::remove(path);
    sync_directory(path.parent_path());
}
MediaReader::MediaReader(const std::filesystem::path &path, bool use_gpu, double timeout_seconds)
    : deadline_(std::chrono::steady_clock::now() +
                std::chrono::milliseconds(static_cast<long long>(timeout_seconds * 1000)))
{
    initialize_gstreamer();
    if (!std::filesystem::is_regular_file(path))
    {
        throw SegmentDecodingError("Segment file does not exist");
    }
    GError *error = nullptr;
    pipeline_ =
        gst_parse_launch("uridecodebin name=source ! videoconvert ! "
                         "video/x-raw,format=RGB ! appsink name=frames sync=false max-buffers=2",
                         &error);
    if (error || !pipeline_)
    {
        if (error)
        {
            g_error_free(error);
        }
        if (pipeline_)
        {
            gst_object_unref(pipeline_);
        }
        throw SegmentDecodingError("Required GStreamer decoding plugins are unavailable");
    }
    auto *source = gst_bin_get_by_name(GST_BIN(pipeline_), "source");
    auto *uri = gst_filename_to_uri(std::filesystem::absolute(path).c_str(), nullptr);
    g_object_set(source, "uri", uri, "force-sw-decoders", !use_gpu, nullptr);
    g_free(uri);
    gst_object_unref(source);
    sink_ = gst_bin_get_by_name(GST_BIN(pipeline_), "frames");
    gst_element_set_state(pipeline_, GST_STATE_PAUSED);
    auto state = gst_element_get_state(pipeline_, nullptr, nullptr,
                                       static_cast<GstClockTime>(timeout_seconds * GST_SECOND));
    if (state == GST_STATE_CHANGE_FAILURE || state == GST_STATE_CHANGE_ASYNC)
    {
        call_gstreamer_async(
            pipeline_,
            [](GstElement *element, gpointer)
            {
                gst_element_set_state(element, GST_STATE_NULL);
            },
            nullptr, nullptr);
        gst_object_unref(sink_);
        gst_object_unref(pipeline_);
        throw SegmentDecodingError("Cannot decode segment within the configured timeout");
    }
    gst_element_set_state(pipeline_, GST_STATE_PLAYING);
}
MediaReader::~MediaReader()
{
    call_gstreamer_async(
        pipeline_,
        [](GstElement *element, gpointer)
        {
            gst_element_set_state(element, GST_STATE_NULL);
        },
        nullptr, nullptr);
    gst_object_unref(sink_);
    gst_object_unref(pipeline_);
}
double MediaReader::duration_seconds() const
{
    gint64 duration = 0;
    if (!gst_element_query_duration(pipeline_, GST_FORMAT_TIME, &duration) || duration <= 0)
    {
        throw SegmentDecodingError("Segment has no valid media duration");
    }
    return static_cast<double>(duration) / GST_SECOND;
}
void MediaReader::seek(double seconds)
{
    if (!gst_element_seek_simple(
            pipeline_, GST_FORMAT_TIME,
            static_cast<GstSeekFlags>(GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_ACCURATE),
            static_cast<gint64>(seconds * GST_SECOND)))
    {
        throw SegmentDecodingError("Segment does not support seeking");
    }
}
std::optional<DecodedFrame> MediaReader::read_frame()
{
    while (std::chrono::steady_clock::now() < deadline_)
    {
        GstSample *sample = gst_app_sink_try_pull_sample(GST_APP_SINK(sink_), 50 * GST_MSECOND);
        if (sample)
        {
            GstVideoInfo info;
            gst_video_info_init(&info);
            GstVideoFrame frame;
            auto *buffer = gst_sample_get_buffer(sample);
            if (!gst_video_info_from_caps(&info, gst_sample_get_caps(sample)) ||
                !gst_video_frame_map(&frame, &info, buffer, GST_MAP_READ))
            {
                gst_sample_unref(sample);
                throw SegmentDecodingError("Cannot map decoded frame");
            }
            cv::Mat pixels(static_cast<int>(info.height), static_cast<int>(info.width), CV_8UC3,
                           GST_VIDEO_FRAME_PLANE_DATA(&frame, 0),
                           static_cast<std::size_t>(GST_VIDEO_FRAME_PLANE_STRIDE(&frame, 0)));
            const auto pts = GST_BUFFER_PTS(buffer);
            auto stream_time = GST_CLOCK_TIME_IS_VALID(pts)
                                   ? gst_segment_to_stream_time(gst_sample_get_segment(sample),
                                                                GST_FORMAT_TIME, pts)
                                   : GST_CLOCK_TIME_NONE;
            const auto duration = GST_BUFFER_DURATION(buffer);
            if (!GST_CLOCK_TIME_IS_VALID(stream_time))
            {
                gst_video_frame_unmap(&frame);
                gst_sample_unref(sample);
                throw SegmentDecodingError("Decoded frame has no presentation timestamp");
            }
            double frame_duration =
                GST_CLOCK_TIME_IS_VALID(duration)
                    ? static_cast<double>(duration) / GST_SECOND
                    : (info.fps_n > 0 ? static_cast<double>(info.fps_d) / info.fps_n : 0);
            DecodedFrame result{pixels.clone(), static_cast<double>(stream_time) / GST_SECOND,
                                frame_duration};
            gst_video_frame_unmap(&frame);
            gst_sample_unref(sample);
            return result;
        }
        auto *bus = gst_element_get_bus(pipeline_);
        auto *error = gst_bus_pop_filtered(bus, GST_MESSAGE_ERROR);
        gst_object_unref(bus);
        if (error)
        {
            gst_message_unref(error);
            throw SegmentDecodingError("GStreamer failed while decoding segment");
        }
        if (gst_app_sink_is_eos(GST_APP_SINK(sink_)))
        {
            return std::nullopt;
        }
    }
    throw SegmentDecodingError("Segment decoding exceeded configured timeout");
}
} // namespace homesight::detail

namespace homesight
{
SampledClipFrames sample_clip_frames_from_segment(const std::filesystem::path &path, int count,
                                                  int width, int height, bool use_gpu,
                                                  double timeout)
{
    if (count < 1 || count > 4096 || width < 1 || height < 1 || width > 8192 || height > 8192 ||
        !std::isfinite(timeout) || timeout <= 0 || timeout > 86400 ||
        static_cast<std::uint64_t>(count) * static_cast<unsigned>(width) *
                static_cast<unsigned>(height) * 3 >
            1024ULL * 1024 * 1024)
    {
        throw InvalidConfigError("Invalid frame sampling dimensions or timeout (limit: 1 GiB)");
    }
    detail::MediaReader reader(path, use_gpu, timeout);
    const auto duration = reader.duration_seconds();
    SampledClipFrames result;
    result.frame_count = count;
    result.frame_width = width;
    result.frame_height = height;
    const auto frame_bytes = static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 3;
    result.rgb_pixels.resize(frame_bytes * static_cast<std::size_t>(count));
    for (int i = 0; i < count; ++i)
    {
        reader.seek(duration * (static_cast<double>(i) + 0.5) / count);
        auto frame = reader.read_frame();
        if (!frame)
        {
            throw SegmentDecodingError("Segment ended before the requested frame");
        }
        const double aspect = static_cast<double>(width) / height;
        int crop_width = frame->rgb.cols;
        int crop_height = frame->rgb.rows;
        if (static_cast<double>(crop_width) / crop_height > aspect)
        {
            crop_width = std::max(1, static_cast<int>(crop_height * aspect));
        }
        else
        {
            crop_height = std::max(1, static_cast<int>(crop_width / aspect));
        }
        cv::Rect crop((frame->rgb.cols - crop_width) / 2, (frame->rgb.rows - crop_height) / 2,
                      crop_width, crop_height);
        cv::Mat output(height, width, CV_8UC3,
                       result.rgb_pixels.data() + static_cast<std::size_t>(i) * frame_bytes);
        cv::resize(frame->rgb(crop), output, output.size(), 0, 0, cv::INTER_AREA);
        result.frame_timestamps_ms.push_back(static_cast<std::int64_t>(frame->seconds * 1000));
    }
    return result;
}
} // namespace homesight
