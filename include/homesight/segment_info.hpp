#pragma once
/** @file segment_info.hpp
 * Public recording, motion, event, and frame value types.
 */
#include "homesight/recorder_config.hpp"
#include <optional>
#include <vector>
namespace homesight
{
/// Whether a motion result is trustworthy enough to contain a boolean decision.
enum class MotionAnalysisStatus
{
    /// Complete, warmed-up analysis; has_motion contains true or false.
    valid,
    /// Coverage includes model warm-up; the entire clip remains unknown.
    warming_up,
    /// Too few samples or incomplete/discontinuous presentation coverage; decision is unknown.
    insufficient_data,
    /// Decoding or analysis failed; error describes the failure and the model resets.
    failed
};
/// Motion decision and diagnostics; only valid results contain a boolean has_motion.
struct MotionAnalysisResult
{
    /// Decision validity; defaults to insufficient_data.
    MotionAnalysisStatus status = MotionAnalysisStatus::insufficient_data;
    /// Boolean only for valid analysis; empty for warm-up, incomplete data, and failure.
    std::optional<bool> has_motion;
    /// Largest sampled foreground fraction, from 0 to 1; interpret only with a reliable status.
    double peak_changed_area = 0;
    /// Mean sampled foreground fraction for completed analysis; unknown statuses may have partial
    /// diagnostics.
    double mean_changed_area = 0;
    /// Number of analyzed samples meeting changed_area_threshold.
    int frames_with_motion = 0;
    /// Number of sampled frames processed, not the full decoded frame count.
    int frames_analyzed = 0;
    /// Copy of the parameters applied to this analysis.
    MotionAnalysisParams params_used;
    /// Optional decoding or coverage diagnostic; empty when no diagnostic was recorded.
    std::optional<std::string> error;
};
/// Stable identity, host UTC bounds, media timing, and original staging metadata of a video.
struct SegmentInfo
{
    /// Stable random identity independent of wall-clock time; reused for idempotent storage keys.
    std::string segment_id;
    /// Camera identifier associated with the recording.
    std::string camera_name;
    /// Original staging path; may no longer exist after storage cleanup or retention deletion.
    std::filesystem::path staging_file_path;
    /// Recorder-host UTC start time in milliseconds since the Unix epoch.
    std::int64_t start_epoch_ms = 0;
    /// Host start plus finalized media duration, in Unix milliseconds; interrupted unreadable video
    /// may have zero duration.
    std::int64_t end_epoch_ms = 0;
    /// Finalized container presentation duration in seconds, not the configured split target.
    double duration_seconds = 0;
    /// Segment start on the recording session running-time clock, in seconds.
    double media_start_seconds = 0;
    /// Recorded video codec identifier: h264 or h265.
    std::string video_codec;
    /// Size recorded when video was finalized; retained even if the local file is later removed.
    std::uint64_t file_size_bytes = 0;
    /// Recording-session/discontinuity identity used to reset the background model.
    std::string continuity_id;
};
/// Durable storage or retention state of a completed segment revision.
enum class SegmentStorageOutcome
{
    /// Analysis completed and storage work is pending.
    pending,
    /// The archive or object was committed; storage_uri identifies it.
    stored,
    /// A storage attempt failed; durable recorder retry remains pending.
    storage_failed,
    /// A valid negative motion result authorized local deletion.
    deleted_without_motion,
    /// The completed local video was evicted by the rolling allowance.
    deleted_by_quota,
    /// An unfinished recording could not be finalized for normal processing.
    interrupted
};
/// Snapshot of one segment revision, including motion, storage, and errors.
struct CompletedSegment
{
    /// Original video identity and metadata.
    SegmentInfo segment;
    /// Optional analysis result; absent if analysis was disabled or not completed.
    std::optional<MotionAnalysisResult> motion;
    /// Optional analysis diagnostic copied into the recorder snapshot.
    std::optional<std::string> motion_analysis_error;
    /// Storage/retention state for this revision; initially pending.
    SegmentStorageOutcome storage_outcome = SegmentStorageOutcome::pending;
    /// Optional file or S3 URI; uploaded object URIs survive local quota eviction.
    std::optional<std::string> storage_uri;
    /// Optional storage/recovery diagnostic; a successful upload clears storage failure.
    std::optional<std::string> storage_error;
    /// Per-segment revision incremented when a durable event snapshot is committed.
    std::uint64_t state_revision = 0;
};
/// Reason a durable completed-segment snapshot was appended to the journal.
enum class RecorderEventKind
{
    /// Analysis/retention completed; storage may still be pending.
    segment_completed,
    /// Storage state or its durable outcome changed.
    storage_changed,
    /// A local video was evicted; its uploaded object may still exist.
    deleted_by_quota
};
/// Unacknowledged journal event; apply revisions idempotently before acknowledging its ID.
struct RecorderEvent
{
    /// Stable unique event identity used for acknowledgement.
    std::string event_id;
    /// Monotonically increasing journal sequence for this camera.
    std::uint64_t sequence = 0;
    /// Lifecycle transition represented by the snapshot.
    RecorderEventKind kind = RecorderEventKind::segment_completed;
    /// Owned segment revision snapshot; later events may contain newer revisions.
    CompletedSegment completed_segment;
};
/// Recorder lifecycle and connection state, independent of pending durable work.
enum class RecorderState
{
    /// Recorder workers are stopped; durable work may remain.
    stopped,
    /// Establishing an RTSP connection.
    connecting,
    /// Receiving camera video.
    recording,
    /// Waiting or retrying after a transport failure.
    reconnecting,
    /// Finalizing the active recording and joining workers.
    stopping,
    /// A terminal recording/journal/shutdown error occurred; inspect last_error.
    failed
};
/// Read-only operational snapshot; counters accumulate over the recorder instance lifetime.
struct RecorderStatistics
{
    /// Current recorder lifecycle state.
    RecorderState state = RecorderState::stopped;
    /// Finalized video count observed by this recorder instance.
    std::uint64_t segments_written = 0;
    /// Count of finalized clips submitted to motion analysis, including unknown results.
    std::uint64_t segments_analyzed = 0;
    /// Count of deletions authorized by valid negative analyses.
    std::uint64_t segments_deleted_without_motion = 0;
    /// Count of successful local publications or S3 uploads in this instance.
    std::uint64_t segments_stored = 0;
    /// Current journal-backed pending S3 work count; zero for local storage.
    std::uint64_t segments_waiting_for_upload = 0;
    /// Failed storage attempt count, including repeated retries.
    std::uint64_t storage_failures = 0;
    /// Number of reconnect cycles after transport failure.
    std::uint64_t reconnect_attempts = 0;
    /// Last completed-video quota accounting snapshot in bytes; excludes active video.
    std::uint64_t local_video_bytes = 0;
    /// Most recently observed finalized segment end time in Unix milliseconds, or zero.
    std::int64_t last_segment_closed_epoch_ms = 0;
    /// Optional most recent recorder diagnostic; it can describe a reconnect as well as a terminal
    /// error.
    std::optional<std::string> last_error;
};
/// Owning, contiguous RGB bytes and presentation timestamps sampled from a clip.
struct SampledClipFrames
{
    /// Owned row-major bytes in frame/height/width/RGB-channel order.
    std::vector<std::uint8_t> rgb_pixels;
    /// Number of returned frames.
    int frame_count = 0;
    /// Height of each output frame in pixels.
    int frame_height = 0;
    /// Width of each output frame in pixels.
    int frame_width = 0;
    /// Decoded presentation times in milliseconds, one per output frame.
    std::vector<std::int64_t> frame_timestamps_ms;
};
} // namespace homesight
