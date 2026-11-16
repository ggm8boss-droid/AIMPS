#pragma once
#include <array>
#include <cstddef>
#include <cstdint>

namespace aimp::ai_sync::hybrid_motion {
inline constexpr std::uint32_t Valid = 1U;
inline constexpr std::uint32_t NativeAction = 2U;
inline constexpr std::uint32_t Discontinuity = 4U;
inline constexpr std::size_t kHistoryCapacity = 128;
inline constexpr std::uint64_t kInterpolationDelayMs = 100;
// Host-owned native actions (vent entry/exit, passages) are replayed natively
// on the Client the moment their ordered command arrives, i.e. in phase with
// the newest pose, not 100 ms behind it. Follow them with a short buffer only.
inline constexpr std::uint64_t kActionInterpolationDelayMs = 30;
inline constexpr std::uint64_t kMaxInterpolationDelayMs = 500;
inline constexpr std::uint64_t kMaxExtrapolationMs = 150;
inline constexpr std::uint64_t kStaleAfterMs = 600;
// The render delay moves at most this many ms per elapsed ms, so the rendered
// Host clock runs at 0.5x..1.5x while it changes: no forward/backward jump
// when a Character enters or leaves a native action.
inline constexpr float kRenderDelayRampPerMs = 0.5F;

// Moves the current render delay toward the target delay. Invalid input
// returns the target (fail to the known contract, never NaN).
[[nodiscard]] float StepRenderDelay(float currentMs, float targetMs, float elapsedMs) noexcept;

struct Sample {
    std::uint64_t tick{};
    std::array<float, 3> position{}, velocity{};
    float yaw{};
    std::uint32_t flags{};
};
struct Target {
    std::array<float, 3> position{}, velocity{};
    float yaw{};
    std::uint64_t tick{};
    bool nativeAction{}, discontinuity{}, stale{};
};

// Desired poses only: no actor writes, native actions, speed correction or game calls.
// Both clocks are monotonic milliseconds (not UTC); callers serialize access and
// supply nondecreasing arrivalMs/now. Reset for a new Host clock/session/actor.
// Offset is min(arrivalMs - Host tick), represented without signed overflow.
// This includes the minimum observed delivery delay; it is NOT true latency or
// clock synchronization. Clock drift and changes of clock epoch need caller care.
class Timeline {
public:
    // Latest bit-identical duplicate succeeds without changing history/offset/lease.
    // Older ticks, conflicting duplicates and invalid values fail without mutation.
    // New samples also reject regressing arrival clocks. Discontinuity
    // boundaries clear history on entry, throughout the boundary, and on exit.
    // NativeAction samples are a continuous path and stay in the history.
    [[nodiscard]] bool Push(const Sample&, std::uint64_t arrivalMs) noexcept;
    // Empty: false, out unchanged. Stale: latest accepted pose, zero velocity;
    // repeated stale queries never extrapolate.
    // Motion (ordinary or NativeAction) interpolates with 100ms delay and
    // extrapolates at most 150ms; a segment touching an action sample reports
    // nativeAction. A Discontinuity pose holds until an ordinary successor
    // starts its segment. The ordinary locomotion follower must honor
    // stale/nativeAction; the action follower uses the smoothed action path.
    [[nodiscard]] bool SampleAt(std::uint64_t now, Target& out) const noexcept;
    // Same contract with an explicit interpolation delay (clamped to
    // kMaxInterpolationDelayMs). SampleAt(now, out) uses kInterpolationDelayMs.
    [[nodiscard]] bool SampleAt(std::uint64_t now, Target& out, std::uint64_t delayMs) const noexcept;
    void Reset() noexcept;
    [[nodiscard]] std::size_t Count() const noexcept { return count_; }

private:
    std::array<Sample, kHistoryCapacity> history_{};
    std::size_t first_{}, count_{};
    std::uint64_t latestArrivalMs_{}, offsetMagnitude_{};
    bool offsetNegative_{};
};
} // namespace aimp::ai_sync::hybrid_motion
