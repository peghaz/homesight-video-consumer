#pragma once
#include <algorithm>
#include <cmath>
#include <cstddef>

namespace homesight::detail
{
/// Observed presentation intervals, independent of a container's nominal duration.
class TimelineCoverage
{
  public:
    void observe(double seconds, double duration_seconds)
    {
        if (!std::isfinite(seconds) || !std::isfinite(duration_seconds) || seconds < 0 ||
            duration_seconds <= 0)
        {
            valid_ = false;
            return;
        }
        tolerance_ = std::max(0.002, duration_seconds * 0.25);
        if (frames_ == 0)
        {
            first_ = seconds;
        }
        else if (seconds < last_ || std::abs(seconds - end_) > tolerance_)
        {
            valid_ = false;
        }
        last_ = seconds;
        end_ = seconds + duration_seconds;
        ++frames_;
    }
    [[nodiscard]] bool is_complete(double nominal_duration_seconds) const
    {
        return valid_ && frames_ > 0 && std::isfinite(nominal_duration_seconds) &&
               nominal_duration_seconds > 0 && first_ <= tolerance_ &&
               std::abs(nominal_duration_seconds - end_) <= tolerance_;
    }
    [[nodiscard]] double observed_seconds() const
    {
        return std::max(0.0, end_ - first_);
    }

  private:
    bool valid_ = true;
    std::size_t frames_ = 0;
    double first_ = 0;
    double last_ = 0;
    double end_ = 0;
    double tolerance_ = 0.002;
};
} // namespace homesight::detail
