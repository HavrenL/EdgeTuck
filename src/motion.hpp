#pragma once
#include <algorithm>
#include <cmath>
#include <chrono>

namespace edge {
// Animation samples need sub-frame precision on high-refresh displays.
// steady_clock uses the Windows performance counter, without changing system
// timer resolution or adding a ticking thread.
using MotionClock = std::chrono::steady_clock;
inline float motion_elapsed(MotionClock::time_point start,MotionClock::time_point now=MotionClock::now()) {
    return std::chrono::duration<float,std::milli>(now-start).count();
}
struct Offset { float x{}, y{}; };
// Same physical drawer at both ends of the motion. Only its leading edge peeks out.
inline Offset closed_offset(int side, float width, float height, float peek) {
    if (side == 0) return {-(width - peek), 0};
    if (side == 1) return {width - peek, 0};
    return {0, -(height - peek)};
}
inline float motion_progress(float from, float to, float elapsed_ms, float duration_ms) {
    const float t = std::clamp(elapsed_ms / std::max(1.0f, duration_ms), 0.0f, 1.0f);
    return from + (to - from) * (1.0f - std::pow(1.0f - t, 3.0f));
}
inline Offset offset_at(Offset closed, float progress) {
    return {closed.x * (1.0f - progress), closed.y * (1.0f - progress)};
}
inline float drawer_content_opacity(float width,float height,Offset offset,float peek) {
    return std::clamp(std::min(width-std::abs(offset.x)-peek,height-std::abs(offset.y)-peek)/std::max(peek,1.0f),0.0f,1.0f);
}
}
