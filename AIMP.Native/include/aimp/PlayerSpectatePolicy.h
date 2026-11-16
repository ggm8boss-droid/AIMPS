#pragma once

#include <cstddef>
#include <cstdint>

namespace aimp::player_lifecycle::spectate {

// Adapted from alien-isolation-coop (coopnet follow_step): once the local
// player is down, watch a living participant. Pure policy, engine-free.
inline constexpr std::uint64_t kStartDelayMs = 3000U;   // let the kill/death camera play first
inline constexpr std::uint64_t kReassertMs = 250U;      // re-activate if a script camera took over
inline constexpr std::size_t kMaxCandidates = 32U;

// Candidates are living remote participants in ascending peer order. Keep the
// current target while it is a candidate; otherwise take the next peer after
// it (wrapping), so a death moves the view to the following player.
[[nodiscard]] constexpr std::uint64_t KeepOrNext(const std::uint64_t current,
    const std::uint64_t* peers, const std::size_t count) noexcept {
    if (count == 0U) return 0U;
    for (std::size_t i = 0U; i < count; ++i) if (peers[i] == current) return current;
    for (std::size_t i = 0U; i < count; ++i) if (peers[i] > current) return peers[i];
    return peers[0];
}

// Left/right arrows: step = -1 or +1 through the sorted candidates (wrapping).
[[nodiscard]] constexpr std::uint64_t Cycle(const std::uint64_t current,
    const std::uint64_t* peers, const std::size_t count, const int step) noexcept {
    if (count == 0U) return 0U;
    std::size_t index = count;
    for (std::size_t i = 0U; i < count; ++i) if (peers[i] == current) index = i;
    if (index == count) return KeepOrNext(current, peers, count);
    if (step > 0) return peers[(index + 1U) % count];
    if (step < 0) return peers[(index + count - 1U) % count];
    return current;
}

} // namespace aimp::player_lifecycle::spectate
