#include "aimp/AlienPvPFear.h"
#include <cstdlib>
#include <iostream>

namespace {
using namespace aimp::alien_pvp::fear;
void Check(bool pass, const char* message) {
    if (!pass) { std::cerr << message << '\n'; std::exit(1); }
}
// Seconds of constant flame (no chase) until the retreat triggers.
float SecondsToRetreat(State& s, std::uint64_t& now) {
    float t = 0.0F;
    while (s.phase == Phase::Normal && t < 30.0F) {
        now += 10U; t += 0.01F;
        static_cast<void>(s.Step(now, 0.01F, true, true, false, false, 0.99F));
    }
    return t;
}
void Recover(State& s, std::uint64_t& now) {
    for (int i = 0; i < 1000 && s.phase != Phase::Normal; ++i) {
        now += 10U;
        const bool backstage = s.meter < 99.999F;
        static_cast<void>(s.Step(now, 0.01F, false, false, backstage, false, 0.99F));
    }
}
} // namespace

int main() {
    // Geometry: survivor at origin facing +Z (yaw 0); Alien 4 m ahead.
    Survivor flame{0, 0, 0, 0, 1, true, true};
    Check(Flamed(flame, 0, 0, 4), "flame in front, in range");
    Check(!Flamed(flame, 0, 0, 7.5F), "flame out of range");
    Check(!Flamed(flame, 4, 0, 0), "flame outside the cone");
    Check(!Flamed(flame, 0, 4, 4), "flame on another floor");
    Check(Threatening(flame, 2, 0, 3), "flamethrower facing within 5 m blocks the kill");
    Check(!Threatening(flame, 0, 0, -3), "from behind the kill is not blocked");
    Survivor holstered{0, 0, 0, 0, 1, false, false};
    Check(!Threatening(holstered, 0, 0, 3), "not aimed/fired: no block");
    Survivor shotgun{0, 0, 0, 0, 2, true, true};
    Check(ShotgunAimed(shotgun, 0, 0, 5) && !ShotgunAimed(shotgun, 0, 0, 7) && !ShotgunAimed(shotgun, 2, 0, 3),
        "shotgun: close + aimed only");

    // Drain timings per retreat (pass 20): ~1 s, ~1.33 s, ~2 s, ~4 s, then capped.
    State s{}; std::uint64_t now = 100000U;
    const float expected[] = {1.0F, 1.333F, 2.0F, 4.0F, 4.0F};
    for (int i = 0; i < 5; ++i) {
        const float t = SecondsToRetreat(s, now);
        Check(t > expected[i] - 0.1F && t < expected[i] + 0.1F, "retreat timing follows the resistance table");
        Check(s.retreatRequested, "retreat edge raised");
        s.retreatRequested = false;
        Check(!s.ExitAllowed() || s.phase == Phase::Retreating, "retreating");
        // Enter the vent: recovering, exit refused until full.
        now += 10U; static_cast<void>(s.Step(now, 0.01F, false, false, true, false, 0.99F));
        Check(s.phase == Phase::Recovering && !s.ExitAllowed(), "exit refused below 100%");
        Recover(s, now);
        Check(s.phase == Phase::Normal && s.meter >= 99.999F, "full again after the refill");
    }
    Check(s.resistance > 0.749F && s.resistance < 0.751F, "resistance capped at 75%");

    // Chase flame: retreat at once and still require the refill.
    State c{}; now = 200000U;
    static_cast<void>(c.Step(now, 0.016F, true, true, false, true, 0.99F));
    Check(c.phase == Phase::Retreating && c.chaseRetreats == 1U && c.meter <= 50.0F, "chase flame retreats at once");

    // Flinch rolls: always at resistance 0 with a low roll, never with roll >= 1-r.
    State f{}; now = 300000U;
    Check(f.Step(now, 0.01F, true, false, false, false, 0.1F), "flinch at 0 resistance");
    Check(f.Stunned(now + 100U) && !f.Stunned(now + 1000U), "flinch is short");
    f.resistance = 0.75F; now += 2000U;
    Check(!f.Step(now, 0.01F, true, false, false, false, 0.3F), "75% resistance: roll 0.3 does not flinch");

    // Regeneration in the open after 4 s without flame.
    State r{}; now = 400000U;
    for (int i = 0; i < 50; ++i) { now += 10U; static_cast<void>(r.Step(now, 0.01F, true, false, false, false, 0.99F)); }
    const float low = r.meter;
    now += 3000U; static_cast<void>(r.Step(now, 0.1F, false, false, false, false, 0.99F));
    Check(r.meter == low, "no regen within 4 s of the last flame");
    now += 1500U; static_cast<void>(r.Step(now, 0.1F, false, false, false, false, 0.99F));
    Check(r.meter > low, "regen after 4 s");

    // Shotgun stun: 1 s, then a 3 s cooldown.
    State g{}; now = 500000U;
    Check(g.Shot(now) && g.Stunned(now + 900U) && !g.Stunned(now + 1100U), "1 s stun");
    Check(!g.Shot(now + 2000U), "cooldown");
    Check(g.Shot(now + 4100U), "stun again after the cooldown");

    // Kill block grace after the flamethrower is lowered.
    State k{}; now = 600000U;
    static_cast<void>(k.Step(now, 0.01F, false, true, false, false, 0.99F));
    Check(k.KillBlocked(now + 1000U) && !k.KillBlocked(now + 1600U), "1.5 s kill-block grace");

    Check(RetreatPitch(kRetreatPitch) && !RetreatPitch(0.0F) && !RetreatPitch(-1.2F), "retreat pitch marker");
    std::cout << "AlienPvPFear OK\n";
    return 0;
}
