#include "homesight/segment_motion_analyzer.hpp"
#include "homesight/errors.hpp"
#include "media.hpp"
#include "timeline_coverage.hpp"
#include <cmath>
#include <mutex>
#include <opencv2/imgproc.hpp>
#include <opencv2/video/background_segm.hpp>

namespace homesight
{
class SegmentMotionAnalyzer::Impl
{
  public:
    MotionAnalysisParams params;
    cv::Ptr<cv::BackgroundSubtractorMOG2> model;
    cv::Size resolution;
    std::mutex mutex;
    std::string continuity;
    double covered_seconds = 0;
    double expected_start = -1;
    explicit Impl(MotionAnalysisParams p) : params(p)
    {
        reset();
    }
    void reset()
    {
        model = cv::createBackgroundSubtractorMOG2(params.history_frames, params.variance_threshold,
                                                   true);
        covered_seconds = 0;
        expected_start = -1;
        resolution = {};
        continuity.clear();
    }
};
SegmentMotionAnalyzer::SegmentMotionAnalyzer(MotionAnalysisParams params)
{
    validate_motion_params(params);
    impl_ = std::make_unique<Impl>(params);
}
SegmentMotionAnalyzer::~SegmentMotionAnalyzer() = default;
void SegmentMotionAnalyzer::reset()
{
    std::lock_guard lock(impl_->mutex);
    impl_->reset();
}
MotionAnalysisResult
SegmentMotionAnalyzer::analyze_segment_motion(const std::filesystem::path &path,
                                              const std::string &continuity, double start)
{
    std::lock_guard lock(impl_->mutex);
    auto &p = impl_->params;
    MotionAnalysisResult result;
    result.params_used = p;
    if (continuity != impl_->continuity || (start >= 0 && impl_->expected_start >= 0 &&
                                            std::abs(start - impl_->expected_start) > 0.75))
    {
        impl_->reset();
    }
    impl_->continuity = continuity;
    try
    {
        detail::MediaReader reader(path, p.use_gpu_decoding, p.decode_timeout_seconds);
        double duration = reader.duration_seconds();
        double next_sample = 0;
        detail::TimelineCoverage timeline;
        double coverage_start = 0;
        double previous_sample = -1;
        bool complete_warmup_coverage = impl_->covered_seconds >= p.warmup_seconds;
        while (auto frame = reader.read_frame())
        {
            timeline.observe(frame->seconds, frame->duration_seconds);
            if (frame->seconds + 1e-6 < next_sample)
            {
                continue;
            }
            next_sample = frame->seconds + 1 / p.sample_rate_fps;
            if (impl_->resolution != frame->rgb.size() ||
                (previous_sample >= 0 && frame->seconds - previous_sample > 2 / p.sample_rate_fps))
            {
                impl_->reset();
                impl_->continuity = continuity;
                impl_->resolution = frame->rgb.size();
                complete_warmup_coverage = p.warmup_seconds == 0;
                coverage_start = frame->seconds;
            }
            previous_sample = frame->seconds;
            int height = std::max(1, frame->rgb.rows * p.analysis_frame_width / frame->rgb.cols);
            cv::Mat resized, foreground;
            cv::resize(frame->rgb, resized, {p.analysis_frame_width, height}, 0, 0, cv::INTER_AREA);
            impl_->model->apply(resized, foreground, p.learning_rate);
            cv::Mat changed;
            cv::compare(foreground, p.exclude_shadows ? 255 : 0, changed,
                        p.exclude_shadows ? cv::CMP_EQ : cv::CMP_GT);
            double area = static_cast<double>(cv::countNonZero(changed)) /
                          static_cast<double>(changed.total());
            result.peak_changed_area = std::max(result.peak_changed_area, area);
            result.mean_changed_area += area;
            ++result.frames_analyzed;
            if (area >= p.changed_area_threshold)
            {
                ++result.frames_with_motion;
            }
        }
        if (!timeline.is_complete(duration))
        {
            impl_->reset();
            result.status = MotionAnalysisStatus::insufficient_data;
            result.error = "Decoded presentation timeline does not cover the complete segment";
            return result;
        }
        impl_->covered_seconds += std::max(0.0, timeline.observed_seconds() - coverage_start);
        impl_->expected_start = start < 0 ? -1 : start + duration;
        if (result.frames_analyzed > 0)
        {
            result.mean_changed_area /= result.frames_analyzed;
        }
        if (result.frames_analyzed < p.minimum_frames_with_motion)
        {
            result.status = MotionAnalysisStatus::insufficient_data;
        }
        else if (!complete_warmup_coverage)
        {
            result.status = MotionAnalysisStatus::warming_up;
        }
        else
        {
            result.status = MotionAnalysisStatus::valid;
            result.has_motion = result.frames_with_motion >= p.minimum_frames_with_motion;
        }
    }
    catch (const std::exception &error)
    {
        impl_->reset();
        result.status = MotionAnalysisStatus::failed;
        result.error = error.what();
        result.has_motion.reset();
    }
    return result;
}
MotionAnalysisResult analyze_segment_motion(const std::filesystem::path &path,
                                            const MotionAnalysisParams &params)
{
    return SegmentMotionAnalyzer(params).analyze_segment_motion(path);
}
} // namespace homesight
