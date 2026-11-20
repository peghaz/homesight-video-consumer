#pragma once
#include "homesight/segment_info.hpp"
#include <mutex>
#include <sqlite3.h>

namespace homesight::detail
{
struct JournalRow
{
    CompletedSegment completed;
    std::string phase;
    std::filesystem::path local_path;
};
class Journal
{
  public:
    Journal(const std::filesystem::path &directory, std::uint64_t allowance_bytes);
    ~Journal();
    Journal(const Journal &) = delete;
    Journal &operator=(const Journal &) = delete;
    void save(JournalRow &row, std::optional<RecorderEventKind> kind = std::nullopt);
    [[nodiscard]] std::vector<JournalRow> rows();
    [[nodiscard]] std::vector<JournalRow> pending_rows(const std::vector<std::string> &phases,
                                                       std::size_t limit = 128);
    [[nodiscard]] std::optional<JournalRow> find_row(const std::string &id);
    [[nodiscard]] std::uint64_t pending_count();
    [[nodiscard]] std::vector<RecorderEvent> events(std::uint64_t after, std::size_t count);
    void acknowledge(const std::vector<std::string> &ids);
    void check_capacity();

  private:
    void execute(const std::string &sql);
    void check_capacity_locked();
    std::mutex mutex_;
    sqlite3 *database_ = nullptr;
    int lock_fd_ = -1;
    std::filesystem::path path_;
    std::uint64_t allowance_;
};
} // namespace homesight::detail
