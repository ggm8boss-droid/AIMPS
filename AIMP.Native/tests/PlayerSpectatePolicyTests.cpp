#include "aimp/PlayerSpectatePolicy.h"

#include <cstdint>
#include <iostream>

namespace {
int failures{};
void Check(const bool result, const char* name) {
    if (!result) { std::cerr << name << '\n'; ++failures; }
}
}

int main() {
    using namespace aimp::player_lifecycle::spectate;
    constexpr std::uint64_t peers[]{10U, 20U, 30U};
    Check(KeepOrNext(0U, peers, 0U) == 0U, "No candidate: nobody to watch");
    Check(KeepOrNext(0U, peers, 3U) == 10U, "First view: lowest peer");
    Check(KeepOrNext(20U, peers, 3U) == 20U, "Living target is kept");
    Check(KeepOrNext(25U, peers, 3U) == 30U, "Dead target: next peer");
    Check(KeepOrNext(35U, peers, 3U) == 10U, "Dead last target: wraps to first");
    Check(Cycle(10U, peers, 3U, 1) == 20U, "Right arrow: next player");
    Check(Cycle(30U, peers, 3U, 1) == 10U, "Right arrow wraps");
    Check(Cycle(10U, peers, 3U, -1) == 30U, "Left arrow wraps");
    Check(Cycle(20U, peers, 3U, -1) == 10U, "Left arrow: previous player");
    Check(Cycle(20U, peers, 1U, 1) == 10U, "Single candidate replaces a dead target");
    Check(Cycle(10U, peers, 1U, -1) == 10U, "Single candidate stays");
    Check(Cycle(15U, peers, 3U, 1) == 20U, "Unknown target falls back to next");
    if (failures == 0) std::cout << "PlayerSpectatePolicy: all checks passed\n";
    return failures == 0 ? 0 : 1;
}
