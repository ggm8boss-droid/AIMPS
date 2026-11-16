#include "aimp/AnimationSyncVentGround.h"
#include <cmath>
#include <iostream>
#include <stdexcept>

using aimp::animation_sync::VentFootDepth;
using aimp::animation_sync::VentGroundLift;
static void Close(float actual, float expected, const char* message) {
    if (!std::isfinite(actual) || std::abs(actual - expected) > 1.e-5F)
        throw std::runtime_error(message);
}
int main() {
    Close(VentFootDepth(0.F,-0.6F,0.F,0.5F,1.F),0.6F,
        "Late foot below the standing rig should be supported");
    Close(VentFootDepth(0.F,-0.6F,0.F,0.5F,0.F),0.1F,
        "Early foot can trail the original floor");
    Close(VentFootDepth(0.F,0.2F,0.F,0.5F,1.F),0.F,
        "Raised feet should never be pushed higher");
    Close(VentGroundLift(0.F,-0.6F,0.F,0.5F,1.F,0.F),0.06F,
        "First correction must be bounded");
    Close(VentGroundLift(0.F,-0.6F,0.F,0.5F,1.F,0.45F),0.45F,
        "Maximum correction is 45 cm");
    Close(VentGroundLift(0.F,0.F,0.F,0.F,0.F,0.45F),0.415F,
        "Correction release must be smoothed");
    Close(VentGroundLift(0.F,-0.6F,0.F,0.5F,1.F,0.70F),0.45F,
        "A bad stored correction cannot exceed the maximum");
    Close(VentFootDepth(0.F,0.F,0.F,NAN,1.F),0.F,
        "Bad owner data must not force displacement");
    std::cout << "AnimationSync native vent-floor tests passed\n";
}
