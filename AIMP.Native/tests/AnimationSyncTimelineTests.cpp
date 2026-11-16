#include "aimp/AnimationSyncTimeline.h"

#include <cmath>
#include <iostream>
#include <stdexcept>

namespace {
void Require(bool condition, const char* reason) {
    if (!condition) throw std::runtime_error(reason);
}
void Phase(float actual, float expected, const char* reason) {
    Require(std::isfinite(actual) && std::abs(actual - expected) <= 0.00001F, reason);
}
}

int main() {
    using aimp::animation_sync::State;
    namespace timeline = aimp::animation_sync::timeline;
    State s{};
    s.transition = 1;
    s.animation = 1;
    s.durationSeconds = 10.0F;
    s.phaseSeconds = 2.0F;
    float phase{};
    std::int32_t remaining{};
    Require(timeline::Project(s, 1500U, phase, remaining) && remaining == 1,
        "Delayed one-shot projection did not preserve remaining cycle count");
    Phase(phase, 3.5F, "Delayed one-shot phase");
    Require(!timeline::Project(s, 8000U, phase, remaining),
        "An exhausted one-shot must not restart");

    s.loops = 3;
    s.phaseSeconds = 9.0F;
    Require(timeline::Project(s, 2500U, phase, remaining) && remaining == 2,
        "Finite loop must decrement the native remaining count");
    Phase(phase, 1.5F, "Finite loop phase wrapping");
    Require(!timeline::Project(s, 21000U, phase, remaining),
        "Exhausted finite loop count must not replay");

    s.startFrame = 60; // 2.0 seconds
    s.endFrame = 150;  // 5.0 seconds
    s.phaseSeconds = 4.0F;
    Require(timeline::Project(s, 2500U, phase, remaining) && remaining == 2,
        "Trimmed loop must decrement remaining count on wrap");
    Phase(phase, 3.5F, "Trimmed native clip phase");
    s.loops = -1;
    s.speed = 2.0F;
    Require(timeline::Project(s, 300000U, phase, remaining) && remaining == -1,
        "Indefinite native clip must remain live");
    Phase(phase, 4.0F, "Indefinite clip phase");
    s.loops = 0;
    Require(!timeline::Project(s, 0U, phase, remaining),
        "A zero remaining count must be completed");
    s.loops = 1;
    s.endFrame = 30;
    Require(!timeline::Project(s, 0U, phase, remaining),
        "Invalid empty trim range must be rejected");
    s.endFrame = -1;
    s.durationSeconds = 0.0F;
    Require(!timeline::Project(s, 0U, phase, remaining),
        "Zero clip duration must be rejected");
    std::cout << "AnimationSync native timeline projection tests passed\n";
    return 0;
}
