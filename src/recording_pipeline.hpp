#pragma once
#include "homesight/errors.hpp"
#include "homesight/segment_info.hpp"
#include <atomic>
#include <functional>
#include <string>

namespace homesight::detail
{
/// An expected capture cancellation when a new fragment arrives after stop begins.
class RecordingStopped : public RecorderError
{
  public:
    using RecorderError::RecorderError;
};
struct RtspConnectionSettings
{
    std::string location;
    std::string username;
    std::string password;
};
/// Keep credentials out of the request URI; tolerate literal @ in user information.
[[nodiscard]] RtspConnectionSettings parse_rtsp_connection_settings(const std::string &url);
/// Runs a single RTSP connection. All timestamps are recorder-host UTC and media durations.
void record_session(const SegmentRecorderConfig &config, const std::atomic<bool> &stop,
                    const std::atomic<std::int64_t> &finalize_deadline_ms,
                    const std::function<void(const SegmentInfo &)> &opened,
                    const std::function<void(const SegmentInfo &)> &closed,
                    const std::function<void()> &recording);
[[nodiscard]] std::int64_t monotonic_ms();
} // namespace homesight::detail
