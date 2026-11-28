#pragma once
// AvS 2026-11-26 round 10: flamethrower fear meter, fire resistance, shotgun
// stun and the flamethrower kill block. Pure policy (no native calls) so it is
// unit-testable; AlienPvPLocalController feeds it each pilot frame on the
// Alien player's PC. The Alien is invincible: the meter is a "run away" gauge.
//
// User decisions (asked 2026-11-26):
//  * flamed before a chase: meter drains; at 50% the Alien recoils and runs to a
//    vent on its own; backstage it refills to 100% (then exits are allowed).
//    100 -> 50 takes ~1 s of constant flame at first (pass 20); every retreat adds 25%
//    fire resistance (max 75%) so it drains slower and flinches less.
//    Resistance resets per match.
//  * flamed during a chase: retreat at once.
//  * survivor facing the Alien with the flamethrower out within ~5 m: no kill.
//  * shotgun blast at the head within ~6 m: 1 s stun, ~3 s cooldown.
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace aimp::alien_pvp::fear {

// Pass 20 (avsfix19 test: flamed in two bursts, meter only fell to 75%, no run):
// 100 -> 50 now takes ~1 s of constant flame (was 2 s), the open-air regen waits
// 4 s and is slower, and the flame cone/range are wider (stream spread).
struct Tuning final {
    float drainPerSecond{50.0F};      // % per second of full exposure at 0 resistance
    float retreatAt{50.0F};           // meter % that triggers the retreat
    float refillPerSecond{6.25F};     // % per second backstage while recovering (50 -> 100 in 8 s)
    float regenPerSecond{6.0F};       // % per second in the open, after regenDelayMs without flame
    std::uint64_t regenDelayMs{4000U};
    float resistancePerRetreat{0.25F};
    float resistanceMax{0.75F};
    float flameRange{7.0F};           // metres
    float flameHalfAngle{0.60F};      // radians (~34 degrees) around the survivor's facing
    float blockRange{5.0F};           // kill block distance
    float blockHalfAngle{1.05F};      // radians (~60 degrees): "facing the Alien"
    std::uint64_t blockGraceMs{1500U};// flamethrower aimed/fired this recently still blocks
    float shotgunRange{6.0F};
    float shotgunHalfAngle{0.20F};    // radians (~11 degrees): aimed at the head/body
    std::uint64_t stunMs{1000U};
    std::uint64_t stunCooldownMs{3000U};
    std::uint64_t flinchMs{450U};
    std::uint64_t flinchIntervalMs{1500U}; // at most one flinch roll per interval of exposure
};
inline constexpr Tuning kTuning{};
// A flame-retreat EnterVent carries this pitch (the pilot never sends a
// steering pitch; the launcher relays pitch through degrees). The Host then
// accepts a far vent and lets the native vent node path there.
inline constexpr float kRetreatPitch=-1.5F;
inline constexpr float kRetreatReach=25.0F;
inline bool RetreatPitch(float pitch) noexcept { return pitch < -1.4F; }

enum class Phase : std::uint8_t { Normal, Retreating, Recovering };

struct Survivor final {
    float x{}, y{}, z{}, yaw{};
    std::uint8_t weapon{};   // character_action_runtime::RemoteWeapon: 1 flamethrower, 2 shotgun
    bool aim{}, firing{};
};

// Retail yaw convention used by the pilot: forward = (sin yaw, cos yaw).
inline float AngleTo(const Survivor& s, float ax, float az) noexcept {
    const float dx = ax - s.x, dz = az - s.z;
    if (std::hypot(dx, dz) < 0.05F) return 0.0F;
    float d = std::atan2(dx, dz) - s.yaw;
    while (d > 3.14159265F) d -= 6.28318531F;
    while (d < -3.14159265F) d += 6.28318531F;
    return std::abs(d);
}
inline float Planar(const Survivor& s, float ax, float az) noexcept { return std::hypot(ax - s.x, az - s.z); }
inline bool SameFloor(const Survivor& s, float ay) noexcept { return std::abs(ay - s.y) < 2.5F; }

// Flame stream on the Alien this frame: firing a flamethrower, in range, in the cone.
inline bool Flamed(const Survivor& s, float ax, float ay, float az, const Tuning& t = kTuning) noexcept {
    return s.weapon == 1U && s.firing && SameFloor(s, ay) && Planar(s, ax, az) <= t.flameRange &&
        AngleTo(s, ax, az) <= t.flameHalfAngle;
}
// Flamethrower held up (aimed or fired) and facing the Alien up close.
inline bool Threatening(const Survivor& s, float ax, float ay, float az, const Tuning& t = kTuning) noexcept {
    return s.weapon == 1U && (s.aim || s.firing) && SameFloor(s, ay) && Planar(s, ax, az) <= t.blockRange &&
        AngleTo(s, ax, az) <= t.blockHalfAngle;
}
inline bool ShotgunAimed(const Survivor& s, float ax, float ay, float az, const Tuning& t = kTuning) noexcept {
    return s.weapon == 2U && SameFloor(s, ay) && Planar(s, ax, az) <= t.shotgunRange &&
        AngleTo(s, ax, az) <= t.shotgunHalfAngle;
}

struct State final {
    float meter{100.0F};
    float resistance{};
    std::uint32_t retreats{}, flinches{}, stuns{}, blockedClicks{}, chaseRetreats{};
    Phase phase{Phase::Normal};
    std::uint64_t lastFlameAt{}, flinchUntil{}, nextFlinchRollAt{}, stunUntil{}, stunReadyAt{};
    std::uint64_t blockUntil{};
    bool retreatRequested{}; // edge for the controller: start the run to a vent
    bool chaseRetreat{};

    void Reset() noexcept { *this = State{}; }
    [[nodiscard]] bool Stunned(std::uint64_t now) const noexcept { return now < stunUntil || now < flinchUntil; }
    [[nodiscard]] bool KillBlocked(std::uint64_t now) const noexcept { return now < blockUntil; }
    [[nodiscard]] bool ExitAllowed() const noexcept { return phase != Phase::Recovering || meter >= 99.999F; }

    // One pilot frame. roll in [0,1) decides flinches (deterministic in tests).
    // Returns true when a flinch started this frame (controller plays the voice).
    bool Step(std::uint64_t now, float dtSeconds, bool flamed, bool threatened, bool backstage,
        bool chasing, float roll, const Tuning& t = kTuning) noexcept {
        dtSeconds = std::clamp(dtSeconds, 0.0F, 0.25F);
        bool flinched = false;
        if (threatened) blockUntil = now + t.blockGraceMs;
        if (phase == Phase::Normal) {
            if (flamed) {
                lastFlameAt = now;
                meter = std::max(0.0F, meter - t.drainPerSecond * (1.0F - resistance) * dtSeconds);
                if (now >= nextFlinchRollAt) {
                    nextFlinchRollAt = now + t.flinchIntervalMs;
                    if (roll < 1.0F - resistance) { flinchUntil = now + t.flinchMs; ++flinches; flinched = true; }
                }
                if (chasing || meter <= t.retreatAt) {
                    chaseRetreat = chasing && meter > t.retreatAt;
                    if (chaseRetreat) ++chaseRetreats;
                    phase = Phase::Retreating; retreatRequested = true; ++retreats;
                    resistance = std::min(t.resistanceMax, resistance + t.resistancePerRetreat);
                    if (meter > t.retreatAt) meter = t.retreatAt; // a chase flame also needs the refill
                }
            } else if (now - lastFlameAt >= t.regenDelayMs) {
                meter = std::min(100.0F, meter + t.regenPerSecond * dtSeconds);
            }
        } else if (phase == Phase::Retreating) {
            if (backstage) phase = Phase::Recovering;
        }
        if (phase == Phase::Recovering) {
            if (backstage) meter = std::min(100.0F, meter + t.refillPerSecond * dtSeconds);
            if (meter >= 99.999F && !backstage) phase = Phase::Normal; // out again
        }
        return flinched;
    }
    // Retreat run failed (no vent reachable): give control back, keep the meter.
    void RetreatAbandoned() noexcept { if (phase == Phase::Retreating) phase = Phase::Normal; }
    // Shotgun edge: returns true when a stun starts.
    bool Shot(std::uint64_t now, const Tuning& t = kTuning) noexcept {
        if (now < stunReadyAt) return false;
        stunUntil = now + t.stunMs; stunReadyAt = now + t.stunMs + t.stunCooldownMs; ++stuns;
        return true;
    }
};

} // namespace aimp::alien_pvp::fear
