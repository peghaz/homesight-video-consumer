#include "recording_pipeline.hpp"
#include "homesight/errors.hpp"
#include "media.hpp"
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <exception>
#include <gst/gst.h>
#include <gst/pbutils/pbutils.h>
#include <map>
#include <mutex>
#include <thread>

namespace homesight::detail
{
RtspConnectionSettings parse_rtsp_connection_settings(const std::string &url)
{
    RtspConnectionSettings result{url, {}, {}};
    const auto scheme_end = url.find("://");
    if (scheme_end == std::string::npos)
    {
        throw InvalidConfigError("Invalid RTSP URL");
    }
    const auto authority_start = scheme_end + 3;
    const auto authority_end = url.find_first_of("/?#", authority_start);
    const auto authority = url.substr(authority_start, authority_end - authority_start);
    const auto separator = authority.rfind('@');
    if (separator == std::string::npos)
    {
        return result;
    }
    const auto user_info = authority.substr(0, separator);
    const auto password_start = user_info.find(':');
    const auto decode = [](const std::string &value)
    {
        std::unique_ptr<gchar, decltype(&g_free)> decoded(
            g_uri_unescape_string(value.c_str(), nullptr), g_free);
        if (!decoded)
        {
            throw InvalidConfigError("Invalid percent-encoding in RTSP credentials");
        }
        return std::string(decoded.get());
    };
    result.username = decode(user_info.substr(0, password_start));
    if (password_start != std::string::npos)
    {
        result.password = decode(user_info.substr(password_start + 1));
    }
    result.location.erase(authority_start, separator + 1);
    return result;
}
std::int64_t monotonic_ms()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}
namespace
{
struct ActiveSegment
{
    SegmentInfo info;
    GstClockTime start_running_time = 0;
};
class Session : public std::enable_shared_from_this<Session>
{
  public:
    const SegmentRecorderConfig &config;
    std::function<void(const SegmentInfo &)> opened;
    std::function<void(const SegmentInfo &)> closed;
    std::function<void()> recording;
    GstElement *pipeline = nullptr;
    GstElement *source = nullptr;
    GstElement *sink = nullptr;
    std::atomic<GstElement *> queue{nullptr};
    std::atomic<bool> pad_linked{false};
    std::atomic<bool> seen_buffer{false};
    std::atomic<std::int64_t> last_activity{monotonic_ms()};
    std::atomic<bool> teardown_started{false};
    std::mutex teardown_mutex;
    std::condition_variable teardown_changed;
    bool teardown_done = false;
    std::mutex mutex;
    std::map<std::string, ActiveSegment> active;
    std::exception_ptr error;
    std::string codec;
    std::string continuity = new_id();
    Session(const SegmentRecorderConfig &c, std::function<void(const SegmentInfo &)> o,
            std::function<void(const SegmentInfo &)> f, std::function<void()> r)
        : config(c), opened(std::move(o)), closed(std::move(f)), recording(std::move(r))
    {
        const auto connection = parse_rtsp_connection_settings(c.camera.rtsp_url);
        initialize_gstreamer();
        pipeline = gst_pipeline_new(nullptr);
        source = gst_element_factory_make("rtspsrc", nullptr);
        sink = gst_element_factory_make("splitmuxsink", nullptr);
        if (!source || !sink)
        {
            if (source)
            {
                gst_object_unref(source);
            }
            if (sink)
            {
                gst_object_unref(sink);
            }
            gst_object_unref(pipeline);
            throw RecorderError("Required GStreamer RTSP or splitmux plugins are unavailable");
        }
        gst_bin_add_many(GST_BIN(pipeline), source, sink, nullptr);
        g_object_set(source, "location", connection.location.c_str(), "user-id",
                     connection.username.c_str(), "user-pw", connection.password.c_str(), "latency",
                     c.camera.rtsp_latency_ms, "protocols",
                     c.camera.rtsp_transport == RtspTransport::tcp ? 4 : 1, "tcp-timeout",
                     static_cast<guint64>(c.network_timeout_seconds * G_USEC_PER_SEC), "timeout",
                     static_cast<guint64>(c.network_timeout_seconds * G_USEC_PER_SEC),
                     "teardown-timeout", static_cast<guint64>(0), nullptr);
        g_object_set(
            sink, "max-size-time", static_cast<guint64>(c.segment_duration_seconds * GST_SECOND),
            "max-size-bytes", static_cast<guint64>(0), "send-keyframe-requests", true,
            "async-finalize", true, "muxer-factory",
            c.container_format == VideoContainerFormat::mp4 ? "mp4mux" : "matroskamux", nullptr);
        g_signal_connect(source, "select-stream", G_CALLBACK(select_stream), this);
        g_signal_connect(source, "pad-added", G_CALLBACK(pad_added), this);
        g_signal_connect(sink, "format-location-full", G_CALLBACK(format_location), this);
    }
    ~Session()
    {
        gst_object_unref(pipeline);
    }
    bool check_callback_error(bool stopping)
    {
        std::lock_guard lock(mutex);
        if (error)
        {
            try
            {
                std::rethrow_exception(error);
            }
            catch (const RecordingStopped &)
            {
                if (stopping)
                {
                    return false;
                }
                throw;
            }
        }
        return true;
    }
    void request_eos()
    {
        auto *owner = new std::shared_ptr<Session>(shared_from_this());
        call_gstreamer_async(
            pipeline,
            [](GstElement *element, gpointer data)
            {
                const auto &self = **static_cast<std::shared_ptr<Session> *>(data);
                auto *target = self.queue.load();
                gst_element_send_event(target ? target : element, gst_event_new_eos());
            },
            owner,
            [](gpointer data)
            {
                delete static_cast<std::shared_ptr<Session> *>(data);
            });
    }
    void begin_teardown()
    {
        if (teardown_started.exchange(true))
        {
            return;
        }
        auto *owner = new std::shared_ptr<Session>(shared_from_this());
        call_gstreamer_async(
            pipeline,
            [](GstElement *element, gpointer data)
            {
                auto &self = **static_cast<std::shared_ptr<Session> *>(data);
                gst_element_set_state(element, GST_STATE_NULL);
                std::lock_guard lock(self.teardown_mutex);
                self.teardown_done = true;
                self.teardown_changed.notify_all();
            },
            owner,
            [](gpointer data)
            {
                delete static_cast<std::shared_ptr<Session> *>(data);
            });
    }
    bool wait_for_teardown(std::int64_t deadline)
    {
        std::unique_lock lock(teardown_mutex);
        do
        {
            if (teardown_done && weak_from_this().use_count() <= 2)
            {
                return true;
            }
            teardown_changed.wait_for(lock, std::chrono::milliseconds(2));
        } while (monotonic_ms() < deadline);
        return teardown_done && weak_from_this().use_count() <= 2;
    }
    static gboolean select_stream(GstElement *, guint, GstCaps *caps, gpointer) noexcept
    {
        const auto *structure = gst_caps_get_structure(caps, 0);
        const auto *media = gst_structure_get_string(structure, "media");
        return g_strcmp0(media, "video") == 0;
    }
    static void pad_added(GstElement *, GstPad *pad, gpointer data) noexcept
    {
        auto &self = *static_cast<Session *>(data);
        try
        {
            if (self.pad_linked.exchange(true))
            {
                return;
            }
            auto *caps = gst_pad_get_current_caps(pad);
            if (!caps)
            {
                return;
            }
            const auto *structure = gst_caps_get_structure(caps, 0);
            const auto *encoding = gst_structure_get_string(structure, "encoding-name");
            std::string name = encoding ? encoding : "";
            gst_caps_unref(caps);
            if (name != "H264" && name != "H265")
            {
                throw RecorderError("Camera must supply H.264 or H.265 video");
            }
            auto *depay =
                gst_element_factory_make(name == "H264" ? "rtph264depay" : "rtph265depay", nullptr);
            auto *parser =
                gst_element_factory_make(name == "H264" ? "h264parse" : "h265parse", nullptr);
            // RTP transports presentation timestamps. Reconstruct decode timestamps for
            // reordered pictures when the deployment provides a codec timestamper.
            auto *timestamper = gst_element_factory_make(
                name == "H264" ? "h264timestamper" : "h265timestamper", nullptr);
            auto *queue = gst_element_factory_make("queue", nullptr);
            if (!depay || !parser || !queue)
            {
                if (depay)
                {
                    gst_object_unref(depay);
                }
                if (parser)
                {
                    gst_object_unref(parser);
                }
                if (queue)
                {
                    gst_object_unref(queue);
                }
                if (timestamper)
                {
                    gst_object_unref(timestamper);
                }
                throw RecorderError("Required video parser or RTP depayloader is unavailable");
            }
            g_object_set(parser, "config-interval", -1, nullptr);
            gst_bin_add_many(GST_BIN(self.pipeline), depay, parser, queue, nullptr);
            self.queue = queue;
            bool elements_linked;
            if (timestamper)
            {
                gst_bin_add(GST_BIN(self.pipeline), timestamper);
                elements_linked =
                    gst_element_link_many(depay, parser, timestamper, queue, self.sink, nullptr);
            }
            else
            {
                elements_linked = gst_element_link_many(depay, parser, queue, self.sink, nullptr);
            }
            if (!elements_linked)
            {
                throw RecorderError("Cannot link recording pipeline");
            }
            auto *input = gst_element_get_static_pad(depay, "sink");
            auto linked = gst_pad_link(pad, input);
            gst_object_unref(input);
            if (linked != GST_PAD_LINK_OK)
            {
                throw RecorderError("Cannot link camera video stream");
            }
            gst_pad_add_probe(
                pad, GST_PAD_PROBE_TYPE_BUFFER,
                [](GstPad *, GstPadProbeInfo *info, gpointer probe_data) noexcept
                {
                    auto &state = *static_cast<Session *>(probe_data);
                    state.last_activity = monotonic_ms();
                    bool seen = state.seen_buffer.exchange(true);
                    auto *buffer = gst_pad_probe_info_get_buffer(info);
                    if (seen && buffer && GST_BUFFER_FLAG_IS_SET(buffer, GST_BUFFER_FLAG_DISCONT))
                    {
                        try
                        {
                            std::lock_guard lock(state.mutex);
                            state.continuity = new_id();
                            for (auto &[path, active] : state.active)
                            {
                                (void)path;
                                active.info.continuity_id = "discontinuity:" + state.continuity;
                            }
                        }
                        catch (...)
                        {
                            std::lock_guard lock(state.mutex);
                            state.error = std::current_exception();
                        }
                    }
                    return GST_PAD_PROBE_OK;
                },
                &self, nullptr);
            {
                std::lock_guard lock(self.mutex);
                self.codec = name == "H264" ? "h264" : "h265";
            }
            gst_element_sync_state_with_parent(queue);
            if (timestamper)
            {
                gst_element_sync_state_with_parent(timestamper);
            }
            gst_element_sync_state_with_parent(parser);
            gst_element_sync_state_with_parent(depay);
        }
        catch (...)
        {
            std::lock_guard lock(self.mutex);
            self.error = std::current_exception();
        }
    }
    static gchar *format_location(GstElement *, guint, GstSample *sample, gpointer data) noexcept
    {
        auto &self = *static_cast<Session *>(data);
        try
        {
            SegmentInfo info;
            info.segment_id = new_id();
            info.camera_name = self.config.camera.camera_name;
            info.start_epoch_ms = utc_epoch_ms();
            {
                std::lock_guard lock(self.mutex);
                info.video_codec = self.codec;
                info.continuity_id = self.continuity;
            }
            auto directory = self.config.staging_directory / info.camera_name / "video";
            create_directories_durable(directory);
            info.staging_file_path = std::filesystem::absolute(
                directory /
                (info.segment_id +
                 (self.config.container_format == VideoContainerFormat::mp4 ? ".mp4" : ".mkv")));
            auto *buffer = gst_sample_get_buffer(sample);
            const auto *segment = gst_sample_get_segment(sample);
            auto running_time =
                gst_segment_to_running_time(segment, GST_FORMAT_TIME, GST_BUFFER_PTS(buffer));
            if (!GST_CLOCK_TIME_IS_VALID(running_time))
            {
                throw RecorderError("Camera video has no media timestamp");
            }
            info.media_start_seconds = static_cast<double>(running_time) / GST_SECOND;
            self.opened(info);
            {
                std::lock_guard lock(self.mutex);
                self.active.emplace(info.staging_file_path.string(),
                                    ActiveSegment{info, running_time});
            }
            self.recording();
            return g_strdup(info.staging_file_path.c_str());
        }
        catch (...)
        {
            std::lock_guard lock(self.mutex);
            self.error = std::current_exception();
            return g_strdup("/dev/full");
        }
    }
    void handle_element(const GstStructure *structure)
    {
        if (!structure || !gst_structure_has_name(structure, "splitmuxsink-fragment-closed"))
        {
            return;
        }
        const auto *location = gst_structure_get_string(structure, "location");
        guint64 end = 0;
        if (!location || !gst_structure_get_uint64(structure, "running-time", &end))
        {
            throw RecorderError("Closed recording has no media timestamp");
        }
        ActiveSegment segment;
        {
            std::lock_guard lock(mutex);
            auto found = active.find(location);
            if (found == active.end())
            {
                throw RecorderError("Closed recording has no journal identity");
            }
            segment = found->second;
            active.erase(found);
        }
        auto &info = segment.info;
        if (end < segment.start_running_time)
        {
            throw RecorderError("Camera media timestamp discontinuity");
        }
        // A splitmux running-time span can differ from the muxed presentation
        // duration (in particular with reordered H.264 pictures). Publish the
        // finalized container's duration, which playback and recovery also use.
        {
            GError *inspection_error = nullptr;
            auto *discoverer = gst_discoverer_new(
                static_cast<GstClockTime>(config.network_timeout_seconds * GST_SECOND),
                &inspection_error);
            if (!discoverer)
            {
                if (inspection_error)
                {
                    g_error_free(inspection_error);
                }
                throw RecorderError("Cannot inspect finalized recording");
            }
            auto *uri = gst_filename_to_uri(info.staging_file_path.c_str(), nullptr);
            auto *discovered = gst_discoverer_discover_uri(discoverer, uri, &inspection_error);
            g_free(uri);
            const auto media_duration =
                discovered ? gst_discoverer_info_get_duration(discovered) : GST_CLOCK_TIME_NONE;
            if (discovered)
            {
                gst_discoverer_info_unref(discovered);
            }
            gst_object_unref(discoverer);
            if (inspection_error)
            {
                g_error_free(inspection_error);
            }
            if (!GST_CLOCK_TIME_IS_VALID(media_duration) || media_duration == 0)
            {
                throw RecorderError("Finalized recording has no media duration");
            }
            info.duration_seconds = static_cast<double>(media_duration) / GST_SECOND;
        }
        info.end_epoch_ms =
            info.start_epoch_ms + static_cast<std::int64_t>(info.duration_seconds * 1000);
        info.file_size_bytes = std::filesystem::file_size(info.staging_file_path);
        sync_file(info.staging_file_path);
        sync_directory(info.staging_file_path.parent_path());
        closed(info);
    }
};
} // namespace
void record_session(const SegmentRecorderConfig &config, const std::atomic<bool> &stop,
                    const std::atomic<std::int64_t> &deadline,
                    const std::function<void(const SegmentInfo &)> &opened,
                    const std::function<void(const SegmentInfo &)> &closed,
                    const std::function<void()> &recording)
{
    auto session_owner = std::make_shared<Session>(config, opened, closed, recording);
    auto &session = *session_owner;
    struct Cleanup
    {
        std::shared_ptr<Session> session;
        ~Cleanup()
        {
            session->begin_teardown();
        }
    } cleanup{session_owner};
    auto *bus = gst_element_get_bus(session.pipeline);
    std::unique_ptr<GstBus, decltype(&gst_object_unref)> bus_owner(bus, gst_object_unref);
    if (gst_element_set_state(session.pipeline, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE)
    {
        throw RecorderError("Cannot start RTSP pipeline");
    }
    bool eos_sent = false;
    bool disconnected = false;
    auto last_activity = monotonic_ms();
    auto failure_deadline = static_cast<std::int64_t>(0);
    while (true)
    {
        if (!session.check_callback_error(stop.load()))
        {
            break;
        }
        if ((stop || disconnected) && !eos_sent)
        {
            if (!session.queue.load())
            {
                break;
            }
            eos_sent = true;
            session.request_eos();
        }
        if ((stop && monotonic_ms() >= deadline.load()) ||
            (disconnected && monotonic_ms() >= failure_deadline))
        {
            break;
        }
        auto *message = gst_bus_timed_pop(bus, 50 * GST_MSECOND);
        if (message)
        {
            std::unique_ptr<GstMessage, decltype(&gst_message_unref)> owner(message,
                                                                            gst_message_unref);
            if (!session.check_callback_error(stop.load()))
            {
                break;
            }
            if (GST_MESSAGE_TYPE(message) == GST_MESSAGE_ELEMENT)
            {
                auto *structure = gst_message_get_structure(message);
                session.handle_element(structure);
                if (structure &&
                    (gst_structure_has_name(structure, "splitmuxsink-fragment-opened") ||
                     gst_structure_has_name(structure, "splitmuxsink-fragment-closed")))
                {
                    last_activity = monotonic_ms();
                }
            }
            else if (GST_MESSAGE_TYPE(message) == GST_MESSAGE_EOS)
            {
                break;
            }
            else if (GST_MESSAGE_TYPE(message) == GST_MESSAGE_ERROR && !disconnected)
            {
                GError *error = nullptr;
                gst_message_parse_error(message, &error, nullptr);
                const bool transport = error && error->domain == GST_RESOURCE_ERROR &&
                                       (error->code == GST_RESOURCE_ERROR_OPEN_READ ||
                                        error->code == GST_RESOURCE_ERROR_READ ||
                                        error->code == GST_RESOURCE_ERROR_NOT_FOUND ||
                                        error->code == GST_RESOURCE_ERROR_SETTINGS);
                const bool authentication = error && error->domain == GST_RESOURCE_ERROR &&
                                            error->code == GST_RESOURCE_ERROR_NOT_AUTHORIZED;
                if (error)
                {
                    g_error_free(error);
                }
                if (!transport)
                {
                    if (authentication)
                    {
                        throw RecorderError("RTSP authentication failed (unauthorized); check "
                                            "camera username and password");
                    }
                    throw RecorderError(
                        "GStreamer recording failed (plugin, stream or local storage error)");
                }
                disconnected = true;
                failure_deadline = monotonic_ms() +
                                   static_cast<std::int64_t>(config.network_timeout_seconds * 1000);
            }
        }
        if (!stop && !disconnected &&
            monotonic_ms() - std::max(last_activity, session.last_activity.load()) >
                static_cast<std::int64_t>(
                    (config.network_timeout_seconds + config.segment_duration_seconds * 2) * 1000))
        {
            disconnected = true;
            failure_deadline =
                monotonic_ms() + static_cast<std::int64_t>(config.network_timeout_seconds * 1000);
        }
    }
    session.begin_teardown();
    const auto cleanup_deadline =
        stop ? deadline.load()
             : monotonic_ms() + static_cast<std::int64_t>(config.network_timeout_seconds * 1000);
    if (!session.wait_for_teardown(cleanup_deadline))
    {
        throw RecorderError("GStreamer finalization deadline exceeded; camera state remains owned "
                            "until cleanup finishes");
    }
    if (!stop)
    {
        throw CameraConnectionError("Camera stream disconnected or timed out");
    }
}
} // namespace homesight::detail
