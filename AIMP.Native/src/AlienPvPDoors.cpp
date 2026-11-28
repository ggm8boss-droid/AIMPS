#include "aimp/AlienPvPDoors.h"

#include "aimp/BuildProfile.h"
#include "aimp/CathodePrimitiveExecutor.h"
#include "aimp/CrashGuard.h"
#include "aimp/SignatureResolver.h"
#include "aimp/WorldEntityRegistry.h"

#include <Windows.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <mutex>
#include <vector>

namespace aimp::alien_pvp_doors {
namespace {
// CathodeLib FunctionType.Door (ShortGuid of "Door"), same numbering as the
// manifest primitiveType (PlayEnvironmentAnimation = 1553761290 elsewhere).
constexpr std::uint32_t kDoorType = 0xAF6549BBU;
// Door input "request_open" (dispatcher 009159C0 -> DoorNative vtable +0x398,
// 004E8B00), same ShortGuid as world_animated kRequestOpen.
constexpr std::uint32_t kRequestOpen = 0x8DA169A3U;
// Door execution object (ctor 004A3D00, vptr 011E04F0): DoorNative interface at
// +0x18 (vptr 011E0138), flags at +4 (0x200 = locked: sub_4E6EE0 returns early,
// sub_4A43F0 shows locked_text), interactable prompt slot (FUN_007AFCE0) at +0x28.
constexpr std::size_t kNativeOffset = 0x18U, kFlagsOffset = 0x04U, kSlotOffset = 0x28U;
constexpr std::uint32_t kLockedFlag = 0x200U;
// Interactable slot (shared with PlayerHidingRuntime): +0x40 prompt position,
// +0x4C entity position.
constexpr std::size_t kSlotPromptPosition = 0x40U, kSlotEntityPosition = 0x4CU;
constexpr std::size_t kMaximumDoors = 768U;
constexpr float kOpenRadius = 2.3F, kOpenHeight = 2.5F;
constexpr std::uint64_t kPumpMs = 150U, kReopenMs = 1000U, kPlaceRetryMs = 2000U;

struct Door final {
    WorldEntityId id{};
    float x{}, y{}, z{};
    bool placed{};
    std::uint64_t retryAt{}, openedAt{};
};
std::mutex gMutex;
WorldEntityRegistry* gRegistry{};
const void* gNativeVtable{};
std::vector<Door> gDoors;
std::uint32_t gGeneration{};
std::uint64_t gNextPump{};
Stats gStats{};

template<typename T> bool Read(const void* at, T& value) noexcept {
    return at && crash_guard::TryCopy(&value, at, sizeof(T));
}
bool Finite3(const float* p) noexcept {
    return std::isfinite(p[0]) && std::isfinite(p[1]) && std::isfinite(p[2]) &&
        std::abs(p[0]) < 100000.0F && std::abs(p[1]) < 100000.0F && std::abs(p[2]) < 100000.0F &&
        (p[0] != 0.0F || p[1] != 0.0F || p[2] != 0.0F);
}
// The live execution object is a Door: its DoorNative interface vptr matches.
bool DoorObject(const void* outer) noexcept {
    const void* vtable{};
    return outer && gNativeVtable && Read(static_cast<const std::byte*>(outer) + kNativeOffset, vtable) &&
        vtable == gNativeVtable;
}
bool DoorPosition(const void* outer, float* out) noexcept {
    const std::byte* slot{};
    if (!Read(static_cast<const std::byte*>(outer) + kSlotOffset, slot) || !slot) return false;
    float p[3]{};
    if (Read(slot + kSlotEntityPosition, p) && Finite3(p)) {out[0] = p[0]; out[1] = p[1]; out[2] = p[2]; return true;}
    if (Read(slot + kSlotPromptPosition, p) && Finite3(p)) {out[0] = p[0]; out[1] = p[1]; out[2] = p[2]; return true;}
    return false;
}
// Pass 20 probe: CathodeLib FunctionType ids (Door 0xAF6549BB verified the same
// way) and entity ShortGuids from COMMANDS.PAK (CHALLENGEMAP16): shared
// Archetypes\Misc_Subarchetypes\Hiding_logic and Archetypes\Jobs\VentTraversalMarker.
constexpr std::uint32_t kPlayAnimationType = 0xCD561E51U, kBoxType = 0x51BE8134U, kHoldBreathType = 0xFA864C87U;
constexpr std::uint32_t kLockerKillAlien[] = {0x3CF6B536U, 0x63765EF9U, 0xB46BA55CU, 0x8DDA548AU};
constexpr std::uint32_t kLockerBox = 0xD23502D2U, kHoldBreath = 0xB255938BU, kKilltrapAlien = 0x1E890FBBU;
void Probe() noexcept {
    std::uint64_t anims{}, resolved{}, boxes{}, breath{}, traps{}, trapResolved{}, total{};
    std::vector<std::uint32_t> instances;
    const auto resolveOnce = [](const WorldEntityId& id) noexcept {
        auto r = gRegistry->Resolve(id);
        return r.status == WorldRegistryResolveStatus::Resolved && static_cast<bool>(r.lease);
    };
    try {
        for (const auto& id : gRegistry->FindWorldEntitiesOfPrimitiveType(kPlayAnimationType, 8192U)) {
            ++total;
            const auto key = gRegistry->FindRuntimeWorldEntityKey(id);
            if (!key) continue;
            bool locker{};
            for (const auto guid : kLockerKillAlien) locker = locker || key->entityA == guid;
            if (locker) {
                ++anims;
                if (std::find(instances.begin(), instances.end(), key->entityB) == instances.end())
                    instances.push_back(key->entityB);
                if (anims <= 4U && resolveOnce(id)) ++resolved;
            } else if (key->entityA == kKilltrapAlien) {
                ++traps;
                if (traps <= 4U && resolveOnce(id)) ++trapResolved;
            }
        }
        for (const auto& id : gRegistry->FindWorldEntitiesOfPrimitiveType(kBoxType, 8192U)) {
            const auto key = gRegistry->FindRuntimeWorldEntityKey(id);
            if (key && key->entityA == kLockerBox) ++boxes;
        }
        for (const auto& id : gRegistry->FindWorldEntitiesOfPrimitiveType(kHoldBreathType, 256U)) {
            const auto key = gRegistry->FindRuntimeWorldEntityKey(id);
            if (key && key->entityA == kHoldBreath) ++breath;
        }
    } catch (...) {}
    gStats.lockerKillAnims = anims; gStats.lockerKillInstances = instances.size();
    gStats.lockerKillResolved = resolved; gStats.lockerBoxes = boxes; gStats.holdBreath = breath;
    gStats.killtrapAnims = traps; gStats.killtrapResolved = trapResolved; gStats.playAnimationTotal = total;
}
void Rebuild(std::uint32_t generation) noexcept {
    gDoors.clear();
    gGeneration = generation;
    try {
        for (const auto& id : gRegistry->FindWorldEntitiesOfPrimitiveType(kDoorType, kMaximumDoors))
            gDoors.push_back(Door{id});
    } catch (...) { gDoors.clear(); }
    gStats.known = gDoors.size();
    gStats.placed = 0U;
    Probe();
}
} // namespace

bool Initialize(const SignatureResolver& resolver, WorldEntityRegistry& registry, std::string& error) noexcept {
    error.clear();
    const auto* vtable = resolver.Resolve(SymbolId::CathodeDoorNativeVtable);
    if (vtable == nullptr || !resolver.ExactBuildValidated()) {
        error = "Alien PvP doors: certified DoorNative class unavailable";
        return false;
    }
    std::lock_guard lock(gMutex);
    gNativeVtable = vtable;
    gRegistry = &registry;
    gDoors.clear(); gGeneration = 0U; gStats = {};
    return true;
}

void Shutdown() noexcept {
    std::lock_guard lock(gMutex);
    gRegistry = nullptr; gNativeVtable = nullptr; gDoors.clear(); gGeneration = 0U;
}

void Pump(const std::uintptr_t alien) noexcept {
    const auto now = GetTickCount64();
    std::lock_guard lock(gMutex);
    if (!gRegistry || !gNativeVtable || now < gNextPump) return;
    gNextPump = now + kPumpMs;
    const auto context = gRegistry->ReplicationContext();
    if (!context.manifestLoaded || context.levelGeneration == 0U) {gDoors.clear(); gGeneration = 0U; return;}
    if (context.levelGeneration != gGeneration) Rebuild(context.levelGeneration);
    if (!alien || gDoors.empty()) return;
    // Living, frontstage Alien only (no doors from inside a vent).
    float a[3]{}; std::uint32_t dead{}; std::uint8_t back0{}, back1{};
    const auto* character = reinterpret_cast<const std::byte*>(alien);
    if (!Read(character + 0x70U, a) || !Finite3(a) || !Read(character + 0x2F90U, dead) || dead ||
        !Read(character + 0x2DAU, back0) || !Read(character + 0x2DBU, back1) || back0 || back1) return;

    std::size_t placedNow = 0U, placeBudget = 24U; // bound native resolves per pump
    for (auto& door : gDoors) {
        if (!door.placed) {
            if (now < door.retryAt || placeBudget == 0U) continue;
            --placeBudget;
            door.retryAt = now + kPlaceRetryMs;
            auto resolved = gRegistry->Resolve(door.id);
            float p[3]{};
            if (resolved.status != WorldRegistryResolveStatus::Resolved || !resolved.lease ||
                !DoorObject(resolved.lease.Get()) || !DoorPosition(resolved.lease.Get(), p)) {
                ++gStats.unresolved; continue;
            }
            door.x = p[0]; door.y = p[1]; door.z = p[2]; door.placed = true;
        }
        ++placedNow;
        const float dx = door.x - a[0], dz = door.z - a[2], dy = door.y - a[1];
        const float d2 = dx * dx + dz * dz;
        if (d2 > kOpenRadius * kOpenRadius || std::abs(dy) > kOpenHeight) continue;
        if (door.openedAt && now - door.openedAt < kReopenMs) continue;
        door.openedAt = now;
        auto resolved = gRegistry->Resolve(door.id);
        if (resolved.status != WorldRegistryResolveStatus::Resolved || !resolved.lease ||
            !DoorObject(resolved.lease.Get())) {++gStats.failed; door.placed = false; continue;}
        std::uint32_t flags{};
        if (!Read(static_cast<const std::byte*>(resolved.lease.Get()) + kFlagsOffset, flags)) {++gStats.failed; continue;}
        if ((flags & kLockedFlag) != 0U) {++gStats.locked; continue;}
        const auto result = cathode_primitives::ExecuteInputEdge(
            kRequestOpen, kRequestOpen, 0.0F, resolved.lease, resolved.lease);
        if (result.queued && result.status == cathode_primitives::ExecuteStatus::Queued) {
            ++gStats.opens; gStats.lastDoorM = std::sqrt(d2);
        } else ++gStats.failed;
    }
    gStats.placed = placedNow;
}

Stats Snapshot() noexcept {
    std::lock_guard lock(gMutex);
    return gStats;
}
} // namespace aimp::alien_pvp_doors
