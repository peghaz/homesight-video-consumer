#pragma once
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <gst/gst.h>
#include <memory>
#include <opencv2/core.hpp>
#include <optional>
#include <string>

namespace homesight::detail
{
void initialize_gstreamer();
using AsyncElementFunction = void (*)(GstElement *, gpointer);
void call_gstreamer_async(GstElement *element, AsyncElementFunction function,
                          gpointer data = nullptr, GDestroyNotify destroy = nullptr);
[[nodiscard]] std::string new_id();
[[nodiscard]] std::int64_t utc_epoch_ms();
struct DecodedFrame
{
    cv::Mat rgb;
    double seconds = 0;
    double duration_seconds = 0;
    std::uint64_t timestamp_ns = 0;
};
class MediaReader
{
  public:
    MediaReader(const std::filesystem::path &path, bool use_gpu, double timeout_seconds);
    ~MediaReader();
    MediaReader(const MediaReader &) = delete;
    MediaReader &operator=(const MediaReader &) = delete;
    [[nodiscard]] std::optional<DecodedFrame> read_frame();
    void seek(double seconds);
    [[nodiscard]] double duration_seconds() const;

  private:
    GstElement *pipeline_ = nullptr;
    GstElement *sink_ = nullptr;
    std::chrono::steady_clock::time_point deadline_;
};
void sync_file(const std::filesystem::path &path);
void sync_directory(const std::filesystem::path &path);
void create_directories_durable(const std::filesystem::path &path);
void remove_file_durable(const std::filesystem::path &path);
} // namespace homesight::detail
