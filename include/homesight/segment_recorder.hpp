#pragma once
/** @file segment_recorder.hpp
 * Durable per-camera recording and event delivery.
 */
#include "homesight/segment_info.hpp"
#include <chrono>
#include <memory>
namespace homesight
{
/** Own a camera's journal, video pipeline, analysis, and storage workers.
 * Construction locks the camera state directory for the object's entire lifetime,
 * including while stopped. Reading events advances a shared delivery cursor but
 * does not acknowledge them. Persist each revision idempotently before acknowledging.
 * Control and event operations are internally synchronized; the caller must keep
 * this object alive and coordinate consumers sharing its delivery cursors.
 */
class SegmentRecorder
{
  public:
    /** Validate settings, open the durable journal, and claim exclusive ownership.
     * @param config Camera, storage, analysis, quota, and staging settings copied into the
     * recorder.
     * @throws InvalidConfigError If configuration is invalid.
     * @throws RecorderError If state is inaccessible or another owner holds the camera lock.
     * @throws SegmentStorageError If the selected storage backend cannot be initialized.
     */
    explicit SegmentRecorder(SegmentRecorderConfig config);
    /// Stop workers without propagating exceptions; delayed callbacks retain required ownership.
    ~SegmentRecorder();
    /// Copying would duplicate exclusive journal ownership and is disabled.
    SegmentRecorder(const SegmentRecorder &) = delete;
    /// Copy assignment is disabled for an exclusively owned recorder.
    SegmentRecorder &operator=(const SegmentRecorder &) = delete;
    /** Reconcile durable unfinished work and launch recording, analysis, and storage.
     * Connection proceeds asynchronously; inspect statistics for connection/worker errors.
     * @throws RecorderError If already started, cleanup is still pending, or recovery fails.
     */
    void start_recording();
    /** Request finalization and join workers without draining the durable backlog.
     * An in-progress decode/upload can add its operation timeout to the finalization
     * deadline. Unfinished work replays on restart. Deadline failure is reported in
     * statistics; retained callbacks keep resources and the camera lock alive.
     * @param finalize_timeout Finalization allowance, from zero to 24 hours; default 10 seconds.
     * @throws InvalidConfigError If the allowance is outside its supported range.
     */
    void stop_recording(std::chrono::milliseconds finalize_timeout = std::chrono::seconds(10));
    /// Return true in connecting, recording, or reconnecting states; false while stopping/failed.
    /// @return Whether a connection/recording worker is active.
    [[nodiscard]] bool is_recording() const;
    /// Return an owned operational snapshot, including current journal-backed pending S3 work.
    /// @return Counts, state, local quota accounting, and optional diagnostic.
    [[nodiscard]] RecorderStatistics get_recorder_statistics() const;
    /// Return a configuration copy, including supplied credentials; to_dict() redaction is
    /// Python-only.
    /// @return Independent copy of the supplied settings.
    [[nodiscard]] SegmentRecorderConfig get_config() const;
    /** Deliver journal events after this instance's shared event cursor.
     * @param max_count Maximum number to return; default 1024.
     * @return Owned snapshots, ordered by per-camera sequence; reading never acknowledges.
     * @throws RecorderError If the journal cannot be read.
     */
    [[nodiscard]] std::vector<RecorderEvent> take_recorder_events(std::size_t max_count = 1024);
    /** Deliver the next journal event, waiting if necessary.
     * @param timeout Wait allowance from zero to 24 hours.
     * @return The event or an empty optional on timeout; acknowledgement is separate.
     * @throws InvalidConfigError If timeout is outside its supported range.
     * @throws RecorderError If the journal cannot be read.
     */
    [[nodiscard]] std::optional<RecorderEvent>
    wait_for_next_recorder_event(std::chrono::milliseconds timeout);
    /** Durably acknowledge events after application-side persistence.
     * @param event_ids Stable event identities; acknowledgement is idempotent.
     * @throws RecorderError If acknowledgement cannot be committed.
     */
    void acknowledge_recorder_events(const std::vector<std::string> &event_ids);
    /** Read compatibility outcomes using a cursor independent of the event API.
     * Storage changes may deliver newer revisions of a previously reported segment.
     * @param max_count Maximum number of outcomes; default 1024.
     * @return Completed outcomes, excluding pending storage and quota-only notifications.
     * @throws RecorderError If journal delivery fails.
     */
    [[nodiscard]] std::vector<CompletedSegment>
    take_completed_segments(std::size_t max_count = 1024);
    /** Wait for a compatibility outcome without acknowledging durable events.
     * @param timeout Wait allowance from zero to 24 hours.
     * @return An outcome or an empty optional on timeout.
     * @throws InvalidConfigError If timeout is outside its supported range.
     * @throws RecorderError If journal delivery fails.
     */
    [[nodiscard]] std::optional<CompletedSegment>
    wait_for_next_completed_segment(std::chrono::milliseconds timeout);

  private:
    class Impl;
    std::shared_ptr<Impl> impl_;
};
} // namespace homesight
