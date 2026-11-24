#pragma once
/** @file clip_frame_sampler.hpp
 * Owning RGB frame samples for downstream application processing.
 */
#include "homesight/segment_info.hpp"
namespace homesight
{
/** Sample evenly spaced media times, center-crop, resize, and return owned RGB pixels.
 * The result remains valid independently of the decoder. This function does not
 * modify or delete the source clip. Each call owns its decoder and output buffers.
 * @param path Existing recording supported by the installed GStreamer decoders.
 * @param frame_count Requested sample count, from 1 to 4096.
 * @param frame_width Output width from 1 to 8192 pixels.
 * @param frame_height Output height from 1 to 8192 pixels.
 * @param use_gpu_decoding Allow ranked hardware decoders with CPU fallback; default false.
 * @param decode_timeout_seconds Positive decode wall-time budget, at most 86400 seconds;
 * default 10.
 * @return RGB bytes and decoded presentation times; allocation is limited to 1 GiB.
 * @throws InvalidConfigError If dimensions, count, timeout, or allocation bounds are invalid.
 * @throws SegmentDecodingError If media is unreadable or decoding/seek exceeds its budget.
 */
[[nodiscard]] SampledClipFrames sample_clip_frames_from_segment(const std::filesystem::path &path,
                                                                int frame_count, int frame_width,
                                                                int frame_height,
                                                                bool use_gpu_decoding = false,
                                                                double decode_timeout_seconds = 10);
/** Sample distinct decoded frames at up to a requested rate, including a partial final second.
 * Each clip-relative second is divided into frames_per_second equal, half-open intervals.
 * The first decoded frame in each occupied interval is retained; empty intervals are not padded.
 * Decoding is sequential. Pixels are center-cropped and resized, and the source is not modified.
 * @param path Existing recording supported by the installed GStreamer decoders.
 * @param frames_per_second Positive integer number of sampling intervals per second.
 * @param frame_width Output width from 1 to 8192 pixels.
 * @param frame_height Output height from 1 to 8192 pixels.
 * @param use_gpu_decoding Allow ranked hardware decoders with CPU fallback; default false.
 * @param decode_timeout_seconds Positive decode wall-time budget, at most 86400 seconds;
 * default 10.
 * @return Owned RGB pixels and actual clip-relative presentation timestamps in milliseconds.
 * The result outlives the decoder and contains at most 4096 frames and 1 GiB of RGB bytes.
 * @throws InvalidConfigError If arguments or actual output allocation exceed their limits.
 * @throws SegmentDecodingError If decoding fails, times regress, no frames exist, or time expires.
 */
[[nodiscard]] SampledClipFrames sample_clip_frames_from_segment_at_rate(
    const std::filesystem::path &path, int frames_per_second, int frame_width, int frame_height,
    bool use_gpu_decoding = false, double decode_timeout_seconds = 10);
} // namespace homesight
