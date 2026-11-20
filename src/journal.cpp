#include "journal.hpp"
#include "homesight/errors.hpp"
#include "media.hpp"
#include <fcntl.h>
#include <iomanip>
#include <memory>
#include <sstream>
#include <sys/file.h>
#include <unistd.h>

namespace homesight::detail
{
namespace
{
class Statement
{
  public:
    sqlite3_stmt *value = nullptr;
    explicit Statement(sqlite3 *db, const char *sql)
    {
        if (sqlite3_prepare_v2(db, sql, -1, &value, nullptr) != SQLITE_OK)
        {
            throw RecorderError("Cannot prepare journal statement");
        }
    }
    ~Statement()
    {
        sqlite3_finalize(value);
    }
    void bind(int column, const std::string &text)
    {
        if (sqlite3_bind_text(value, column, text.c_str(), static_cast<int>(text.size()),
                              SQLITE_TRANSIENT) != SQLITE_OK)
        {
            throw RecorderError("Cannot bind journal data");
        }
    }
    void finish()
    {
        int code = sqlite3_step(value);
        if (code == SQLITE_FULL)
        {
            throw JournalFullError("Camera journal allowance exhausted");
        }
        if (code != SQLITE_DONE)
        {
            throw RecorderError("Cannot commit journal statement");
        }
    }
};
std::string column_text(sqlite3_stmt *statement, int column)
{
    auto *text = sqlite3_column_text(statement, column);
    return text ? reinterpret_cast<const char *>(text) : "";
}
std::string encode(const CompletedSegment &c)
{
    const auto &s = c.segment;
    std::ostringstream out;
    out << std::setprecision(17) << 2 << ' ' << std::quoted(s.segment_id) << ' '
        << std::quoted(s.camera_name) << ' ' << std::quoted(s.staging_file_path.string()) << ' '
        << s.start_epoch_ms << ' ' << s.end_epoch_ms << ' ' << s.duration_seconds << ' '
        << std::quoted(s.video_codec) << ' ' << s.file_size_bytes << ' '
        << std::quoted(s.continuity_id) << ' ' << static_cast<int>(c.storage_outcome) << ' '
        << c.state_revision << ' ' << std::quoted(c.storage_uri.value_or("")) << ' '
        << std::quoted(c.storage_error.value_or("")) << ' '
        << std::quoted(c.motion_analysis_error.value_or("")) << ' ' << c.motion.has_value();
    if (c.motion)
    {
        const auto &m = *c.motion;
        const auto &p = m.params_used;
        out << ' ' << static_cast<int>(m.status) << ' '
            << (m.has_motion ? (*m.has_motion ? 1 : 0) : -1) << ' ' << m.peak_changed_area << ' '
            << m.mean_changed_area << ' ' << m.frames_with_motion << ' ' << m.frames_analyzed << ' '
            << std::quoted(m.error.value_or("")) << ' ' << p.sample_rate_fps << ' '
            << p.analysis_frame_width << ' ' << p.warmup_seconds << ' ' << p.history_frames << ' '
            << p.variance_threshold << ' ' << p.learning_rate << ' ' << p.changed_area_threshold
            << ' ' << p.minimum_frames_with_motion << ' ' << p.exclude_shadows << ' '
            << p.use_gpu_decoding << ' ' << p.decode_timeout_seconds;
    }
    out << ' ' << s.media_start_seconds;
    return out.str();
}
CompletedSegment decode(const std::string &payload)
{
    CompletedSegment c;
    auto &s = c.segment;
    int version = 0, outcome = 0;
    bool motion = false;
    std::string path, uri, storage_error, motion_error;
    std::istringstream in(payload);
    in >> version >> std::quoted(s.segment_id) >> std::quoted(s.camera_name) >> std::quoted(path) >>
        s.start_epoch_ms >> s.end_epoch_ms >> s.duration_seconds >> std::quoted(s.video_codec) >>
        s.file_size_bytes >> std::quoted(s.continuity_id) >> outcome >> c.state_revision >>
        std::quoted(uri) >> std::quoted(storage_error) >> std::quoted(motion_error) >> motion;
    s.staging_file_path = path;
    c.storage_outcome = static_cast<SegmentStorageOutcome>(outcome);
    if (!uri.empty())
    {
        c.storage_uri = uri;
    }
    if (!storage_error.empty())
    {
        c.storage_error = storage_error;
    }
    if (!motion_error.empty())
    {
        c.motion_analysis_error = motion_error;
    }
    if (motion)
    {
        MotionAnalysisResult m;
        auto &p = m.params_used;
        int status = 0, has_motion = -1;
        std::string error;
        in >> status >> has_motion >> m.peak_changed_area >> m.mean_changed_area >>
            m.frames_with_motion >> m.frames_analyzed >> std::quoted(error) >> p.sample_rate_fps >>
            p.analysis_frame_width >> p.warmup_seconds >> p.history_frames >>
            p.variance_threshold >> p.learning_rate >> p.changed_area_threshold >>
            p.minimum_frames_with_motion >> p.exclude_shadows >> p.use_gpu_decoding >>
            p.decode_timeout_seconds;
        m.status = static_cast<MotionAnalysisStatus>(status);
        if (has_motion >= 0)
        {
            m.has_motion = has_motion == 1;
        }
        if (!error.empty())
        {
            m.error = error;
        }
        c.motion = m;
    }
    if (version == 2)
    {
        in >> s.media_start_seconds;
    }
    if (!in || (version != 1 && version != 2))
    {
        throw RecorderError("Unsupported or corrupted camera journal payload");
    }
    return c;
}
} // namespace
Journal::Journal(const std::filesystem::path &directory, std::uint64_t allowance_bytes)
    : path_(directory / "journal.sqlite3"), allowance_(allowance_bytes)
{
    create_directories_durable(directory);
    lock_fd_ = open((directory / "owner.lock").c_str(), O_CREAT | O_RDWR | O_CLOEXEC, 0600);
    if (lock_fd_ < 0 || flock(lock_fd_, LOCK_EX | LOCK_NB) != 0)
    {
        if (lock_fd_ >= 0)
        {
            close(lock_fd_);
        }
        lock_fd_ = -1;
        throw RecorderError("Camera state is already owned by another recorder");
    }
    try
    {
        if (sqlite3_open_v2(path_.c_str(), &database_,
                            SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX,
                            nullptr) != SQLITE_OK)
        {
            throw RecorderError("Cannot open camera journal");
        }
        sqlite3_busy_timeout(database_, 2000);
        execute("PRAGMA auto_vacuum=INCREMENTAL");
        execute("PRAGMA journal_mode=WAL");
        execute("PRAGMA synchronous=FULL");
        execute("PRAGMA wal_autocheckpoint=1");
        execute("PRAGMA max_page_count=" + std::to_string(allowance_ / 2 / 4096));
        execute("CREATE TABLE IF NOT EXISTS metadata(version INTEGER NOT NULL)");
        execute("INSERT INTO metadata SELECT 1 WHERE NOT EXISTS(SELECT 1 FROM metadata)");
        Statement schema(database_, "SELECT version FROM metadata");
        if (sqlite3_step(schema.value) != SQLITE_ROW || sqlite3_column_int(schema.value, 0) != 1)
        {
            throw RecorderError("Unsupported camera journal schema");
        }
        execute("CREATE TABLE IF NOT EXISTS segments(id TEXT PRIMARY KEY, phase TEXT NOT NULL, "
                "local_path TEXT NOT NULL, payload TEXT NOT NULL, ordinal INTEGER NOT NULL)");
        execute("CREATE TABLE IF NOT EXISTS events(sequence INTEGER PRIMARY KEY AUTOINCREMENT, id "
                "TEXT UNIQUE NOT NULL, segment_id TEXT NOT NULL, kind INTEGER NOT NULL, payload "
                "TEXT NOT NULL)");
        sync_directory(directory);
        execute("CREATE INDEX IF NOT EXISTS segments_by_phase ON segments(phase,ordinal)");
        execute("CREATE INDEX IF NOT EXISTS segments_by_ordinal ON segments(ordinal)");
        execute("CREATE INDEX IF NOT EXISTS events_by_segment ON events(segment_id)");
    }
    catch (...)
    {
        if (database_)
        {
            sqlite3_close(database_);
        }
        close(lock_fd_);
        throw;
    }
}
Journal::~Journal()
{
    if (database_)
    {
        sqlite3_close(database_);
    }
    if (lock_fd_ >= 0)
    {
        close(lock_fd_);
    }
}
void Journal::execute(const std::string &sql)
{
    const int result = sqlite3_exec(database_, sql.c_str(), nullptr, nullptr, nullptr);
    if (result == SQLITE_FULL)
    {
        throw JournalFullError("Camera journal allowance exhausted");
    }
    if (result != SQLITE_OK)
    {
        throw RecorderError("Camera journal transaction failed");
    }
}
void Journal::check_capacity_locked()
{
    sqlite3_wal_checkpoint_v2(database_, nullptr, SQLITE_CHECKPOINT_TRUNCATE, nullptr, nullptr);
    std::uint64_t bytes = 0;
    for (const auto &suffix : {"", "-wal", "-shm"})
    {
        std::error_code error;
        auto size = std::filesystem::file_size(path_.string() + suffix, error);
        if (!error)
        {
            bytes += size;
        }
    }
    if (bytes + 128 * 1024 >= allowance_)
    {
        throw JournalFullError(
            "Camera journal allowance exhausted; acknowledge durable events before restarting");
    }
}
void Journal::check_capacity()
{
    std::lock_guard lock(mutex_);
    check_capacity_locked();
}
void Journal::save(JournalRow &row, std::optional<RecorderEventKind> kind)
{
    std::lock_guard lock(mutex_);
    check_capacity_locked();
    auto snapshot = row.completed;
    if (kind)
    {
        ++snapshot.state_revision;
    }
    auto payload = encode(snapshot);
    execute("BEGIN IMMEDIATE");
    try
    {
        Statement store(
            database_,
            "INSERT INTO segments VALUES(?1,?2,?3,?4,(SELECT COALESCE(MAX(ordinal),0)+1 FROM "
            "segments)) ON CONFLICT(id) DO UPDATE SET "
            "phase=excluded.phase,local_path=excluded.local_path,payload=excluded.payload");
        store.bind(1, snapshot.segment.segment_id);
        store.bind(2, row.phase);
        store.bind(3, row.local_path.string());
        store.bind(4, payload);
        store.finish();
        if (kind)
        {
            Statement event(database_,
                            "INSERT INTO events(id,segment_id,kind,payload) VALUES(?1,?2,?3,?4)");
            event.bind(1, new_id());
            event.bind(2, snapshot.segment.segment_id);
            sqlite3_bind_int(event.value, 3, static_cast<int>(*kind));
            event.bind(4, payload);
            event.finish();
        }
        execute("COMMIT");
        row.completed = snapshot;
    }
    catch (...)
    {
        sqlite3_exec(database_, "ROLLBACK", nullptr, nullptr, nullptr);
        throw;
    }
}
std::vector<JournalRow> Journal::rows()
{
    std::lock_guard lock(mutex_);
    Statement query(database_, "SELECT payload,phase,local_path FROM segments ORDER BY ordinal");
    std::vector<JournalRow> result;
    int code;
    while ((code = sqlite3_step(query.value)) == SQLITE_ROW)
    {
        result.push_back({decode(column_text(query.value, 0)), column_text(query.value, 1),
                          column_text(query.value, 2)});
    }
    if (code != SQLITE_DONE)
    {
        throw RecorderError("Cannot read camera journal");
    }
    return result;
}
std::vector<JournalRow> Journal::pending_rows(const std::vector<std::string> &phases,
                                              std::size_t limit)
{
    std::lock_guard lock(mutex_);
    if (phases.empty())
    {
        return {};
    }
    std::string sql = "SELECT payload,phase,local_path FROM segments WHERE phase IN (";
    for (std::size_t i = 0; i < phases.size(); ++i)
    {
        sql += (i ? ",?" : "?") + std::to_string(i + 1);
    }
    sql += ") ORDER BY ordinal LIMIT ?" + std::to_string(phases.size() + 1);
    Statement query(database_, sql.c_str());
    for (std::size_t i = 0; i < phases.size(); ++i)
    {
        query.bind(static_cast<int>(i + 1), phases[i]);
    }
    sqlite3_bind_int64(query.value, static_cast<int>(phases.size() + 1),
                       static_cast<sqlite3_int64>(std::min<std::size_t>(limit, 100000)));
    std::vector<JournalRow> result;
    int code;
    while ((code = sqlite3_step(query.value)) == SQLITE_ROW)
    {
        result.push_back({decode(column_text(query.value, 0)), column_text(query.value, 1),
                          column_text(query.value, 2)});
    }
    if (code != SQLITE_DONE)
    {
        throw RecorderError("Cannot read pending camera work");
    }
    return result;
}
std::optional<JournalRow> Journal::find_row(const std::string &id)
{
    std::lock_guard lock(mutex_);
    Statement query(database_, "SELECT payload,phase,local_path FROM segments WHERE id=?1");
    query.bind(1, id);
    int code = sqlite3_step(query.value);
    if (code == SQLITE_DONE)
    {
        return std::nullopt;
    }
    if (code != SQLITE_ROW)
    {
        throw RecorderError("Cannot read camera work revision");
    }
    return JournalRow{decode(column_text(query.value, 0)), column_text(query.value, 1),
                      column_text(query.value, 2)};
}
std::uint64_t Journal::pending_count()
{
    std::lock_guard lock(mutex_);
    Statement query(
        database_,
        "SELECT COUNT(*) FROM segments WHERE phase IN ('storage','uploading','upload_evicted')");
    if (sqlite3_step(query.value) != SQLITE_ROW)
    {
        throw RecorderError("Cannot count pending camera work");
    }
    return static_cast<std::uint64_t>(sqlite3_column_int64(query.value, 0));
}
std::vector<RecorderEvent> Journal::events(std::uint64_t after, std::size_t count)
{
    std::lock_guard lock(mutex_);
    Statement query(
        database_,
        "SELECT id,sequence,kind,payload FROM events WHERE sequence>?1 ORDER BY sequence LIMIT ?2");
    sqlite3_bind_int64(query.value, 1, static_cast<sqlite3_int64>(after));
    sqlite3_bind_int64(query.value, 2,
                       static_cast<sqlite3_int64>(std::min<std::size_t>(count, 100000)));
    std::vector<RecorderEvent> result;
    int code;
    while ((code = sqlite3_step(query.value)) == SQLITE_ROW)
    {
        result.push_back({column_text(query.value, 0),
                          static_cast<std::uint64_t>(sqlite3_column_int64(query.value, 1)),
                          static_cast<RecorderEventKind>(sqlite3_column_int(query.value, 2)),
                          decode(column_text(query.value, 3))});
    }
    if (code != SQLITE_DONE)
    {
        throw RecorderError("Cannot read recorder events");
    }
    return result;
}
void Journal::acknowledge(const std::vector<std::string> &ids)
{
    std::lock_guard lock(mutex_);
    execute("BEGIN IMMEDIATE");
    try
    {
        for (const auto &id : ids)
        {
            Statement erase(database_, "DELETE FROM events WHERE id=?1");
            erase.bind(1, id);
            erase.finish();
        }
        execute("DELETE FROM segments WHERE phase='done' AND local_path='' AND id NOT IN(SELECT "
                "segment_id FROM events)");
        execute("COMMIT");
        execute("PRAGMA incremental_vacuum");
        sqlite3_wal_checkpoint_v2(database_, nullptr, SQLITE_CHECKPOINT_TRUNCATE, nullptr, nullptr);
    }
    catch (...)
    {
        sqlite3_exec(database_, "ROLLBACK", nullptr, nullptr, nullptr);
        throw;
    }
}
} // namespace homesight::detail
