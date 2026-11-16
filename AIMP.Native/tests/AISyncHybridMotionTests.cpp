#include "aimp/AISyncHybridMotion.h"
#include "aimp/AISyncRecoveryMotion.h"
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <type_traits>

using namespace aimp::ai_sync::hybrid_motion;
namespace {
void Check(bool value, int line) {
    if (!value) { std::cerr << "hybrid motion invariant at " << line << '\n'; std::abort(); }
}
#define CHECK(...) Check((__VA_ARGS__), __LINE__)
bool Near(float a, float b) { return std::abs(a - b) < 0.0001F; }
void RecoveryIntensity() {
    aimp::ai_sync::State pose{};
    pose.motionFlags = Valid;
    pose.moveType = 1U;
    pose.locomotionIntensity = 1.0F;
    aimp::ai_sync::RecoveryControl control{};
    // Walking and sprint samples from the reported client capture must keep
    // the Host intensity, rather than reusing measured speed as animation input.
    for (const float speed : {0.324928F, 0.986237F, 8.682479F}) {
        pose.motionVelocity[0] = speed;
        CHECK(aimp::ai_sync::TryBuildRecoveryControl(pose, control));
        CHECK(Near(control.velocityX, speed) && Near(control.velocityZ, 0.0F));
        CHECK(Near(control.intensity, 1.0F));
    }
    pose.motionVelocity[0] = 0.0F;
    pose.locomotionIntensity = 0.0F;
    CHECK(aimp::ai_sync::TryBuildRecoveryControl(pose, control) && control.intensity == 0.0F);
    for (const auto flags : {0U, Valid | NativeAction, Valid | Discontinuity}) {
        pose.motionFlags = flags;
        CHECK(!aimp::ai_sync::TryBuildRecoveryControl(pose, control));
    }
    pose.motionFlags = Valid;
    pose.locomotionIntensity = std::numeric_limits<float>::quiet_NaN();
    CHECK(!aimp::ai_sync::TryBuildRecoveryControl(pose, control));
    pose.locomotionIntensity = 1.0F;
    pose.motionVelocity[2] = std::numeric_limits<float>::infinity();
    CHECK(!aimp::ai_sync::TryBuildRecoveryControl(pose, control));
}
Sample Motion(std::uint64_t tick, float x = 0.0F, float vx = 10.0F) {
    return {tick, {x, 2.0F, 3.0F}, {vx, 0.0F, 0.0F}, 0.0F, Valid};
}
Target At(const Timeline& timeline, std::uint64_t now) {
    Target target{};
    CHECK(timeline.SampleAt(now, target));
    return target;
}
void EmptyAndReset() {
    static_assert(std::is_trivially_copyable_v<Timeline>);
    static_assert(noexcept(Timeline{}.Push(Sample{}, 0)));
    static_assert(noexcept(Timeline{}.SampleAt(0, std::declval<Target&>())));
    static_assert(noexcept(Timeline{}.Reset()));
    Timeline timeline;
    Target unchanged{{1, 2, 3}, {4, 5, 6}, 2, 7, true, true, true};
    CHECK(timeline.Count() == 0 && !timeline.SampleAt(0, unchanged));
    CHECK(unchanged.position[0] == 1 && unchanged.velocity[0] == 4 &&
        unchanged.yaw == 2 && unchanged.tick == 7 && unchanged.nativeAction && unchanged.stale);
    CHECK(timeline.Push(Motion(1000), 2000));
    timeline.Reset();
    CHECK(timeline.Count() == 0 && !timeline.SampleAt(10000, unchanged));
    CHECK(timeline.Push(Motion(1, 20), 0)); // New Host epoch and negative offset.
    CHECK(Near(At(timeline, 50).position[0], 20));
}
void ClocksAndInterpolation() {
    // Identical rendering with either sign of offset and near uint64 limits.
    const auto limit = (std::numeric_limits<std::uint64_t>::max)();
    const std::uint64_t hostStarts[]{1000, 10000, limit - 500, 1, 1};
    const std::uint64_t localStarts[]{10000, 1000, 1000, 0, limit - 500};
    for (std::size_t i = 0; i < 5; ++i) {
        Timeline timeline;
        CHECK(timeline.Push(Motion(hostStarts[i], 0), localStarts[i]));
        CHECK(timeline.Push(Motion(hostStarts[i] + 100, 1), localStarts[i] + 100));
        // 100 ms interpolation delay.
        auto target = At(timeline, localStarts[i] + 150);
        CHECK(target.tick == hostStarts[i] + 50 && Near(target.position[0], .5F));
        CHECK(Near(target.velocity[0], 10) && !target.stale && !target.nativeAction);
        target = At(timeline, localStarts[i] + 225);
        CHECK(target.tick == hostStarts[i] + 125 && Near(target.position[0], 1.25F));
    }
    Timeline timeline;
    auto left = Motion(1000); left.yaw = 3.10F;
    auto right = Motion(1100, 1, 20); right.yaw = -3.10F;
    CHECK(timeline.Push(left, 2000) && timeline.Push(right, 2100));
    const auto mid = At(timeline, 2150);
    // Velocity is the rendered segment's derivative, not the sample average.
    CHECK(Near(std::abs(mid.yaw), 3.14159265F) && Near(mid.velocity[0], 10));
    CHECK(At(timeline, 2200).tick == 1100);
    CHECK(Near(At(timeline, 2200).position[0], 1));
    // Reverse wrap takes the equally short route.
    timeline.Reset(); left.yaw = -3.10F; right.yaw = 3.10F;
    CHECK(timeline.Push(left, 2000) && timeline.Push(right, 2100));
    CHECK(Near(std::abs(At(timeline, 2150).yaw), 3.14159265F));
}
void JitterAndDuplicates() {
    Timeline timeline;
    CHECK(timeline.Push(Motion(1000, 0), 2000)); // Offset +1000.
    CHECK(timeline.Push(Motion(1100, 1), 2130)); // +1030 must not replace minimum.
    CHECK(Near(At(timeline, 2130).position[0], .3F));
    CHECK(timeline.Push(Motion(1200, 2), 2190)); // New minimum +990.
    CHECK(Near(At(timeline, 2190).position[0], 1.0F));
    const auto count = timeline.Count();
    CHECK(!timeline.Push(Motion(1199, 900), 2200));
    CHECK(!timeline.Push(Motion(1200, 900), 2200));
    CHECK(!timeline.Push(Motion(1201, 2), 2189)); // Local clock regression.
    auto conflict = Motion(1200, 2); conflict.velocity[1] = -0.0F;
    CHECK(!timeline.Push(conflict, 2200)); // Float bits are part of identity.
    for (std::uint64_t arrival = 2200; arrival <= 3000; arrival += 100) {
        CHECK(timeline.Push(Motion(1200, 2), arrival));
    }
    CHECK(timeline.Count() == count);
    CHECK(!At(timeline, 2789).stale && At(timeline, 2790).stale);
    // Duplicate with a smaller offset must not move the clock estimate either.
    CHECK(timeline.Push(Motion(1200, 2), 1200));
    CHECK(Near(At(timeline, 2190).position[0], 1.0F));
    CHECK(!timeline.Push(Motion(1000), 4000));
    // A newly delivered but already old sample cannot revive old motion.
    CHECK(timeline.Push(Motion(1201, 2), 4000));
    CHECK(At(timeline, 4000).stale && At(timeline, 4000).velocity[0] == 0);
}
void ExtrapolationAndStale() {
    Timeline timeline;
    CHECK(timeline.Push(Motion(1000, 10, 20), 2000));
    CHECK(Near(At(timeline, 2125).position[0], 10.5F));
    const auto cap = At(timeline, 2250); // 150 ms extrapolation bound.
    CHECK(cap.tick == 1150 && Near(cap.position[0], 13.0F) && cap.velocity[0] == 0);
    const auto held = At(timeline, 2599);
    CHECK(!held.stale && held.tick == cap.tick && held.position == cap.position && held.velocity[0] == 0);
    for (const std::uint64_t now : {2600ULL, 3000ULL, 900000ULL}) {
        const auto stale = At(timeline, now);
        CHECK(stale.stale && stale.tick == 1000 && stale.position[0] == 10 &&
            stale.velocity == std::array<float, 3>{});
    }
    CHECK(timeline.Push(Motion(2000, 50), 3000));
    CHECK(!At(timeline, 3000).stale);
    // Saturated host conversion and timestamp addition never wrap backwards.
    timeline.Reset();
    const auto limit = (std::numeric_limits<std::uint64_t>::max)();
    CHECK(timeline.Push(Motion(limit - 200, 1), 0));
    const auto nearLimit = At(timeline, 250);
    CHECK(nearLimit.tick == limit - 100 && Near(nearLimit.position[0], 2.0F));
    CHECK(At(timeline, 600).stale);
}
void NativeActionPath() {
    // Native actions (vent entry/exit, passages) are a continuous path: they
    // keep the 100 ms delay and interpolate instead of chasing the raw sample.
    Timeline timeline;
    CHECK(timeline.Push(Motion(1000, 0), 2000));
    CHECK(timeline.Push(Motion(1100, 1), 2100));
    auto action = Motion(1200, 2); action.flags |= NativeAction;
    CHECK(timeline.Push(action, 2200) && timeline.Count() == 3);
    auto target = At(timeline, 2200); // Render tick 1100: still ordinary.
    CHECK(Near(target.position[0], 1) && !target.nativeAction);
    target = At(timeline, 2250); // 1150: segment into the action.
    CHECK(Near(target.position[0], 1.5F) && Near(target.velocity[0], 10) && target.nativeAction);
    action.tick = 1300; action.position[0] = 3;
    CHECK(timeline.Push(action, 2300) && timeline.Count() == 4);
    target = At(timeline, 2350); // 1250: between two action samples.
    CHECK(Near(target.position[0], 2.5F) && target.nativeAction && !target.stale);
    // Leaving the action keeps the same delay: no backward jump on exit.
    CHECK(timeline.Push(Motion(1400, 4), 2400) && timeline.Count() == 5);
    target = At(timeline, 2400); // 1300.
    CHECK(Near(target.position[0], 3) && target.nativeAction);
    target = At(timeline, 2460); // 1360: action -> ordinary segment.
    CHECK(Near(target.position[0], 3.6F) && target.nativeAction);
    CHECK(timeline.Push(Motion(1500, 5), 2500));
    target = At(timeline, 2550); // 1450: ordinary again.
    CHECK(Near(target.position[0], 4.5F) && !target.nativeAction);
}
void RenderDelay() {
    // Explicit delay: action follow renders near the newest sample.
    Timeline timeline;
    CHECK(timeline.Push(Motion(1000, 0), 2000));
    CHECK(timeline.Push(Motion(1100, 1), 2100));
    Target target{};
    CHECK(timeline.SampleAt(2100, target, kActionInterpolationDelayMs));
    CHECK(target.tick == 1070 && Near(target.position[0], .7F));
    CHECK(timeline.SampleAt(2100, target, 0) && target.tick == 1100 && Near(target.position[0], 1));
    CHECK(timeline.SampleAt(2100, target, 100000) && target.tick == 1000); // Clamped to 500 ms.
    // Ramp: 0.5 ms per ms toward the target, both directions, never overshoot.
    CHECK(Near(StepRenderDelay(100, 30, 20), 90));
    CHECK(Near(StepRenderDelay(35, 30, 20), 30));
    CHECK(Near(StepRenderDelay(30, 100, 40), 50));
    CHECK(Near(StepRenderDelay(100, 100, 16), 100));
    const float nan = std::numeric_limits<float>::quiet_NaN();
    CHECK(Near(StepRenderDelay(nan, 30, 16), 30));
    CHECK(Near(StepRenderDelay(50, 30, -1), 30));
    CHECK(Near(StepRenderDelay(50, nan, 1000), 100));
    CHECK(Near(StepRenderDelay(50, 5000, 100000), 500));
    // A ramp keeps the rendered Host clock monotonic: 16 ms frames while the
    // delay shrinks 100 -> 30 advance the render tick 16..24 ms per frame.
    float delay = 100;
    std::uint64_t previous = 0;
    for (std::uint64_t now = 2000; now <= 2400; now += 16) {
        delay = StepRenderDelay(delay, 30, 16);
        const auto render = now - 1000 - static_cast<std::uint64_t>(delay);
        CHECK(previous == 0 || (render > previous && render - previous <= 24));
        previous = render;
    }
    CHECK(Near(delay, 30));
}
void Barriers() {
    for (const auto flag : {Discontinuity, NativeAction | Discontinuity}) {
        Timeline timeline;
        CHECK(timeline.Push(Motion(1000, 0), 2000));
        CHECK(timeline.Push(Motion(1100, 1), 2100));
        auto barrier = Motion(1200, 900000, 128); barrier.flags |= flag;
        CHECK(timeline.Push(barrier, 2200) && timeline.Count() == 1);
        for (const std::uint64_t now : {2200ULL, 2250ULL, 2500ULL}) {
            const auto target = At(timeline, now);
            CHECK(target.position == barrier.position && target.tick == barrier.tick &&
                target.velocity == std::array<float, 3>{} && !target.stale);
            CHECK(target.nativeAction == ((flag & NativeAction) != 0));
        }
        CHECK(At(timeline, 2800).stale);
        CHECK(timeline.Push(Motion(1300, -900000), 2300) && timeline.Count() == 1);
        CHECK(At(timeline, 2300).position[0] == -900000 && !At(timeline, 2300).nativeAction);
        CHECK(timeline.Push(Motion(1400, -899999), 2400) && timeline.Count() == 2);
        CHECK(Near(At(timeline, 2450).position[0], -899999.5F));
        // Repeated action samples must never become an interpolation segment.
        barrier.tick = 1500;
        CHECK(timeline.Push(barrier, 2500));
        barrier.tick = 1600; barrier.position[0] = 800000;
        CHECK(timeline.Push(barrier, 2600) && timeline.Count() == 1);
        CHECK(At(timeline, 2600).position[0] == 800000);
    }
}
void ValidationAndCapacity() {
    Timeline timeline;
    const auto good = Motion(1000);
    CHECK(timeline.Push(good, 2000));
    const auto reject = [&](const Sample& bad) {
        CHECK(!timeline.Push(bad, 2500) && timeline.Count() == 1);
        CHECK(At(timeline, 2600).stale); // Rejections never refresh the lease.
    };
    auto bad = good; bad.tick = 0; reject(bad);
    bad = good; bad.flags = 0; reject(bad);
    bad = good; bad.flags = 8 | Valid; reject(bad);
    bad = good; bad.flags = NativeAction; reject(bad);
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float inf = std::numeric_limits<float>::infinity();
    for (const float value : {nan, inf, -inf, 3.3001F, -3.3001F}) {
        bad = good; bad.tick++; bad.yaw = value; reject(bad);
    }
    for (std::size_t axis = 0; axis < 3; ++axis) {
        for (const float value : {nan, inf, -inf, 1000001.0F, -1000001.0F}) {
            bad = good; bad.tick++; bad.position[axis] = value; reject(bad);
        }
        for (const float value : {nan, inf, -inf, 128.001F, -128.001F}) {
            bad = good; bad.tick++; bad.velocity[axis] = value; reject(bad);
        }
    }
    timeline.Reset();
    auto edge = Motion(1, 1000000, 128);
    edge.position[1] = -1000000; edge.velocity[1] = -128; edge.yaw = 3.3F;
    CHECK(timeline.Push(edge, 1000));
    const auto clamped = At(timeline, 1200);
    CHECK(clamped.position[0] == 1000000 && clamped.position[1] == -1000000 &&
        clamped.velocity[0] == 0 && clamped.velocity[1] == 0);
    timeline.Reset(); edge.yaw = -3.3F;
    CHECK(timeline.Push(edge, 1000));
    timeline.Reset();
    for (std::uint64_t i = 1; i <= 1000; ++i) {
        CHECK(timeline.Push(Motion(i * 10, static_cast<float>(i)), i * 10 + 1000));
        CHECK(timeline.Count() == (i < kHistoryCapacity ? i : kHistoryCapacity));
    }
    CHECK(Near(At(timeline, 11000).position[0], 990)); // Ring wraps retain order.
    CHECK(!timeline.Push(Motion(100), 11000));
    CHECK(At(timeline, 0).position[0] == 873); // Before oldest retained sample holds.
}
} // namespace

int main() {
    RecoveryIntensity();
    EmptyAndReset();
    ClocksAndInterpolation();
    JitterAndDuplicates();
    ExtrapolationAndStale();
    NativeActionPath();
    RenderDelay();
    Barriers();
    ValidationAndCapacity();
    std::cout << "AISyncHybridMotionTests passed\n";
}
