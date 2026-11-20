#include "homesight/segment_recorder.hpp"
#include "homesight/errors.hpp"
#include "homesight/segment_motion_analyzer.hpp"
#include "homesight/segment_storage.hpp"
#include "journal.hpp"
#include "media.hpp"
#include "recording_pipeline.hpp"
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <glib.h>
#include <map>
#include <mutex>
#include <thread>

namespace homesight
{
using detail::JournalRow;
class SegmentRecorder::Impl : public std::enable_shared_from_this<SegmentRecorder::Impl>
{
  public:
    SegmentRecorderConfig config;
    std::unique_ptr<detail::Journal> journal;
    std::unique_ptr<SegmentStorage> storage;
    SegmentMotionAnalyzer analyzer;
    mutable std::mutex mutex;
    std::mutex lifecycle;
    std::mutex operations;
    std::mutex delivery;
    std::condition_variable changed;
    RecorderStatistics statistics;
    std::atomic<bool> stop{true};
    std::atomic<std::int64_t> deadline{0};
    std::jthread recording_thread;
    std::jthread analysis_thread;
    std::jthread storage_thread;
    std::map<std::string, std::pair<std::int64_t, double>> retries;
    std::atomic<bool> quota_dirty{true};
    std::uint64_t event_cursor = 0;
    std::uint64_t completed_cursor = 0;
    explicit Impl(SegmentRecorderConfig c) : config(std::move(c)), analyzer(config.motion_analysis)
    {
        validate_config(config);
        config.staging_directory = std::filesystem::absolute(config.staging_directory);
        journal = std::make_unique<detail::Journal>(
            config.staging_directory / config.camera.camera_name, config.journal_allowance_bytes);
        storage = create_segment_storage(config.storage);
    }
    void set_state(RecorderState state)
    {
        std::lock_guard lock(mutex);
        if (statistics.state == RecorderState::failed ||
            (stop && state == RecorderState::recording))
        {
            return;
        }
        statistics.state = state;
        changed.notify_all();
    }
    void fail(const std::exception &error)
    {
        deadline = detail::monotonic_ms() + 10000;
        stop = true;
        std::lock_guard lock(mutex);
        statistics.last_error = error.what();
        statistics.state = RecorderState::failed;
        changed.notify_all();
    }
    void pause(double seconds)
    {
        std::unique_lock lock(mutex);
        changed.wait_for(lock, std::chrono::milliseconds(static_cast<long long>(seconds * 1000)),
                         [this]
                         {
                             return stop.load();
                         });
    }
    void recover()
    {
        analyzer.reset();
        for (auto row : journal->rows())
        {
            if (row.phase == "open")
            {
                auto &info = row.completed.segment;
                try
                {
                    detail::MediaReader media(info.staging_file_path, false,
                                              config.motion_analysis.decode_timeout_seconds);
                    info.duration_seconds = media.duration_seconds();
                    if (!media.read_frame())
                    {
                        throw SegmentDecodingError("Interrupted segment contains no frames");
                    }
                    info.end_epoch_ms = info.start_epoch_ms +
                                        static_cast<std::int64_t>(info.duration_seconds * 1000);
                    info.file_size_bytes = std::filesystem::file_size(info.staging_file_path);
                    row.phase = "analysis";
                    journal->save(row);
                }
                catch (const SegmentDecodingError &)
                {
                    info.duration_seconds = 0;
                    info.end_epoch_ms = info.start_epoch_ms;
                    row.completed.storage_outcome = SegmentStorageOutcome::interrupted;
                    row.completed.storage_error =
                        "Interrupted recording could not be finalized; available footage retained";
                    if (std::filesystem::exists(row.local_path))
                    {
                        info.file_size_bytes = std::filesystem::file_size(row.local_path);
                    }
                    else
                    {
                        row.local_path.clear();
                    }
                    row.phase = "done";
                    journal->save(row, RecorderEventKind::segment_completed);
                }
            }
            else if (row.phase == "delete_motion" || row.phase == "delete_quota" ||
                     row.phase == "delete_quota_upload")
            {
                finish_deletion(row);
            }
            else if (row.phase == "cleanup")
            {
                finish_cleanup(row);
            }
            else if (row.phase == "storage" &&
                     std::holds_alternative<LocalDirectoryStorageConfig>(config.storage))
            {
                auto uri = storage->find_stored_segment(row.completed);
                if (uri)
                {
                    row.completed.storage_uri = *uri;
                    row.completed.storage_outcome = SegmentStorageOutcome::stored;
                    char *path = g_filename_from_uri(uri->c_str(), nullptr, nullptr);
                    if (!path)
                    {
                        throw SegmentStorageError("Invalid recovered storage URI");
                    }
                    if (row.local_path != std::filesystem::path(path) &&
                        std::filesystem::exists(row.local_path))
                    {
                        detail::remove_file_durable(row.local_path);
                    }
                    row.local_path = path;
                    g_free(path);
                    row.phase = "done";
                    journal->save(row, RecorderEventKind::storage_changed);
                }
            }
        }
        enforce_quota();
    }
    void finish_cleanup(JournalRow &row)
    {
        if (!row.local_path.empty())
        {
            detail::remove_file_durable(row.local_path);
        }
        row.local_path.clear();
        row.phase = "done";
        journal->save(row);
    }
    void finish_deletion(JournalRow &row)
    {
        bool inflight = row.phase == "delete_quota_upload";
        bool quota = row.phase == "delete_quota" || inflight;
        if (!row.local_path.empty())
        {
            detail::remove_file_durable(row.local_path);
        }
        row.local_path.clear();
        row.phase = inflight ? "upload_evicted" : "done";
        if (!row.completed.storage_uri || !row.completed.storage_uri->starts_with("s3://"))
        {
            row.completed.storage_uri.reset();
            row.completed.storage_outcome = quota ? SegmentStorageOutcome::deleted_by_quota
                                                  : SegmentStorageOutcome::deleted_without_motion;
        }
        journal->save(row, quota ? RecorderEventKind::deleted_by_quota
                                 : RecorderEventKind::segment_completed);
        retries.erase(row.completed.segment.segment_id);
        changed.notify_all();
    }
    void enforce_quota()
    {
        auto rows = journal->rows();
        std::uint64_t total = 0;
        for (const auto &row : rows)
        {
            if (row.phase != "open" && !row.local_path.empty() &&
                std::filesystem::exists(row.local_path))
            {
                total += std::filesystem::file_size(row.local_path);
            }
        }
        for (auto &row : rows)
        {
            if (total <= config.local_video_allowance_bytes)
            {
                break;
            }
            if (row.phase == "open" || row.local_path.empty() ||
                !std::filesystem::exists(row.local_path))
            {
                continue;
            }
            const auto bytes = std::filesystem::file_size(row.local_path);
            row.phase = row.phase == "uploading" ? "delete_quota_upload" : "delete_quota";
            journal->save(row);
            finish_deletion(row);
            total -= bytes;
        }
        std::lock_guard lock(mutex);
        statistics.local_video_bytes = total;
    }
    void record_camera() noexcept
    {
        try
        {
            auto delay =
                std::make_shared<std::atomic<double>>(config.reconnect_initial_delay_seconds);
            while (!stop)
            {
                try
                {
                    set_state(RecorderState::connecting);
                    detail::record_session(
                        config, stop, deadline,
                        [self = shared_from_this()](const SegmentInfo &info)
                        {
                            if (self->stop)
                            {
                                throw detail::RecordingStopped("Recording is stopping");
                            }
                            JournalRow row;
                            row.completed.segment = info;
                            row.local_path = info.staging_file_path;
                            row.phase = "open";
                            self->journal->save(row);
                        },
                        [self = shared_from_this()](const SegmentInfo &info)
                        {
                            JournalRow row;
                            row.completed.segment = info;
                            row.local_path = info.staging_file_path;
                            row.phase = "analysis";
                            self->journal->save(row);
                            self->quota_dirty = true;
                            std::lock_guard lock(self->mutex);
                            ++self->statistics.segments_written;
                            self->statistics.last_segment_closed_epoch_ms = info.end_epoch_ms;
                            self->changed.notify_all();
                        },
                        [self = shared_from_this(), delay]
                        {
                            self->set_state(RecorderState::recording);
                            *delay = self->config.reconnect_initial_delay_seconds;
                        });
                }
                catch (const JournalFullError &)
                {
                    throw;
                }
                catch (const CameraConnectionError &)
                {
                    if (stop)
                    {
                        break;
                    }
                    {
                        std::lock_guard lock(operations);
                        recover();
                    }
                    {
                        std::lock_guard lock(mutex);
                        statistics.state = RecorderState::reconnecting;
                        ++statistics.reconnect_attempts;
                        statistics.last_error = "Camera connection lost; reconnecting";
                    }
                    pause(delay->load());
                    *delay = std::min(delay->load() * 2, config.reconnect_max_delay_seconds);
                }
            }
        }
        catch (const std::exception &error)
        {
            fail(error);
        }
    }
    void analyze_segments() noexcept
    {
        try
        {
            while (!stop)
            {
                bool worked = false;
                {
                    std::lock_guard lock(operations);
                    if (quota_dirty.exchange(false))
                    {
                        enforce_quota();
                    }
                    for (auto row : journal->pending_rows({"analysis"}, 1))
                    {
                        if (stop)
                        {
                            break;
                        }
                        if (row.phase != "analysis")
                        {
                            continue;
                        }
                        worked = true;
                        if (config.analyze_motion_after_segment_closes)
                        {
                            row.completed.motion = analyzer.analyze_segment_motion(
                                row.completed.segment.staging_file_path,
                                row.completed.segment.continuity_id,
                                row.completed.segment.media_start_seconds);
                            row.completed.motion_analysis_error = row.completed.motion->error;
                            std::lock_guard status_lock(mutex);
                            ++statistics.segments_analyzed;
                        }
                        if (config.delete_segments_without_motion && row.completed.motion &&
                            row.completed.motion->status == MotionAnalysisStatus::valid &&
                            row.completed.motion->has_motion == false)
                        {
                            row.phase = "delete_motion";
                            journal->save(row);
                            finish_deletion(row);
                            std::lock_guard status_lock(mutex);
                            ++statistics.segments_deleted_without_motion;
                        }
                        else
                        {
                            row.phase = "storage";
                            journal->save(row, RecorderEventKind::segment_completed);
                            changed.notify_all();
                        }
                        break;
                    }
                }
                if (!worked)
                {
                    pause(0.1);
                }
            }
        }
        catch (const std::exception &error)
        {
            fail(error);
        }
    }
    void store_segments() noexcept
    {
        try
        {
            const auto *s3 = std::get_if<S3StorageConfig>(&config.storage);
            while (!stop)
            {
                std::unique_lock lock(operations);
                if (stop)
                {
                    break;
                }
                std::optional<JournalRow> selected;
                for (auto row : journal->pending_rows({"storage", "uploading", "upload_evicted"}))
                {
                    auto retry = retries.find(row.completed.segment.segment_id);
                    if (retry == retries.end() || detail::monotonic_ms() >= retry->second.first)
                    {
                        selected = row;
                        break;
                    }
                }
                if (!selected)
                {
                    lock.unlock();
                    pause(0.1);
                    continue;
                }
                auto row = *selected;
                const bool probe = row.phase == "upload_evicted";
                if (s3 && !probe)
                {
                    row.phase = "uploading";
                    journal->save(row);
                }
                if (stop)
                {
                    break;
                }
                if (s3)
                {
                    lock.unlock();
                }
                std::optional<std::string> uri;
                bool operation_failed = false;
                try
                {
                    uri = probe ? storage->find_stored_segment(row.completed)
                                : std::optional(storage->store_segment_file(row.completed));
                }
                catch (const std::exception &)
                {
                    operation_failed = true;
                }
                if (s3)
                {
                    lock.lock();
                }
                auto current = journal->find_row(row.completed.segment.segment_id);
                if (!current)
                {
                    throw RecorderError("In-flight storage revision disappeared");
                }
                row = *current;
                const bool evicted = row.phase == "upload_evicted";
                if (uri)
                {
                    row.completed.storage_uri = *uri;
                    row.completed.storage_error.reset();
                    row.completed.storage_outcome = SegmentStorageOutcome::stored;
                    if (s3)
                    {
                        row.phase = s3->delete_staging_file_after_upload && !row.local_path.empty()
                                        ? "cleanup"
                                        : "done";
                    }
                    else
                    {
                        char *filename = g_filename_from_uri(uri->c_str(), nullptr, nullptr);
                        if (!filename)
                        {
                            throw SegmentStorageError("Invalid local storage result");
                        }
                        row.local_path = filename;
                        g_free(filename);
                        row.phase = "done";
                    }
                    journal->save(row, RecorderEventKind::storage_changed);
                    if (row.phase == "cleanup")
                    {
                        finish_cleanup(row);
                    }
                    retries.erase(row.completed.segment.segment_id);
                    std::lock_guard status_lock(mutex);
                    ++statistics.segments_stored;
                }
                else if (probe && !operation_failed)
                {
                    row.phase = "done";
                    journal->save(row);
                    retries.erase(row.completed.segment.segment_id);
                }
                else
                {
                    row.phase = evicted ? "upload_evicted" : "storage";
                    if (!evicted &&
                        row.completed.storage_outcome != SegmentStorageOutcome::storage_failed)
                    {
                        row.completed.storage_outcome = SegmentStorageOutcome::storage_failed;
                        row.completed.storage_error =
                            "Segment storage failed; durable retry pending";
                        journal->save(row, RecorderEventKind::storage_changed);
                    }
                    else
                    {
                        journal->save(row);
                    }
                    auto &retry = retries[row.completed.segment.segment_id];
                    retry.second =
                        retry.second == 0
                            ? (s3 ? s3->retry_initial_delay_seconds : 1)
                            : std::min(retry.second * 2, s3 ? s3->retry_max_delay_seconds : 30);
                    retry.first =
                        detail::monotonic_ms() + static_cast<std::int64_t>(retry.second * 1000);
                    std::lock_guard status_lock(mutex);
                    ++statistics.storage_failures;
                }
                enforce_quota();
                changed.notify_all();
            }
        }
        catch (const std::exception &error)
        {
            fail(error);
        }
    }
    std::vector<RecorderEvent> take_events(std::uint64_t &cursor, std::size_t count)
    {
        std::lock_guard lock(delivery);
        auto events = journal->events(cursor, count);
        if (!events.empty())
        {
            cursor = events.back().sequence;
        }
        return events;
    }
};
SegmentRecorder::SegmentRecorder(SegmentRecorderConfig config)
    : impl_(std::make_shared<Impl>(std::move(config)))
{
}
SegmentRecorder::~SegmentRecorder()
{
    try
    {
        stop_recording();
    }
    catch (const std::exception &)
    { /* Worker state is already durable; destructors cannot propagate. */
    }
}
void SegmentRecorder::start_recording()
{
    auto &self = *impl_;
    std::lock_guard lifecycle_lock(self.lifecycle);
    if (self.recording_thread.joinable())
    {
        throw RecorderError("Recorder already started; stop it before restarting");
    }
    if (impl_.use_count() > 1)
    {
        throw RecorderError("Previous GStreamer callbacks are still releasing camera state");
    }
    try
    {
        detail::create_directories_durable(self.config.staging_directory /
                                           self.config.camera.camera_name / "video");
        self.recover();
        self.journal->check_capacity();
    }
    catch (const std::exception &error)
    {
        self.fail(error);
        throw RecorderError(std::string("Cannot initialize recording state: ") + error.what());
    }
    self.stop = false;
    {
        std::lock_guard lock(self.mutex);
        self.statistics.state = RecorderState::stopped;
        self.statistics.last_error.reset();
    }
    self.set_state(RecorderState::connecting);
    try
    {
        self.analysis_thread = std::jthread(
            [&self]
            {
                self.analyze_segments();
            });
        self.storage_thread = std::jthread(
            [&self]
            {
                self.store_segments();
            });
        self.recording_thread = std::jthread(
            [&self]
            {
                self.record_camera();
            });
    }
    catch (...)
    {
        self.stop = true;
        self.changed.notify_all();
        if (self.analysis_thread.joinable())
        {
            self.analysis_thread.join();
        }
        if (self.storage_thread.joinable())
        {
            self.storage_thread.join();
        }
        throw;
    }
}
void SegmentRecorder::stop_recording(std::chrono::milliseconds timeout)
{
    if (timeout.count() < 0 || timeout > std::chrono::hours(24))
    {
        throw InvalidConfigError("Invalid finalization timeout");
    }
    auto &self = *impl_;
    std::lock_guard lifecycle_lock(self.lifecycle);
    if (!self.recording_thread.joinable())
    {
        return;
    }
    self.deadline = detail::monotonic_ms() + timeout.count();
    self.stop = true;
    {
        std::lock_guard lock(self.mutex);
        if (self.statistics.state != RecorderState::failed)
        {
            self.statistics.state = RecorderState::stopping;
        }
    }
    self.changed.notify_all();
    self.recording_thread.join();
    self.analysis_thread.join();
    self.storage_thread.join();
    std::lock_guard lock(self.mutex);
    if (self.statistics.state != RecorderState::failed)
    {
        self.statistics.state = RecorderState::stopped;
    }
    self.changed.notify_all();
}
bool SegmentRecorder::is_recording() const
{
    std::lock_guard lock(impl_->mutex);
    const auto state = impl_->statistics.state;
    return state == RecorderState::connecting || state == RecorderState::recording ||
           state == RecorderState::reconnecting;
}
RecorderStatistics SegmentRecorder::get_recorder_statistics() const
{
    RecorderStatistics result;
    {
        std::lock_guard lock(impl_->mutex);
        result = impl_->statistics;
    }
    result.segments_waiting_for_upload =
        std::holds_alternative<S3StorageConfig>(impl_->config.storage)
            ? impl_->journal->pending_count()
            : 0;
    return result;
}
SegmentRecorderConfig SegmentRecorder::get_config() const
{
    return impl_->config;
}
std::vector<RecorderEvent> SegmentRecorder::take_recorder_events(std::size_t count)
{
    return impl_->take_events(impl_->event_cursor, count);
}
std::optional<RecorderEvent>
SegmentRecorder::wait_for_next_recorder_event(std::chrono::milliseconds timeout)
{
    if (timeout.count() < 0 || timeout > std::chrono::hours(24))
    {
        throw InvalidConfigError("Event timeout must be between zero and 24 hours");
    }
    const auto end = std::chrono::steady_clock::now() + timeout;
    do
    {
        auto events = take_recorder_events(1);
        if (!events.empty())
        {
            return events.front();
        }
        std::unique_lock lock(impl_->mutex);
        impl_->changed.wait_until(
            lock, std::min(end, std::chrono::steady_clock::now() + std::chrono::milliseconds(50)));
    } while (std::chrono::steady_clock::now() < end);
    return std::nullopt;
}
void SegmentRecorder::acknowledge_recorder_events(const std::vector<std::string> &ids)
{
    impl_->journal->acknowledge(ids);
}
std::vector<CompletedSegment> SegmentRecorder::take_completed_segments(std::size_t count)
{
    std::vector<CompletedSegment> result;
    while (result.size() < count)
    {
        auto events = impl_->take_events(impl_->completed_cursor, 1);
        if (events.empty())
        {
            break;
        }
        auto &event = events.front();
        const auto outcome = event.completed_segment.storage_outcome;
        if (event.kind != RecorderEventKind::deleted_by_quota &&
            outcome != SegmentStorageOutcome::pending)
        {
            result.push_back(event.completed_segment);
        }
    }
    return result;
}
std::optional<CompletedSegment>
SegmentRecorder::wait_for_next_completed_segment(std::chrono::milliseconds timeout)
{
    if (timeout.count() < 0 || timeout > std::chrono::hours(24))
    {
        throw InvalidConfigError("Completion timeout must be between zero and 24 hours");
    }
    const auto end = std::chrono::steady_clock::now() + timeout;
    do
    {
        auto values = take_completed_segments(1);
        if (!values.empty())
        {
            return values.front();
        }
        std::unique_lock lock(impl_->mutex);
        impl_->changed.wait_until(
            lock, std::min(end, std::chrono::steady_clock::now() + std::chrono::milliseconds(50)));
    } while (std::chrono::steady_clock::now() < end);
    return std::nullopt;
}
} // namespace homesight
