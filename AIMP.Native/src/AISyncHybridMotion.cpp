#include "aimp/AISyncHybridMotion.h"
#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>

namespace aimp::ai_sync::hybrid_motion {
namespace {
constexpr std::uint32_t kBoundary = NativeAction | Discontinuity;
constexpr float kPositionLimit = 1000000.0F;
constexpr float kMaxSpeed = 12.0F;
constexpr float kPi = 3.14159265358979323846F;
constexpr float kTau = 2.0F * kPi;

bool Validate(const Sample& sample) noexcept {
    if (sample.tick == 0 || (sample.flags & Valid) == 0 ||
        (sample.flags & ~(Valid | kBoundary)) != 0 ||
        !std::isfinite(sample.yaw) || std::abs(sample.yaw) > 3.3F) return false;
    for (std::size_t i = 0; i < 3; ++i) {
        if (!std::isfinite(sample.position[i]) || std::abs(sample.position[i]) > kPositionLimit ||
            !std::isfinite(sample.velocity[i]) || std::abs(sample.velocity[i]) > 128.0F) return false;
    }
    return true;
}

bool Same(const Sample& a, const Sample& b) noexcept {
    if (a.tick != b.tick || a.flags != b.flags ||
        std::bit_cast<std::uint32_t>(a.yaw) != std::bit_cast<std::uint32_t>(b.yaw)) return false;
    for (std::size_t i = 0; i < 3; ++i) {
        if (std::bit_cast<std::uint32_t>(a.position[i]) != std::bit_cast<std::uint32_t>(b.position[i]) ||
            std::bit_cast<std::uint32_t>(a.velocity[i]) != std::bit_cast<std::uint32_t>(b.velocity[i])) return false;
    }
    return true;
}

std::uint64_t AddSaturated(std::uint64_t a, std::uint64_t b) noexcept {
    const auto limit = (std::numeric_limits<std::uint64_t>::max)();
    return b > limit - a ? limit : a + b;
}

float Wrap(float yaw) noexcept {
    if (yaw > kPi) yaw -= kTau;
    if (yaw < -kPi) yaw += kTau;
    return yaw;
}

// Velocity of the straight path between two accepted samples, bounded like
// Validate(). Equal ticks never reach here (Push rejects them).
std::array<float, 3> SegmentVelocity(const Sample& left, const Sample& right) noexcept {
    std::array<float, 3> velocity{};
    if (right.tick <= left.tick) return velocity;
    const float scale = 1000.0F / static_cast<float>(right.tick - left.tick);
    for (std::size_t i = 0; i < 3; ++i) {
        const float value = (right.position[i] - left.position[i]) * scale;
        velocity[i] = std::isfinite(value) ? std::clamp(value, -kMaxSpeed, kMaxSpeed) : 0.0F;
    }
    return velocity;
}

Target Pose(const Sample& sample) noexcept {
    Target target{};
    target.position = sample.position;
    // Retain ordinary linear velocity even before a second sample arrives.
    // A zero-velocity first frame otherwise forces retail Move -> Idle -> Move.
    if ((sample.flags & kBoundary) == 0U) target.velocity = sample.velocity;
    target.yaw = sample.yaw;
    target.tick = sample.tick;
    target.nativeAction = (sample.flags & NativeAction) != 0;
    target.discontinuity = (sample.flags & Discontinuity) != 0;
    return target;
}
} // namespace

bool Timeline::Push(const Sample& sample, std::uint64_t arrivalMs) noexcept {
    if (!Validate(sample)) return false;
    if (count_ != 0) {
        const auto& latest = history_[(first_ + count_ - 1) % kHistoryCapacity];
        if (sample.tick < latest.tick) return false;
        // A retransmission is not a new clock observation or a freshness lease.
        if (sample.tick == latest.tick) return Same(sample, latest);
        if (arrivalMs < latestArrivalMs_) return false;
    }

    const bool negative = arrivalMs < sample.tick;
    const auto magnitude = negative ? sample.tick - arrivalMs : arrivalMs - sample.tick;
    const bool smaller = negative != offsetNegative_ ? negative :
        (negative ? magnitude > offsetMagnitude_ : magnitude < offsetMagnitude_);
    if (count_ == 0 || smaller) {
        offsetNegative_ = negative;
        offsetMagnitude_ = magnitude;
    }

    if (count_ != 0) {
        const auto& latest = history_[(first_ + count_ - 1) % kHistoryCapacity];
        if (((sample.flags | latest.flags) & kBoundary) != 0) {
            first_ = 0;
            count_ = 0;
        }
    }
    if (count_ == kHistoryCapacity) {
        first_ = (first_ + 1) % kHistoryCapacity;
        --count_;
    }
    history_[(first_ + count_) % kHistoryCapacity] = sample;
    ++count_;
    latestArrivalMs_ = arrivalMs;
    return true;
}

bool Timeline::SampleAt(std::uint64_t now, Target& out) const noexcept {
    if (count_ == 0) return false;
    const auto& latest = history_[(first_ + count_ - 1) % kHistoryCapacity];
    const auto hostNow = offsetNegative_ ? AddSaturated(now, offsetMagnitude_) :
        (now >= offsetMagnitude_ ? now - offsetMagnitude_ : 0);
    const auto arrivalAge = now >= latestArrivalMs_ ? now - latestArrivalMs_ : 0;
    const auto hostAge = hostNow >= latest.tick ? hostNow - latest.tick : 0;
    if (arrivalAge >= kStaleAfterMs || hostAge >= kStaleAfterMs) {
        out = Pose(latest);
        out.velocity = {};
        out.stale = true;
        return true;
    }
    if ((latest.flags & kBoundary) != 0) {
        out = Pose(latest);
        return true;
    }

    const auto renderTick = hostNow >= kInterpolationDelayMs ? hostNow - kInterpolationDelayMs : 0;
    const auto& earliest = history_[first_];
    if (renderTick <= earliest.tick) {
        out = Pose(earliest);
        return true;
    }
    for (std::size_t i = 1; i < count_; ++i) {
        const auto& right = history_[(first_ + i) % kHistoryCapacity];
        if (renderTick > right.tick) continue;
        const auto& left = history_[(first_ + i - 1) % kHistoryCapacity];
        const auto fraction = static_cast<float>(static_cast<double>(renderTick - left.tick) /
            static_cast<double>(right.tick - left.tick));
        Target target = Pose(left);
        target.tick = renderTick;
        // The rendered path's own derivative. Host finite differences on a
        // 16 ms tick reached 15 m/s for an 8 m/s sprint (diag 194257).
        const auto segment = SegmentVelocity(left, right);
        for (std::size_t axis = 0; axis < 3; ++axis) {
            target.position[axis] = std::lerp(left.position[axis], right.position[axis], fraction);
            target.velocity[axis] = segment[axis];
        }
        target.yaw = Wrap(left.yaw + Wrap(right.yaw - left.yaw) * fraction);
        out = target;
        return true;
    }

    const auto elapsed = renderTick - latest.tick;
    const auto advance = (std::min)(elapsed, kMaxExtrapolationMs);
    Target target = Pose(latest);
    target.tick = AddSaturated(latest.tick, advance);
    const auto velocity = count_ >= 2
        ? SegmentVelocity(history_[(first_ + count_ - 2) % kHistoryCapacity], latest)
        : target.velocity;
    const float seconds = static_cast<float>(advance) / 1000.0F;
    for (std::size_t axis = 0; axis < 3; ++axis) {
        const float projected = latest.position[axis] + velocity[axis] * seconds;
        target.position[axis] = std::clamp(projected, -kPositionLimit, kPositionLimit);
        // Past the extrapolation bound the pose holds: do not keep demanding
        // travel toward a target that no longer advances.
        target.velocity[axis] = elapsed < kMaxExtrapolationMs &&
            projected == target.position[axis] ? velocity[axis] : 0.0F;
    }
    out = target;
    return true;
}

void Timeline::Reset() noexcept {
    first_ = count_ = 0;
    latestArrivalMs_ = offsetMagnitude_ = 0;
    offsetNegative_ = false;
}
} // namespace aimp::ai_sync::hybrid_motion
