#pragma once
/** @file recorder_config.hpp
 * Public configuration values and validation.
 * References to to_dict() describe Python mapping redaction; C++ values retain credentials.
 */
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <variant>
/// Public camera recording, motion analysis, and segment storage interfaces.
namespace homesight
{
/// Container used to finalize encoded camera video without re-encoding.
enum class VideoContainerFormat
{
    /// Finalize each segment as MP4; an interrupted unfinished file may be unplayable.
    mp4,
    /// Finalize as Matroska, allowing better recovery of interrupted video.
    matroska
};
/// RTP transport selected for the RTSP connection.
enum class RtspTransport
{
    /// Interleave RTP over TCP (the recorder default).
    tcp,
    /// Receive RTP over UDP.
    udp
};
/// Connection settings for one H.264/H.265 camera; credentials remain application-owned.
struct CameraStreamConfig
{
    /// Stable camera identifier: 1–128 lowercase letters, digits, or underscores.
    std::string camera_name;
    /// RTSP or RTSPS URL, optionally containing camera credentials; omitted by to_dict().
    std::string rtsp_url;
    /// RTP transport; defaults to TCP.
    RtspTransport rtsp_transport = RtspTransport::tcp;
    /// Nonnegative RTSP jitter-buffer latency in milliseconds; default 200.
    int rtsp_latency_ms = 200;
};
/// CPU MOG2 sampling, warm-up, and decision parameters, validated when analysis starts.
struct MotionAnalysisParams
{
    /// Analysis samples per media second, greater than zero and at most 240; default 2.
    double sample_rate_fps = 2;
    /// Resized analysis width in pixels, from 8 to 8192; aspect ratio is preserved; default 320.
    int analysis_frame_width = 320;
    /// Nonnegative background-model coverage required before a valid decision; default 30 seconds.
    double warmup_seconds = 30;
    /// MOG2 background history length, at least 2 sampled frames; default 120.
    int history_frames = 120;
    /// Positive MOG2 variance threshold; default 16.
    double variance_threshold = 16;
    /// MOG2 learning rate from -1 to 1; -1 selects automatic learning (default).
    double learning_rate = -1;
    /// Foreground fraction required for a changed frame, greater than zero and at most 1; default
    /// 0.01.
    double changed_area_threshold = 0.01;
    /// Minimum changed samples required for motion, at least 1; default 2.
    int minimum_frames_with_motion = 2;
    /// Exclude MOG2 shadow labels from changed foreground; default true.
    bool exclude_shadows = true;
    /// Allow ranked hardware decoders with CPU fallback; false by default; does not guarantee GPU
    /// selection.
    bool use_gpu_decoding = false;
    /// Positive wall-time limit for decoding one segment, at most 86400 seconds; default 10.
    double decode_timeout_seconds = 10;
};
/// Local archive destination; recordings are organized by camera and UTC date.
struct LocalDirectoryStorageConfig
{
    /// Required archive root on a local filesystem; files are stored below camera/date
    /// subdirectories.
    std::filesystem::path root_directory;
};
/// Explicit SigV4 credentials; empty values permit AWS environment credential fallback.
struct S3Credentials
{
    /// AWS access key identifier; omitted by to_dict().
    std::string access_key_id;
    /// Secret signing key; omitted by to_dict().
    std::string secret_access_key;
    /// Optional temporary-credential session token; omitted by to_dict().
    std::string session_token;
};
/// S3-compatible single-PUT storage and bounded retry settings.
struct S3StorageConfig
{
    /// Required bucket name; the bucket must already exist.
    std::string bucket_name;
    /// Optional object-key prefix before camera/date/segment identity.
    std::string key_prefix;
    /// SigV4 signing region; default us-east-1.
    std::string region = "us-east-1";
    /// Optional HTTP(S) origin for compatible storage; empty selects the regional AWS S3 endpoint.
    std::string endpoint_url;
    /// Put the bucket in the request path instead of the hostname; default false.
    bool use_path_style_addressing = false;
    /// Verify HTTPS certificates and hostnames; default true.
    bool verify_tls_certificates = true;
    /// Explicit or environment-backed credentials; the entire field is omitted by to_dict().
    S3Credentials credentials;
    /// Release staging after the recorder durably commits upload success; default true.
    bool delete_staging_file_after_upload = true;
    /// Positive timeout for one HTTP request in seconds; default 10.
    double request_timeout_seconds = 10;
    /// Positive initial recorder upload retry delay in seconds; default 1.
    double retry_initial_delay_seconds = 1;
    /// Maximum exponential retry delay, at least the initial delay; default 60 seconds.
    double retry_max_delay_seconds = 60;
};
/// Select exactly one local-directory or S3-compatible storage backend.
using SegmentStorageConfig = std::variant<LocalDirectoryStorageConfig, S3StorageConfig>;
/// Per-camera recording, analysis, durable journal, quota, and storage settings.
struct SegmentRecorderConfig
{
    /// Required camera identity and RTSP connection settings.
    CameraStreamConfig camera;
    /// Local-directory or S3-compatible storage configuration.
    SegmentStorageConfig storage;
    /// Required durable local root for each camera journal and pending video; keep stable across
    /// restarts.
    std::filesystem::path staging_directory;
    /// Positive target duration, at most 86400 seconds; splits occur at keyframes; default 10.
    double segment_duration_seconds = 10;
    /// Output video container; default MP4.
    VideoContainerFormat container_format = VideoContainerFormat::mp4;
    /// Stateful motion-analysis parameters, including unknown-result warm-up.
    MotionAnalysisParams motion_analysis;
    /// Analyze finalized clips on the analysis worker; default true.
    bool analyze_motion_after_segment_closes = true;
    /// Delete only valid negative analyses; requires analysis enabled; default false.
    bool delete_segments_without_motion = false;
    /// Positive initial RTSP reconnect delay in seconds; default 1.
    double reconnect_initial_delay_seconds = 1;
    /// Maximum reconnect backoff, at least the initial delay; default 30 seconds.
    double reconnect_max_delay_seconds = 30;
    /// Connection/inactivity operation timeout in seconds, from 0.001 to 86400; default 10.
    double network_timeout_seconds = 10;
    /// Positive completed-local-video budget, including pending uploads; default 2 GiB; active
    /// video may exceed it.
    std::uint64_t local_video_allowance_bytes = 2ULL * 1024 * 1024 * 1024;
    /// Separate durable journal budget, at least 1 MiB; default 64 MiB; exhaustion fails
    /// explicitly.
    std::uint64_t journal_allowance_bytes = 64ULL * 1024 * 1024;
};
/** Validate recorder and nested motion/storage settings.
 * @param config Settings to validate without opening files or network connections.
 * @throws InvalidConfigError If any supported constraint is violated.
 */
void validate_config(const SegmentRecorderConfig &config);
/** Validate MOG2, sampling, and decode bounds.
 * @param params Motion settings to validate.
 * @throws InvalidConfigError If any motion parameter is invalid.
 */
void validate_motion_params(const MotionAnalysisParams &params);
} // namespace homesight
