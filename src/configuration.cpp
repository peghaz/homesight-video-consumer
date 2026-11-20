#include "homesight/errors.hpp"
#include "homesight/recorder_config.hpp"
#include <cmath>
#include <regex>

namespace homesight
{
namespace
{
bool positive(double value)
{
    return std::isfinite(value) && value > 0 && value <= 86400;
}
} // namespace
void validate_motion_params(const MotionAnalysisParams &p)
{
    if (!positive(p.sample_rate_fps) || p.sample_rate_fps > 240 || p.analysis_frame_width < 8 ||
        p.analysis_frame_width > 8192 || !std::isfinite(p.warmup_seconds) || p.warmup_seconds < 0 ||
        p.history_frames < 2 || !positive(p.variance_threshold) ||
        !std::isfinite(p.learning_rate) || p.learning_rate < -1 || p.learning_rate > 1 ||
        !std::isfinite(p.changed_area_threshold) || p.changed_area_threshold <= 0 ||
        p.changed_area_threshold > 1 || p.minimum_frames_with_motion < 1 ||
        !positive(p.decode_timeout_seconds))
    {
        throw InvalidConfigError("Invalid motion analysis parameters");
    }
}
void validate_config(const SegmentRecorderConfig &c)
{
    validate_motion_params(c.motion_analysis);
    if (!std::regex_match(c.camera.camera_name, std::regex("[a-z0-9_]{1,128}")) ||
        !(c.camera.rtsp_url.starts_with("rtsp://") || c.camera.rtsp_url.starts_with("rtsps://")) ||
        c.camera.rtsp_latency_ms < 0 || c.staging_directory.empty() ||
        !positive(c.segment_duration_seconds) || !positive(c.network_timeout_seconds) ||
        c.network_timeout_seconds < 0.001 || !positive(c.reconnect_initial_delay_seconds) ||
        !positive(c.reconnect_max_delay_seconds) ||
        c.reconnect_max_delay_seconds < c.reconnect_initial_delay_seconds ||
        c.local_video_allowance_bytes == 0 || c.journal_allowance_bytes < 1024 * 1024 ||
        (c.delete_segments_without_motion && !c.analyze_motion_after_segment_closes))
    {
        throw InvalidConfigError("Invalid recorder configuration");
    }
    if (const auto *local = std::get_if<LocalDirectoryStorageConfig>(&c.storage))
    {
        if (local->root_directory.empty())
        {
            throw InvalidConfigError("Local storage requires root_directory");
        }
    }
    else
    {
        const auto &s3 = std::get<S3StorageConfig>(c.storage);
        if (s3.bucket_name.empty() || !positive(s3.request_timeout_seconds) ||
            !positive(s3.retry_initial_delay_seconds) ||
            s3.retry_max_delay_seconds < s3.retry_initial_delay_seconds)
        {
            throw InvalidConfigError("Invalid S3 configuration");
        }
    }
}
} // namespace homesight
