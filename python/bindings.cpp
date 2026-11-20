#include "homesight/clip_frame_sampler.hpp"
#include "homesight/errors.hpp"
#include "homesight/segment_motion_analyzer.hpp"
#include "homesight/segment_recorder.hpp"
#include "homesight/segment_storage.hpp"
#include <pybind11/numpy.h>
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>
#include <pybind11/stl/filesystem.h>

namespace py = pybind11;
using namespace homesight;
namespace
{
template <class T>
py::class_<T> value_type(py::module_ &module, const char *name, const char *documentation)
{
    auto type = py::class_<T>(module, name, documentation);
    type.attr("__fields__") = py::list();
    type.def(
        py::init(
            [](py::kwargs kwargs)
            {
                auto result = std::make_unique<T>();
                auto object = py::cast(result.get(), py::return_value_policy::reference);
                auto fields = py::type::of<T>().attr("__fields__");
                for (auto item : kwargs)
                {
                    auto key = py::cast<std::string>(item.first);
                    if (!fields.contains(item.first))
                    {
                        throw py::type_error("Unknown configuration field: " + key);
                    }
                    auto value = py::reinterpret_borrow<py::object>(item.second);
                    auto current = object.attr(key.c_str());
                    if constexpr (std::is_same_v<T, SegmentRecorderConfig>)
                    {
                        if (key == "storage" && py::isinstance<py::dict>(value) &&
                            value.contains("bucket_name"))
                        {
                            current = py::type::of<S3StorageConfig>()();
                        }
                    }
                    if constexpr (std::is_same_v<T, CompletedSegment>)
                    {
                        if (key == "motion" && py::isinstance<py::dict>(value))
                        {
                            current = py::type::of<MotionAnalysisResult>()();
                        }
                    }
                    if (py::isinstance<py::dict>(value) &&
                        py::hasattr(py::type::of(current), "from_dict"))
                    {
                        value = py::type::of(current).attr("from_dict")(value);
                    }
                    object.attr(key.c_str()) = value;
                }
                return result;
            }),
        "Construct from keyword fields or nested mappings. Unknown fields raise TypeError. Missing "
        "fields use native defaults; configuration validation occurs when settings are used.");
    type.def(
        "to_dict",
        [](const T &value)
        {
            py::dict result;
            auto object = py::cast(&value, py::return_value_policy::reference);
            for (auto key : py::type::of<T>().attr("__fields__"))
            {
                auto field = py::cast<std::string>(key);
                if (field == "credentials" || field == "access_key_id" ||
                    field == "secret_access_key" || field == "session_token" || field == "rtsp_url")
                {
                    continue;
                }
                auto item = object.attr(field.c_str());
                result[py::str(field)] =
                    py::hasattr(item, "to_dict") ? item.attr("to_dict")() : py::object(item);
            }
            return result;
        },
        "Return an owned mapping, recursively converting nested values. Camera URLs, credentials, "
        "access/signing keys, and session tokens are omitted; this is not a lossless configuration "
        "backup.");
    type.def_static(
        "from_dict",
        [](py::dict values)
        {
            return py::type::of<T>()(**values);
        },
        "Construct from a mapping passed positionally; nested mappings are supported. Unknown keys "
        "raise TypeError. Missing fields retain their native defaults; validation occurs when "
        "settings are used.");
    return type;
}
template <class T, class Field>
py::class_<T> &field(py::class_<T> &type, const char *name, Field T::*member,
                     const char *documentation)
{
    type.def_readwrite(name, member, documentation);
    type.attr("__fields__").attr("append")(name);
    return type;
}
py::array frames_array(SampledClipFrames frames)
{
    auto pixels = std::make_unique<std::vector<std::uint8_t>>(std::move(frames.rgb_pixels));
    py::capsule owner(pixels.get(),
                      [](void *pointer)
                      {
                          delete static_cast<std::vector<std::uint8_t> *>(pointer);
                      });
    auto *data = pixels.release();
    return py::array_t<std::uint8_t>(
        {frames.frame_count, frames.frame_height, frames.frame_width, 3},
        {static_cast<py::ssize_t>(frames.frame_height) * frames.frame_width * 3,
         static_cast<py::ssize_t>(frames.frame_width) * 3, static_cast<py::ssize_t>(3),
         static_cast<py::ssize_t>(1)},
        data->data(), owner);
}
} // namespace
#define VALUE(TYPE, DOC) auto TYPE##_type = value_type<TYPE>(m, #TYPE, DOC)
#define FIELD(TYPE, NAME, DOC) field(TYPE##_type, #NAME, &TYPE::NAME, DOC)
#define READ(TYPE, NAME, DOC) .def_readonly(#NAME, &TYPE::NAME, DOC)
PYBIND11_MODULE(_native, m)
{
    m.doc() = "Native durable camera recording and motion analysis";
    m.attr("__version__") = "0.1.0";
    auto base = py::register_exception<RecorderError>(m, "RecorderError");
    base.attr("__doc__") = "Base exception for recorder, journal, media, and storage operations.";
    py::register_exception<InvalidConfigError>(m, "InvalidConfigError", base.ptr())
        .attr("__doc__") =
        "A supplied configuration or operation argument violates its supported bounds.";
    py::register_exception<SegmentDecodingError>(m, "SegmentDecodingError", base.ptr())
        .attr("__doc__") = "A recording cannot be decoded or sampled within its configured budget.";
    py::register_exception<SegmentStorageError>(m, "SegmentStorageError", base.ptr())
        .attr("__doc__") = "A local or S3-compatible storage operation could not be completed.";
    py::register_exception<JournalFullError>(m, "JournalFullError", base.ptr()).attr("__doc__") =
        "The separate durable journal allowance is exhausted; committed events are preserved.";
    py::enum_<VideoContainerFormat>(
        m, "VideoContainerFormat",
        "Container used to finalize encoded camera video without re-encoding.")
        .value("mp4", VideoContainerFormat::mp4,
               "Finalize each segment as MP4; an interrupted unfinished file may be unplayable.")
        .value("matroska", VideoContainerFormat::matroska,
               "Finalize as Matroska, allowing better recovery of interrupted video.");
    py::enum_<RtspTransport>(m, "RtspTransport", "RTP transport selected for the RTSP connection.")
        .value("tcp", RtspTransport::tcp, "Interleave RTP over TCP (the recorder default).")
        .value("udp", RtspTransport::udp, "Receive RTP over UDP.");
    py::enum_<MotionAnalysisStatus>(
        m, "MotionAnalysisStatus",
        "Whether a motion result is trustworthy enough to contain a boolean decision.")
        .value("valid", MotionAnalysisStatus::valid,
               "Complete, warmed-up analysis; has_motion contains true or false.")
        .value("warming_up", MotionAnalysisStatus::warming_up,
               "Coverage includes model warm-up; the entire clip remains unknown.")
        .value("insufficient_data", MotionAnalysisStatus::insufficient_data,
               "Too few samples or incomplete/discontinuous presentation coverage; decision is "
               "unknown.")
        .value("failed", MotionAnalysisStatus::failed,
               "Decoding or analysis failed; error describes the failure and the model resets.");
    py::enum_<SegmentStorageOutcome>(
        m, "SegmentStorageOutcome",
        "Durable storage or retention state of a completed segment revision.")
        .value("pending", SegmentStorageOutcome::pending,
               "Analysis completed and storage work is pending.")
        .value("stored", SegmentStorageOutcome::stored,
               "The archive or object was committed; storage_uri identifies it.")
        .value("storage_failed", SegmentStorageOutcome::storage_failed,
               "A storage attempt failed; durable recorder retry remains pending.")
        .value("deleted_without_motion", SegmentStorageOutcome::deleted_without_motion,
               "A valid negative motion result authorized local deletion.")
        .value("deleted_by_quota", SegmentStorageOutcome::deleted_by_quota,
               "The completed local video was evicted by the rolling allowance.")
        .value("interrupted", SegmentStorageOutcome::interrupted,
               "An unfinished recording could not be finalized for normal processing.");
    py::enum_<RecorderEventKind>(
        m, "RecorderEventKind",
        "Reason a durable completed-segment snapshot was appended to the journal.")
        .value("segment_completed", RecorderEventKind::segment_completed,
               "Analysis/retention completed; storage may still be pending.")
        .value("storage_changed", RecorderEventKind::storage_changed,
               "Storage state or its durable outcome changed.")
        .value("deleted_by_quota", RecorderEventKind::deleted_by_quota,
               "A local video was evicted; its uploaded object may still exist.");
    py::enum_<RecorderState>(
        m, "RecorderState",
        "Recorder lifecycle and connection state, independent of pending durable work.")
        .value("stopped", RecorderState::stopped,
               "Recorder workers are stopped; durable work may remain.")
        .value("connecting", RecorderState::connecting, "Establishing an RTSP connection.")
        .value("recording", RecorderState::recording, "Receiving camera video.")
        .value("reconnecting", RecorderState::reconnecting,
               "Waiting or retrying after a transport failure.")
        .value("stopping", RecorderState::stopping,
               "Finalizing the active recording and joining workers.")
        .value("failed", RecorderState::failed,
               "A terminal recording/journal/shutdown error occurred; inspect last_error.");
    VALUE(CameraStreamConfig,
          "Connection settings for one H.264/H.265 camera; credentials remain application-owned.");
    FIELD(CameraStreamConfig, camera_name,
          "Stable camera identifier: 1–128 lowercase letters, digits, or underscores.");
    FIELD(CameraStreamConfig, rtsp_url,
          "RTSP or RTSPS URL, optionally containing camera credentials; omitted by to_dict().");
    FIELD(CameraStreamConfig, rtsp_transport, "RTP transport; defaults to TCP.");
    FIELD(CameraStreamConfig, rtsp_latency_ms,
          "Nonnegative RTSP jitter-buffer latency in milliseconds; default 200.");
    VALUE(MotionAnalysisParams,
          "CPU MOG2 sampling, warm-up, and decision parameters, validated when analysis starts.");
    FIELD(MotionAnalysisParams, sample_rate_fps,
          "Analysis samples per media second, greater than zero and at most 240; default 2.");
    FIELD(MotionAnalysisParams, analysis_frame_width,
          "Resized analysis width in pixels, from 8 to 8192; aspect ratio is preserved; default "
          "320.");
    FIELD(MotionAnalysisParams, warmup_seconds,
          "Nonnegative background-model coverage required before a valid decision; default 30 "
          "seconds.");
    FIELD(MotionAnalysisParams, history_frames,
          "MOG2 background history length, at least 2 sampled frames; default 120.");
    FIELD(MotionAnalysisParams, variance_threshold,
          "Positive MOG2 variance threshold; default 16.");
    FIELD(MotionAnalysisParams, learning_rate,
          "MOG2 learning rate from -1 to 1; -1 selects automatic learning (default).");
    FIELD(MotionAnalysisParams, changed_area_threshold,
          "Foreground fraction required for a changed frame, greater than zero and at most 1; "
          "default 0.01.");
    FIELD(MotionAnalysisParams, minimum_frames_with_motion,
          "Minimum changed samples required for motion, at least 1; default 2.");
    FIELD(MotionAnalysisParams, exclude_shadows,
          "Exclude MOG2 shadow labels from changed foreground; default true.");
    FIELD(MotionAnalysisParams, use_gpu_decoding,
          "Allow ranked hardware decoders with CPU fallback; false by default; does not guarantee "
          "GPU selection.");
    FIELD(MotionAnalysisParams, decode_timeout_seconds,
          "Positive wall-time limit for decoding one segment, at most 86400 seconds; default 10.");
    VALUE(LocalDirectoryStorageConfig,
          "Local archive destination; recordings are organized by camera and UTC date.");
    FIELD(LocalDirectoryStorageConfig, root_directory,
          "Required archive root on a local filesystem; files are stored below camera/date "
          "subdirectories.");
    VALUE(S3Credentials,
          "Explicit SigV4 credentials; empty values permit AWS environment credential fallback.");
    FIELD(S3Credentials, access_key_id, "AWS access key identifier; omitted by to_dict().");
    FIELD(S3Credentials, secret_access_key, "Secret signing key; omitted by to_dict().");
    FIELD(S3Credentials, session_token,
          "Optional temporary-credential session token; omitted by to_dict().");
    VALUE(S3StorageConfig, "S3-compatible single-PUT storage and bounded retry settings.");
    FIELD(S3StorageConfig, bucket_name, "Required bucket name; the bucket must already exist.");
    FIELD(S3StorageConfig, key_prefix,
          "Optional object-key prefix before camera/date/segment identity.");
    FIELD(S3StorageConfig, region, "SigV4 signing region; default us-east-1.");
    FIELD(S3StorageConfig, endpoint_url,
          "Optional HTTP(S) origin for compatible storage; empty selects the regional AWS S3 "
          "endpoint.");
    FIELD(S3StorageConfig, use_path_style_addressing,
          "Put the bucket in the request path instead of the hostname; default false.");
    FIELD(S3StorageConfig, verify_tls_certificates,
          "Verify HTTPS certificates and hostnames; default true.");
    FIELD(S3StorageConfig, credentials,
          "Explicit or environment-backed credentials; the entire field is omitted by to_dict().");
    FIELD(S3StorageConfig, delete_staging_file_after_upload,
          "Release staging after the recorder durably commits upload success; default true.");
    FIELD(S3StorageConfig, request_timeout_seconds,
          "Positive timeout for one HTTP request in seconds; default 10.");
    FIELD(S3StorageConfig, retry_initial_delay_seconds,
          "Positive initial recorder upload retry delay in seconds; default 1.");
    FIELD(S3StorageConfig, retry_max_delay_seconds,
          "Maximum exponential retry delay, at least the initial delay; default 60 seconds.");
    VALUE(SegmentRecorderConfig,
          "Per-camera recording, analysis, durable journal, quota, and storage settings.");
    FIELD(SegmentRecorderConfig, camera, "Required camera identity and RTSP connection settings.");
    FIELD(SegmentRecorderConfig, storage,
          "Local-directory or S3-compatible storage configuration.");
    FIELD(SegmentRecorderConfig, staging_directory,
          "Required durable local root for each camera journal and pending video; keep stable "
          "across restarts.");
    FIELD(
        SegmentRecorderConfig, segment_duration_seconds,
        "Positive target duration, at most 86400 seconds; splits occur at keyframes; default 10.");
    FIELD(SegmentRecorderConfig, container_format, "Output video container; default MP4.");
    FIELD(SegmentRecorderConfig, motion_analysis,
          "Stateful motion-analysis parameters, including unknown-result warm-up.");
    FIELD(SegmentRecorderConfig, analyze_motion_after_segment_closes,
          "Analyze finalized clips on the analysis worker; default true.");
    FIELD(SegmentRecorderConfig, delete_segments_without_motion,
          "Delete only valid negative analyses; requires analysis enabled; default false.");
    FIELD(SegmentRecorderConfig, reconnect_initial_delay_seconds,
          "Positive initial RTSP reconnect delay in seconds; default 1.");
    FIELD(SegmentRecorderConfig, reconnect_max_delay_seconds,
          "Maximum reconnect backoff, at least the initial delay; default 30 seconds.");
    FIELD(SegmentRecorderConfig, network_timeout_seconds,
          "Connection/inactivity operation timeout in seconds, from 0.001 to 86400; default 10.");
    FIELD(SegmentRecorderConfig, local_video_allowance_bytes,
          "Positive completed-local-video budget, including pending uploads; default 2 GiB; active "
          "video may exceed it.");
    FIELD(SegmentRecorderConfig, journal_allowance_bytes,
          "Separate durable journal budget, at least 1 MiB; default 64 MiB; exhaustion fails "
          "explicitly.");
    VALUE(SegmentInfo, "Stable identity, host UTC bounds, media timing, and original staging "
                       "metadata of a video.");
    FIELD(SegmentInfo, segment_id,
          "Stable random identity independent of wall-clock time; reused for idempotent storage "
          "keys.");
    FIELD(SegmentInfo, camera_name, "Camera identifier associated with the recording.");
    FIELD(
        SegmentInfo, staging_file_path,
        "Original staging path; may no longer exist after storage cleanup or retention deletion.");
    FIELD(SegmentInfo, start_epoch_ms,
          "Recorder-host UTC start time in milliseconds since the Unix epoch.");
    FIELD(SegmentInfo, end_epoch_ms,
          "Host start plus finalized media duration, in Unix milliseconds; interrupted unreadable "
          "video may have zero duration.");
    FIELD(SegmentInfo, duration_seconds,
          "Finalized container presentation duration in seconds, not the configured split target.");
    FIELD(SegmentInfo, video_codec, "Recorded video codec identifier: h264 or h265.");
    FIELD(SegmentInfo, file_size_bytes,
          "Size recorded when video was finalized; retained even if the local file is later "
          "removed.");
    FIELD(SegmentInfo, continuity_id,
          "Recording-session/discontinuity identity used to reset the background model.");
    FIELD(SegmentInfo, media_start_seconds,
          "Segment start on the recording session running-time clock, in seconds.");
    VALUE(MotionAnalysisResult,
          "Motion decision and diagnostics; only valid results contain a boolean has_motion.");
    FIELD(MotionAnalysisResult, status, "Decision validity; defaults to insufficient_data.");
    FIELD(MotionAnalysisResult, has_motion,
          "Boolean only for valid analysis; empty for warm-up, incomplete data, and failure.");
    FIELD(
        MotionAnalysisResult, peak_changed_area,
        "Largest sampled foreground fraction, from 0 to 1; interpret only with a reliable status.");
    FIELD(MotionAnalysisResult, mean_changed_area,
          "Mean sampled foreground fraction for completed analysis; unknown statuses may have "
          "partial diagnostics.");
    FIELD(MotionAnalysisResult, frames_with_motion,
          "Number of analyzed samples meeting changed_area_threshold.");
    FIELD(MotionAnalysisResult, frames_analyzed,
          "Number of sampled frames processed, not the full decoded frame count.");
    FIELD(MotionAnalysisResult, params_used, "Copy of the parameters applied to this analysis.");
    FIELD(MotionAnalysisResult, error,
          "Optional decoding or coverage diagnostic; empty when no diagnostic was recorded.");
    VALUE(CompletedSegment,
          "Snapshot of one segment revision, including motion, storage, and errors.");
    FIELD(CompletedSegment, segment, "Original video identity and metadata.");
    FIELD(CompletedSegment, motion,
          "Optional analysis result; absent if analysis was disabled or not completed.");
    FIELD(CompletedSegment, motion_analysis_error,
          "Optional analysis diagnostic copied into the recorder snapshot.");
    FIELD(CompletedSegment, storage_outcome,
          "Storage/retention state for this revision; initially pending.");
    FIELD(CompletedSegment, storage_uri,
          "Optional file or S3 URI; uploaded object URIs survive local quota eviction.");
    FIELD(CompletedSegment, storage_error,
          "Optional storage/recovery diagnostic; a successful upload clears storage failure.");
    FIELD(CompletedSegment, state_revision,
          "Per-segment revision incremented when a durable event snapshot is committed.");
    py::class_<RecorderEvent>(
        m, "RecorderEvent",
        "Unacknowledged journal event; apply revisions idempotently before acknowledging its ID.")
        READ(RecorderEvent, event_id, "Stable unique event identity used for acknowledgement.")
            READ(RecorderEvent, sequence,
                 "Monotonically increasing journal sequence for this camera.")
                READ(RecorderEvent, kind, "Lifecycle transition represented by the snapshot.") READ(
                    RecorderEvent, completed_segment,
                    "Owned segment revision snapshot; later events may contain newer revisions.");
    py::class_<RecorderStatistics>(
        m, "RecorderStatistics",
        "Read-only operational snapshot; counters accumulate over the recorder instance lifetime.")
        READ(RecorderStatistics, state, "Current recorder lifecycle state.") READ(
            RecorderStatistics, segments_written,
            "Finalized video count observed by this recorder instance.")
            READ(RecorderStatistics, segments_analyzed,
                 "Count of finalized clips submitted to motion analysis, including unknown "
                 "results.") READ(RecorderStatistics, segments_deleted_without_motion,
                                  "Count of deletions authorized by valid negative analyses.")
                READ(RecorderStatistics, segments_stored,
                     "Count of successful local publications or S3 uploads in this instance.")
                    READ(RecorderStatistics, segments_waiting_for_upload,
                         "Current journal-backed pending S3 work count; zero for local storage.")
                        READ(RecorderStatistics, storage_failures,
                             "Failed storage attempt count, including repeated retries.")
                            READ(RecorderStatistics, reconnect_attempts,
                                 "Number of reconnect cycles after transport failure.")
                                READ(RecorderStatistics, local_video_bytes,
                                     "Last completed-video quota accounting snapshot in bytes; "
                                     "excludes active video.")
                                    READ(RecorderStatistics, last_segment_closed_epoch_ms,
                                         "Most recently observed finalized segment end time in "
                                         "Unix milliseconds, or zero.")
                                        READ(RecorderStatistics, last_error,
                                             "Optional most recent recorder diagnostic; it can "
                                             "describe a reconnect as well as a terminal error.");
    py::class_<SegmentMotionAnalyzer>(
        m, "SegmentMotionAnalyzer",
        "Maintain one per-camera MOG2 background model across chronological clips. Calls and reset "
        "are serialized. Continuity, resolution, and media-time discontinuities reset warm-up; "
        "only valid results contain a boolean decision.")
        .def(py::init<MotionAnalysisParams>(), py::arg("params") = MotionAnalysisParams{},
             "Create a validated independent background model. Invalid motion settings raise "
             "InvalidConfigError.")
        .def("reset", &SegmentMotionAnalyzer::reset, py::call_guard<py::gil_scoped_release>(),
             "Discard background history, resolution, continuity identity, and warm-up coverage.")
        .def("analyze_segment_motion", &SegmentMotionAnalyzer::analyze_segment_motion,
             py::arg("segment_file_path"), py::arg("continuity_id") = "",
             py::arg("start_media_seconds") = -1, py::call_guard<py::gil_scoped_release>(),
             "Analyze a complete recording without modifying it. The module helper starts a fresh "
             "model; the analyzer method retains history. For the method, continuity_id changes "
             "reset the model and start_media_seconds supplies session media time; a negative "
             "start disables cross-clip time checks. Clips overlapping warm-up remain unknown. "
             "Decode/coverage failures reset the model and return diagnostics; invalid parameters "
             "raise InvalidConfigError.");
    m.def("analyze_segment_motion", &analyze_segment_motion, py::arg("segment_file_path"),
          py::arg("params") = MotionAnalysisParams{}, py::call_guard<py::gil_scoped_release>(),
          "Analyze a complete recording without modifying it. The module helper starts a fresh "
          "model; the analyzer method retains history. For the method, continuity_id changes reset "
          "the model and start_media_seconds supplies session media time; a negative start "
          "disables cross-clip time checks. Clips overlapping warm-up remain unknown. "
          "Decode/coverage failures reset the model and return diagnostics; invalid parameters "
          "raise InvalidConfigError.");
    m.def(
        "sample_clip_frames_from_segment",
        [](const std::filesystem::path &path, int count, int width, int height, bool gpu,
           double timeout)
        {
            SampledClipFrames frames;
            {
                py::gil_scoped_release release;
                frames = sample_clip_frames_from_segment(path, count, width, height, gpu, timeout);
            }
            return frames_array(std::move(frames));
        },
        py::arg("segment_file_path"), py::arg("frame_count"), py::arg("frame_width"),
        py::arg("frame_height"), py::arg("use_gpu_decoding") = false,
        py::arg("decode_timeout_seconds") = 10,
        "Sample evenly spaced presentation times, center-crop, and resize to owned RGB "
        "pixels.\n\nArgs:\n    segment_file_path: Existing recording supported by installed "
        "decoders.\n    frame_count: Number of frames from 1 to 4096.\n    frame_width: Output "
        "width from 1 to 8192 pixels.\n    frame_height: Output height from 1 to 8192 pixels.\n    "
        "use_gpu_decoding: Allow ranked hardware decoders with CPU fallback; default False.\n    "
        "decode_timeout_seconds: Positive wall-time budget up to 86400 seconds; default "
        "10.\n\nReturns:\n    Contiguous uint8 NumPy array of shape (frame_count, frame_height, "
        "frame_width, 3). An owning capsule keeps its RGB bytes valid independently of native "
        "temporaries; allocation is limited to 1 GiB.\n\nRaises:\n    InvalidConfigError: Count, "
        "dimensions, timeout, or allocation bounds are invalid.\n    SegmentDecodingError: "
        "Decode/seek fails or exceeds its budget.");
    py::class_<SegmentStorage>(m, "SegmentStorage",
                               "Synchronous local/S3 backend returned by create_segment_storage(). "
                               "Standalone callers coordinate file lifetimes; recorder workers add "
                               "durable intents, retry, and cleanup handling.")
        .def("store_segment_file", &SegmentStorage::store_segment_file, py::arg("completed"),
             py::call_guard<py::gil_scoped_release>(),
             "Publish the full snapshot under an idempotent key and return its file/S3 URI. Local "
             "publication can move staging; standalone S3 publication retains it. Failed "
             "operations raise SegmentStorageError without a standalone retry loop.")
        .def("find_stored_segment", &SegmentStorage::find_stored_segment, py::arg("completed"),
             py::call_guard<py::gil_scoped_release>(),
             "Probe the stable archive/object key after uncertain publication. Return its URI or "
             "None for definite absence. Probe failures raise SegmentStorageError rather than "
             "reporting absence.")
        .def("delete_stored_segment", &SegmentStorage::delete_stored_segment,
             py::arg("storage_uri"), py::call_guard<py::gil_scoped_release>(),
             "Delete a backend-owned recording. Failed deletion or an escaping URI raises "
             "SegmentStorageError.")
        .def("download_segment_to_local_file", &SegmentStorage::download_segment_to_local_file,
             py::arg("storage_uri"), py::arg("destination"),
             py::call_guard<py::gil_scoped_release>(),
             "Download/copy to destination using an exclusive temporary and durable publication. "
             "Unrelated sibling files are preserved. Invalid URIs, transfer failures, or "
             "publication failures raise SegmentStorageError.");
    m.def("create_segment_storage", &create_segment_storage, py::arg("config"),
          py::call_guard<py::gil_scoped_release>(),
          "Create local-directory or S3-compatible storage without opening a camera. S3 uses "
          "explicit/AWS environment credentials and single PUTs. Invalid settings raise "
          "InvalidConfigError; unavailable backends raise SegmentStorageError.");
    py::class_<SegmentRecorder>(
        m, "SegmentRecorder",
        "Own a camera journal and its recording, analysis, and storage workers. The camera lock "
        "lasts until destruction, including while stopped. Apply durable revisions idempotently "
        "before acknowledging events. Coordinate consumers sharing delivery cursors.",
        py::release_gil_before_calling_cpp_dtor())
        .def(py::init<SegmentRecorderConfig>(), py::arg("config"),
             py::call_guard<py::gil_scoped_release>(),
             "Validate settings, open the journal, and claim the camera lock without starting "
             "recording. Invalid settings raise InvalidConfigError; ownership or journal failures "
             "raise RecorderError; storage setup can raise SegmentStorageError.")
        .def("start_recording", &SegmentRecorder::start_recording,
             py::call_guard<py::gil_scoped_release>(),
             "Reconcile durable unfinished work and launch workers. The RTSP connection is "
             "asynchronous; inspect statistics for failures. Already-started, pending-cleanup, or "
             "failed recovery conditions raise RecorderError.")
        .def(
            "stop_recording",
            [](SegmentRecorder &self, long long timeout)
            {
                self.stop_recording(std::chrono::milliseconds(timeout));
            },
            py::arg("finalize_timeout_ms") = 10000, py::call_guard<py::gil_scoped_release>(),
            "Finalize the active clip and join workers without draining the backlog. "
            "finalize_timeout_ms is from 0 to 86400000 (default 10000); current decode/upload "
            "operations can add their timeout. Shutdown failure is recorded in statistics. Invalid "
            "bounds raise InvalidConfigError.")
        .def("is_recording", &SegmentRecorder::is_recording,
             "Return True while connecting, recording, or reconnecting; False while stopped, "
             "stopping, or failed.")
        .def("get_config", &SegmentRecorder::get_config,
             "Return an independent settings copy, including supplied credentials. Use to_dict() "
             "for redacted output.")
        .def("get_recorder_statistics", &SegmentRecorder::get_recorder_statistics,
             py::call_guard<py::gil_scoped_release>(),
             "Return owned statistics, instance-lifetime counters, and current pending S3 work. "
             "last_error can describe a reconnect as well as terminal failure.")
        .def("take_recorder_events", &SegmentRecorder::take_recorder_events,
             py::arg("max_count") = 1024, py::call_guard<py::gil_scoped_release>(),
             "Return up to max_count (default 1024) journal events ordered by sequence. Advances "
             "the shared event delivery cursor without acknowledging. Unacknowledged event IDs "
             "replay in a new recorder instance; journal failures raise RecorderError.")
        .def("acknowledge_recorder_events", &SegmentRecorder::acknowledge_recorder_events,
             py::arg("event_ids"), py::call_guard<py::gil_scoped_release>(),
             "Durably acknowledge event_ids after application persistence. Acknowledgement is "
             "idempotent. Journal failures raise RecorderError.")
        .def(
            "wait_for_next_recorder_event",
            [](SegmentRecorder &self, long long timeout)
            {
                return self.wait_for_next_recorder_event(std::chrono::milliseconds(timeout));
            },
            py::arg("timeout_ms"), py::call_guard<py::gil_scoped_release>(),
            "Return the next owned event or None on timeout. timeout_ms is from 0 to 86400000; "
            "invalid bounds raise InvalidConfigError. Delivery advances the event cursor without "
            "acknowledgement.")
        .def(
            "take_completed_segments", &SegmentRecorder::take_completed_segments,
            py::arg("max_count") = 1024, py::call_guard<py::gil_scoped_release>(),
            "Read up to max_count (default 1024) compatibility outcomes, using a separate delivery "
            "cursor. Excludes pending storage and quota-only notifications; later storage "
            "revisions can update previously seen segments. Reading does not acknowledge events.")
        .def(
            "wait_for_next_completed_segment",
            [](SegmentRecorder &self, long long timeout)
            {
                return self.wait_for_next_completed_segment(std::chrono::milliseconds(timeout));
            },
            py::arg("timeout_ms"), py::call_guard<py::gil_scoped_release>(),
            "Return a compatibility outcome or None on timeout. timeout_ms is from 0 to 86400000; "
            "invalid bounds raise InvalidConfigError. Reading does not acknowledge durable events.")
        .def(
            "__enter__",
            [](SegmentRecorder &self) -> SegmentRecorder &
            {
                self.start_recording();
                return self;
            },
            py::return_value_policy::reference_internal, py::call_guard<py::gil_scoped_release>(),
            "Start recording and return this recorder for context-manager use.")
        .def(
            "__exit__",
            [](SegmentRecorder &self, py::object, py::object, py::object)
            {
                py::gil_scoped_release release;
                self.stop_recording();
            },
            "Stop on context exit while allowing body exceptions to propagate. Camera journal "
            "ownership lasts until destruction.");
}
