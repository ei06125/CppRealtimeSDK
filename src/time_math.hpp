// Shared checked arithmetic for the signed nanosecond time domain.
// Avoid release multiplication overflow and saturate counters rather than allowing wraparound.
#pragma once
#include "rtx/scheduler.hpp"
#include <limits>
#include <stdexcept>
namespace rtx::detail {
inline void valid_time(Time value) {
 if(value<Time::zero()) throw std::invalid_argument("monotonic time must be nonnegative");
}
// Find the next phase-aligned release strictly after now without multiplying periods.
inline Time next_release(Time release,Time now,Duration period) {
 valid_time(release);valid_time(now);
 if(now<release || period<=Duration::zero()) throw std::invalid_argument("invalid release clock or period");
 const auto delta=period-(now-release)%period;
 if(now>Time::max()-delta) throw std::overflow_error("release time overflow");
 return now+delta;
}
inline void increment(std::uint64_t& value,std::uint64_t delta=1) {
 const auto maximum=std::numeric_limits<std::uint64_t>::max();
 value=delta>maximum-value?maximum:value+delta;
}
}
