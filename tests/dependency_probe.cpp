// Standalone diagnostic: deliberately links no HomeSight code or headers.
#include <atomic>
#include <chrono>
#include <gst/app/gstappsink.h>
#include <gst/gst.h>
#include <gst/video/video.h>
#include <memory>
#include <opencv2/core.hpp>
#include <thread>

struct Payload
{
    int immutable_value = 42;
    std::atomic<int> *completed;
};

int main(int argc, char **argv)
{
    if (argc != 2)
    {
        return 2;
    }
    gst_init(nullptr, nullptr);
    std::atomic<int> completed{0};
    auto *object = gst_pipeline_new(nullptr);
    for (int index = 0; index < 20; ++index)
    {
        auto *payload = new Payload{42, &completed};
#if GST_CHECK_VERSION(1, 28, 0)
        gst_object_call_async(
            GST_OBJECT(object),
            [](GstObject *, gpointer pointer)
            {
                std::unique_ptr<Payload> value(static_cast<Payload *>(pointer));
                if (value->immutable_value == 42)
                {
                    ++*value->completed;
                }
            },
            payload);
#else
        gst_element_call_async(
            object,
            [](GstElement *, gpointer pointer)
            {
                const auto &value = *static_cast<Payload *>(pointer);
                if (value.immutable_value == 42)
                {
                    ++*value.completed;
                }
            },
            payload,
            [](gpointer pointer)
            {
                delete static_cast<Payload *>(pointer);
            });
#endif
    }
    while (completed.load() < 20)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    gst_object_unref(object);
    GError *error = nullptr;
    auto *pipeline =
        gst_parse_launch("uridecodebin name=source ! videoconvert ! video/x-raw,format=RGB ! "
                         "appsink name=frames sync=false max-buffers=2",
                         &error);
    if (!pipeline || error)
    {
        return 3;
    }
    auto *source = gst_bin_get_by_name(GST_BIN(pipeline), "source");
    auto *uri = gst_filename_to_uri(argv[1], nullptr);
    g_object_set(source, "uri", uri, "force-sw-decoders", TRUE, nullptr);
    g_free(uri);
    gst_object_unref(source);
    auto *sink = gst_bin_get_by_name(GST_BIN(pipeline), "frames");
    auto *bus = gst_element_get_bus(pipeline);
    gst_element_set_state(pipeline, GST_STATE_PLAYING);
    int frames = 0;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (std::chrono::steady_clock::now() < deadline)
    {
        while (auto *message = gst_bus_pop(bus))
        {
            const auto type = GST_MESSAGE_TYPE(message);
            gst_message_unref(message);
            if (type == GST_MESSAGE_ERROR)
            {
                return 6;
            }
        }
        auto *sample = gst_app_sink_try_pull_sample(GST_APP_SINK(sink), 100 * GST_MSECOND);
        if (!sample)
        {
            if (gst_app_sink_is_eos(GST_APP_SINK(sink)))
            {
                break;
            }
            continue;
        }
        GstVideoInfo info;
        gst_video_info_init(&info);
        GstVideoFrame frame;
        auto *buffer = gst_sample_get_buffer(sample);
        if (!gst_video_info_from_caps(&info, gst_sample_get_caps(sample)) ||
            !gst_video_frame_map(&frame, &info, buffer, GST_MAP_READ))
        {
            return 4;
        }
        const auto pts = GST_BUFFER_PTS(buffer);
        const auto duration = GST_BUFFER_DURATION(buffer);
        cv::Mat pixels(static_cast<int>(info.height), static_cast<int>(info.width), CV_8UC3,
                       GST_VIDEO_FRAME_PLANE_DATA(&frame, 0),
                       static_cast<std::size_t>(GST_VIDEO_FRAME_PLANE_STRIDE(&frame, 0)));
        const auto owned = pixels.clone();
        if (GST_CLOCK_TIME_IS_VALID(pts) && GST_CLOCK_TIME_IS_VALID(duration) && !owned.empty())
        {
            ++frames;
        }
        gst_video_frame_unmap(&frame);
        gst_sample_unref(sample);
    }
    gst_element_set_state(pipeline, GST_STATE_NULL);
    gst_object_unref(sink);
    gst_object_unref(bus);
    gst_object_unref(pipeline);
    gst_deinit();
    return frames > 0 ? 0 : 5;
}
