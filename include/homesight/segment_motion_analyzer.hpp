#pragma once
/** @file segment_motion_analyzer.hpp
 * Stateful, conservative motion analysis of chronological recordings.
 */
#include "homesight/segment_info.hpp"
#include <memory>
namespace homesight
{
/** Maintain one MOG2 background model across a chronological camera sequence.
 * Calls and reset() are serialized. Changes in continuity, resolution, or supplied
 * media time reset warm-up. Unknown/failed analyses cannot authorize deletion.
 */
class SegmentMotionAnalyzer
{
  public:
    /** Create an independent background model.
     * @param params Validated sampling, MOG2, warm-up, and decode settings.
     * @throws InvalidConfigError If motion parameters are invalid.
     */
    explicit SegmentMotionAnalyzer(MotionAnalysisParams params = {});
    /// Release the owned model after outstanding calls have completed.
    ~SegmentMotionAnalyzer();
    /// Background models have exclusive ownership and cannot be copied.
    SegmentMotionAnalyzer(const SegmentMotionAnalyzer &) = delete;
    /// Assignment cannot copy a background model.
    SegmentMotionAnalyzer &operator=(const SegmentMotionAnalyzer &) = delete;
    /// Discard background history, coverage, resolution, and continuity state.
    void reset();
    /** Decode a complete clip and update the background model at sampled media times.
     * A clip overlapping warm-up remains wholly unknown. Decode/coverage failures
     * reset the model and are returned as failed/insufficient_data diagnostics.
     * @param path Existing recording to decode; this operation does not modify it.
     * @param continuity_id Shared recording-session identity; changes reset the model.
     * @param start_media_seconds Session start time in seconds; a negative value disables
     * cross-clip time checks.
     * @return Owned decision and diagnostics; has_motion is set only when status is valid.
     */
    [[nodiscard]] MotionAnalysisResult analyze_segment_motion(const std::filesystem::path &path,
                                                              const std::string &continuity_id = "",
                                                              double start_media_seconds = -1);

  private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};
/** Analyze one clip using a fresh background model that is discarded afterward.
 * @param path Existing recording; never deleted by this helper.
 * @param params Analysis settings; default warm-up can keep a short clip unknown.
 * @return Motion validity, optional decision, and diagnostics.
 * @throws InvalidConfigError If motion parameters are invalid.
 */
[[nodiscard]] MotionAnalysisResult analyze_segment_motion(const std::filesystem::path &path,
                                                          const MotionAnalysisParams &params = {});
} // namespace homesight
