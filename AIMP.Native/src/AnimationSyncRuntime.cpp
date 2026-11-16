#include "aimp/AnimationSyncRuntime.h"
#include "aimp/AnimationSyncTimeline.h"
#include "aimp/AnimationSyncVisualRoot.h"
#include "aimp/AnimationSyncVentGround.h"
#include "aimp/AnimationSyncPropsRuntime.h"
#include "aimp/AISyncNativeIntent.h"
#include "aimp/CharacterDirectStepRuntime.h"
#include "aimp/CharacterResourceAdmission.h"
#include "aimp/CrashGuard.h"
#include "aimp/Diagnostics.h"
#include "aimp/HookManager.h"
#include "aimp/PlayerHidingPresentationRuntime.h"
#include "aimp/PlayerHidingLocalCapture.h"
#include "aimp/PlayerHidingRuntime.h"

#include <Windows.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <bitset>
#include <cmath>
#include <new>
#include <cstring>
#include <initializer_list>
#include <iomanip>
#include <limits>
#include <locale>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

// Same native contract as PlayerHidingPresentationRuntime (see
// analysis/hiding-presentation/native-player-contract.md): CMD_PlayAnimation
// entities (0067FC40) construct the target Character's +36C cycle through
// 006579F0 -> 006568B0, which copies 22 parameter words to cycle+44. The
// owner's cycle therefore already names the clip alias (cycle+6C) and its
// timeline; the observer replays that alias on its own representation.
namespace aimp::animation_sync {
namespace {
constexpr std::uintptr_t kScriptedType = 0xEADB58U;
constexpr std::uint8_t kScriptedState = 0x12U;
// Timeouts concern observations, never the legitimate duration of an action.
constexpr std::uint64_t kObservationTimeoutMs = 5000U;
constexpr std::uint64_t kMaximumStartAgeMs = 1500U;
// After the owner's clip ends, a one-shot keeps playing for the network delay.
constexpr std::uint64_t kClearGraceMs = 750U;
constexpr std::size_t kSlots = 40U;

struct Cycle final {
    std::uintptr_t address{}, type{}, owner{}, graph{};
    std::uint32_t runtime{}, resource{}, animation{};
    std::uint8_t state{}, serial{}, flags{};
};
// Read-only probe of the representation's 8 animation pose layers
// (anim controller at Character+0x3F0: +0x210+L*4 skeleton holder, +0x230+L*4
// pose, pose+4 = local transforms, pose+0x10 = derived MODEL cache). It answers whether a base
// idle/locomotion layer keeps being evaluated while the interaction clip plays.
struct LayerProbe final {
    std::uintptr_t pose{};
    std::uint32_t hash{}, bones{}, samples{}, changes{};
};
using LayerProbes = std::array<LayerProbe, 8>;
struct RemoteDiagnostics final {
    LayerProbes idle{}, before{}, during{};
    std::uint32_t controllerFlags{}, scriptedPose{};
    // Launcher bone-mask descriptor of the replayed clip (stage 1: reported only).
    std::uint32_t maskStatus{0xFFFFFFFFU}, maskSkeleton{}, maskTracks{}, maskMoving{}, maskHeld{};
    std::uint32_t maskTarget{}, maskCrewBones{}, maskCrewDriven{};
    std::uint64_t maskNoBase{};
    std::uint64_t applied{}, failed{}, stale{}, unavailable{}, interrupted{}, completed{}, cancelled{}, deferred{};
    std::uint64_t transition{};
    std::uint32_t animation{}, resource{}, clipPath{};
    const char* clipSource{"none"};
    const char* stage{"pending"};
    // Owner clock following: seeks back onto the owner's phase, and replays
    // restarted because ours ended while the owner's same clip still ran.
    std::uint64_t resynced{}, readmitted{};
    // Native Character displacement, measured before each engine animation tick.
    // This detects whether retail actually applies the clip's reference motion.
    std::uint64_t nativeRootSamples{}, nativeRootDiscontinuities{};
    float nativeRootDistance{}, nativeOwnerGap{}, nativeOwnerGapMax{}, nativeAnchorDrift{};
    // Spatial flight samples are diagnostic-only, retained per remote actor.
    // TickMs shares the native locomotion flight recorder's timebase.
    struct SpatialSample final {
        std::uint64_t tickMs{}, transition{};
        std::uint32_t clipPath{};
        float phase{-1.0F}, characterYaw{}, ownerYaw{};
        std::array<float, 3> character{}, owner{};
        bool hasOwner{};
    };
    std::array<SpatialSample, 64> spatial{};
    std::uint32_t spatialNext{};
    // Last replays: how and where (our clock vs owner's) each one ended.
    struct Ending final {
        std::uint64_t transition{};
        std::uint32_t animation{};
        std::int32_t start{}, end{}, loops{};
        float ours{-1.0F}, owner{-1.0F}, duration{};
        const char* result{};
    };
    std::array<Ending, 12> endings{};
    std::uint32_t endingNext{};
};
struct Slot final {
    std::uintptr_t character{};
    State desired{};
    std::uint64_t binding{}, epoch{}, highest{}, handled{}, receivedAt{}, clearedAt{}, startedAt{};
    std::int32_t ownedLoops{};
    // Inventory slot holstered for the clip (-1 = none); drawn back after it.
    std::int32_t holstered{-1};
    // The alias our replay of desired plays: the player's exact clip
    // (ReplayAlias) when the representation's database has it, else desired's.
    std::uint32_t play{};
    bool nativeClip{}; // Distinct original, full-body NPC animation.
    // Retain the earliest observation of an action. A later stream-ready
    // admission must not place the NPC at the owner's already-traversed
    // position and then play that same root-motion path a second time.
    std::array<float, 3> firstOwnerPosition{}, nativeRootLastPosition{};
    float firstOwnerYaw{};
    bool hasFirstOwnerPosition{}, nativeRootLastValid{};
    std::uint64_t lastSpatialAt{};
    // Crew bones the owned clip drives; the others keep the last idle pose.
    bool maskActive{};
    std::bitset<256> crewDriven{}, crewDisplaced{};
    // Crew bones the owner's body effectors drive (-1 = no posture rebuild).
    std::array<std::int16_t, 69> crewPose{};
    // Holster-first: the clip starts once the weapon is away and the unarmed
    // idle has run a few frames, so untouched limbs keep a natural pose. The
    // wait is not counted as owner progress (the replay runs that much later).
    std::uint64_t holsterStarted{}, unarmedAt{}, holsterWait{};
    // The owner's player position sits a few cm below where a crew
    // representation stands; the clip keeps the representation's grounding.
    float heightOffset{}, ownerStartY{}, ownedYaw{};
    // Owner clip clock rate from successive refreshes (-1 = unknown): a
    // staged interaction may hold its clip while waiting (LEDs, generator).
    float ownerRate{-1.0F}, rateSamplePhase{};
    std::uint64_t rateSampleAt{}, resyncAt{};
    std::uint32_t readmits{};
    Cycle owned{};
    RemoteDiagnostics diagnostics{};
};
struct LocalCapture final {
    std::uintptr_t character{}, cycle{};
    std::uint32_t runtime{}, animation{};
    std::uint8_t serial{};
    std::uint64_t startedAt{};
    State state{};
    // Exact local hiding edge already represented by a native or bridged clip.
    std::uint64_t hidingTransition{};
};
struct LocalDiagnostics final {
    std::uint64_t captured{}, cleared{}, hidingSkipped{}, fallbackCaptured{}, torn{}, unavailable{}, posed{};
    std::uint32_t animation{}, resource{};
};
struct Stream final {
    std::uintptr_t character{};
    std::uint64_t binding{}, transition{};
    std::uint32_t animation{};
    std::unique_ptr<native_resource::StrongLease> database;
    bool acquired{};
};

std::atomic<std::uintptr_t> gBase{};
std::atomic<std::uintptr_t> gIdleTransition{};
std::mutex gMutex, gLocalMutex;
std::array<Slot, kSlots> gSlots{};
std::uint64_t gEpoch{};
// Locker entry/hidden/exit: the owner's live Character transform keyed by its
// clip time (gMutex, indexed like gSlots). The first-person clip's movement is
// that transform, not its pose.
struct HidingSample final { float time{}; std::array<float, 3> position{}; float yaw{}; };
struct HidingTrack final {
    std::uint64_t transition{};
    std::uint32_t count{};
    bool played{};
    std::array<HidingSample, 256> samples{};
};
// [0] entry, [1] hidden, [2] exit: the next phase arrives while the previous
// clip still plays here.
using HidingTracks = std::array<HidingTrack, 3>;
std::array<HidingTracks, kSlots> gTracks{};
void Record(const std::size_t index, const State& state) noexcept {
    const auto phase = player_hiding_presentation::ClassifyClip(state.animation).phase;
    if (phase == 0U) return;
    auto& tracks = gTracks[index];
    auto& track = tracks[phase - 1U];
    if (track.transition != state.transition) {
        if (phase == 1U) tracks = {}; // a new locker sequence
        track.transition = state.transition; track.count = 0U; track.played = false;
    }
    const HidingSample sample{state.phaseSeconds, state.position, state.yaw};
    if (track.count != 0U && (sample.time <= track.samples[track.count - 1U].time + 0.0001F ||
            track.count == track.samples.size())) track.samples[track.count - 1U] = sample;
    else track.samples[track.count++] = sample;
}
// Engine thread only.
LocalCapture gLocal{};
std::uint64_t gLocalTransition{};
// gLocalMutex.
LocalSnapshot gLocalSnapshot{};
LocalDiagnostics gLocalDiagnostics{};
// Engine-thread owned, process lifetime: see PlayerHidingPresentationRuntime.
// StrongLease destructors must never run on the loader thread.
auto& gStreams = *new std::array<Stream, kSlots>{};
// Engine-thread list: value-only Forget may run on IPC/lifecycle threads;
// native prop references are retired on the next natural engine pump.
std::array<std::uintptr_t, kSlots> gPropCharacters{};

// --- Launcher bone masks (<game>/AIMP/AnimationSync/bone-masks.bin) -------
// Written by AnimationSyncBoneMasks.cs during preparation: per clip, each source
// bone's motion and distance to its reference pose, plus the engine's own
// SKELE\MAPS retarget tables. A replayed first-person clip only drives the crew
// bones it really animates or holds away from its reference pose; every other
// crew bone keeps the representation's last idle pose. Without a valid file or
// map, the clip plays unmasked as before.
constexpr std::size_t kMaskBones = 256U;
using BoneSet = std::bitset<kMaskBones>;
// driven: bones whose rotation the clip owns. displaced: bones the clip really
// moves (> kDisplacedTranslation); every other bone keeps the representation's
// own translation (bone lengths, root and first-person camera/hip height).
struct MaskClip final { std::uint32_t path{}, status{}, skeleton{}, tracks{}, moving{}, held{}; BoneSet driven{}, displaced{}; };
struct MaskMap final { std::uint32_t source{}, target{}, targetBones{}; std::vector<std::pair<std::int16_t, std::int16_t>> pairs; };
std::vector<MaskClip> gMaskClips;
std::vector<MaskMap> gMaskMaps;
std::int32_t gMaskFile{-1}; // -1 absent/invalid, otherwise clip count
// FEMALEFP, the first-person Ripley rig (docs/animationsync-bone-mask-analysis.md):
// HEAD hangs from EXTRACT and carries the shoulders; HIPS carries the pelvis,
// legs and spine. A first-person clip moves that head/arms rig, not the body.
constexpr std::uint32_t kFirstPersonSkeleton = 2971063975U;
constexpr std::size_t kFirstPersonBones = 69U;
enum FirstPersonBone : std::uint32_t {
    kFpHead = 1, kFpLeftArm = 4, kFpLeftForearm = 6, kFpLeftHand = 9,
    kFpRightArm = 27, kFpRightForearm = 29, kFpRightHand = 32, kFpHips = 49,
    kFpLeftThigh = 51, kFpLeftCalf = 52, kFpLeftFoot = 53, kFpLeftToe = 54,
    kFpRightThigh = 55, kFpRightCalf = 56, kFpRightFoot = 57, kFpRightToe = 58,
};
constexpr std::array<FirstPersonBone, 16> kPoseBones{kFpHead, kFpLeftArm, kFpLeftForearm, kFpLeftHand,
    kFpRightArm, kFpRightForearm, kFpRightHand, kFpHips, kFpLeftThigh, kFpLeftCalf, kFpLeftFoot, kFpLeftToe,
    kFpRightThigh, kFpRightCalf, kFpRightFoot, kFpRightToe};
using CrewBones = std::array<std::int16_t, kFirstPersonBones>;
std::vector<std::int16_t> gFirstPersonParents; // from bone-masks.bin; empty = unknown
// Bones that move (rotation) or are held >5 degrees away from the source
// reference. Translation-only offsets (first-person camera/root height) are not
// enough: they are what lowers the crew body.
constexpr float kMovingDegrees = 2.0F, kHeldDegrees = 5.0F, kDisplacedTranslation = 0.2F;
void LoadMasks() noexcept {
    gMaskClips.clear(); gMaskMaps.clear(); gMaskFile = -1; gFirstPersonParents.clear();
    std::array<wchar_t, 1024> module{};
    const auto length = GetModuleFileNameW(nullptr, module.data(), static_cast<DWORD>(module.size()));
    if (length == 0U || length >= module.size()) return;
    std::wstring path(module.data(), length);
    const auto slash = path.find_last_of(L"\\/");
    if (slash == std::wstring::npos) return;
    path.resize(slash + 1U);
    path += L"AIMP\\AnimationSync\\bone-masks.bin";
    const auto file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (file == INVALID_HANDLE_VALUE) return;
    LARGE_INTEGER size{};
    std::vector<std::uint8_t> bytes;
    if (GetFileSizeEx(file, &size) && size.QuadPart > 0 && size.QuadPart < 64LL * 1024 * 1024) {
        bytes.resize(static_cast<std::size_t>(size.QuadPart));
        DWORD read{};
        if (!ReadFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &read, nullptr) || read != bytes.size()) bytes.clear();
    }
    CloseHandle(file);
    std::size_t at{};
    const auto u32 = [&](std::uint32_t& value) {
        if (at + 4U > bytes.size()) return false;
        std::memcpy(&value, bytes.data() + at, 4U); at += 4U; return true;
    };
    const auto i16 = [&](std::int16_t& value) {
        if (at + 2U > bytes.size()) return false;
        std::memcpy(&value, bytes.data() + at, 2U); at += 2U; return true;
    };
    std::uint32_t magic{}, version{}, skeletons{}, maps{}, clips{};
    if (!u32(magic) || magic != 0x4D424941U || !u32(version) || version != 4U || (at += 32U) > bytes.size() ||
        !u32(skeletons) || skeletons > 256U) return;
    // Skeleton roots (no parent) carry the placement, never a clip pose.
    std::vector<std::pair<std::uint32_t, BoneSet>> roots;
    std::vector<std::int16_t> firstPerson;
    for (std::uint32_t i = 0; i < skeletons; ++i) {
        std::uint32_t hash{}, bones{};
        if (!u32(hash) || !u32(bones) || bones > 4096U || at + bones * 2ULL > bytes.size()) return;
        if (hash == kFirstPersonSkeleton && bones == kFirstPersonBones) {
            firstPerson.resize(bones);
            std::memcpy(firstPerson.data(), bytes.data() + at, bones * 2U);
        }
        BoneSet root{};
        for (std::uint32_t b = 0; b < bones && b < kMaskBones; ++b) {
            std::int16_t parent{};
            std::memcpy(&parent, bytes.data() + at + b * 2U, sizeof(parent));
            if (parent < 0) root.set(b);
        }
        roots.emplace_back(hash, root);
        at += (bones * 2U + 3U) / 4U * 4U;
    }
    if (!u32(maps) || maps > 4096U) return;
    std::vector<MaskMap> loadedMaps(maps);
    for (auto& map : loadedMaps) {
        std::uint32_t count{};
        if (!u32(map.source) || !u32(map.target) || !u32(map.targetBones) || !u32(count) || count > 4096U) return;
        map.pairs.resize(count);
        for (auto& [from, to] : map.pairs) if (!i16(from) || !i16(to)) return;
    }
    if (!u32(clips) || clips > 100000U) return;
    std::vector<MaskClip> loaded;
    loaded.reserve(clips);
    for (std::uint32_t i = 0; i < clips; ++i) {
        MaskClip clip{};
        if (!u32(clip.path) || !u32(clip.status) || !u32(clip.skeleton) || !u32(clip.tracks) || clip.tracks > 4096U ||
            at + clip.tracks * 20ULL > bytes.size()) return;
        for (std::uint32_t t = 0; t < clip.tracks; ++t, at += 20U) {
            std::int16_t bone{};
            float motion[4]{};
            std::memcpy(&bone, bytes.data() + at, sizeof(bone));
            std::memcpy(motion, bytes.data() + at + 4U, sizeof(motion));
            const bool moving = motion[0] > kMovingDegrees;
            const bool held = !moving && motion[2] > kHeldDegrees;
            clip.moving += moving ? 1U : 0U;
            clip.held += held ? 1U : 0U;
            if (bone < 0 || static_cast<std::size_t>(bone) >= kMaskBones) continue;
            if (moving || held) clip.driven.set(static_cast<std::size_t>(bone));
            if (motion[1] > kDisplacedTranslation) clip.displaced.set(static_cast<std::size_t>(bone));
        }
        for (const auto& [hash, root] : roots)
            if (hash == clip.skeleton) { clip.driven &= ~root; clip.displaced &= ~root; }
        loaded.push_back(std::move(clip));
    }
    if (at != bytes.size() || !std::is_sorted(loaded.begin(), loaded.end(),
            [](const MaskClip& a, const MaskClip& b) { return a.path < b.path; })) return;
    gMaskClips = std::move(loaded);
    gMaskMaps = std::move(loadedMaps);
    gFirstPersonParents = std::move(firstPerson);
    gMaskFile = static_cast<std::int32_t>(gMaskClips.size());
}
const MaskClip* FindMask(const std::uint32_t path) noexcept {
    const auto it = std::lower_bound(gMaskClips.begin(), gMaskClips.end(), path,
        [](const MaskClip& clip, const std::uint32_t value) { return clip.path < value; });
    return it != gMaskClips.end() && it->path == path ? &*it : nullptr;
}
// The unique engine map from one skeleton to a skeleton of the given bone count.
const MaskMap* UniqueMap(const std::uint32_t source, const std::uint32_t targetBones) noexcept {
    const MaskMap* found{};
    for (const auto& map : gMaskMaps) {
        if (map.source != source || map.targetBones != targetBones) continue;
        if (found != nullptr) return nullptr; // ambiguous target skeleton
        found = &map;
    }
    return found;
}
BoneSet MapBones(const MaskMap& map, const BoneSet& input) noexcept {
    BoneSet output{};
    for (const auto& [from, to] : map.pairs)
        if (from >= 0 && to >= 0 && static_cast<std::size_t>(from) < kMaskBones &&
            static_cast<std::size_t>(to) < kMaskBones && input.test(static_cast<std::size_t>(from)))
            output.set(static_cast<std::size_t>(to));
    return output;
}
// Crew high-res bones the clip drives: clip skeleton -> crew low-res skeleton
// (layer 0 bone count) -> this character's own high-res skeleton (layer 7, the
// final pose; its hash is the clip database's character id, LocalClip's
// resource). Bones without a source (face, helpers) are not driven.
// The crew high-res bone of each first-person bone, through the same engine
// maps (FEMALEFP -> crew low-res -> this character's high-res); -1 = none.
bool MapCrewBones(const MaskClip& clip, const std::uint32_t lowBones, const std::uint32_t highBones,
    const std::uint32_t highSkeleton, CrewBones& output) noexcept {
    output.fill(-1);
    if (clip.skeleton != kFirstPersonSkeleton) return false;
    const auto* low = UniqueMap(clip.skeleton, lowBones);
    if (low == nullptr) return false;
    const MaskMap* high{};
    if (lowBones != highBones) {
        for (const auto& map : gMaskMaps)
            if (map.source == low->target && map.target == highSkeleton && map.targetBones == highBones) { high = &map; break; }
        if (high == nullptr) return false;
    }
    const auto target = [](const MaskMap& map, const std::int16_t from) -> std::int16_t {
        for (const auto& [source, to] : map.pairs) if (source == from) return to;
        return -1;
    };
    for (std::size_t b = 0; b < output.size(); ++b) {
        auto bone = target(*low, static_cast<std::int16_t>(b));
        if (bone >= 0 && high != nullptr) bone = target(*high, bone);
        output[b] = bone;
    }
    for (const auto bone : kPoseBones) if (output[bone] < 0) return false;
    return true;
}
bool CrewDriven(const MaskClip& clip, const std::uint32_t lowBones, const std::uint32_t highBones,
    const std::uint32_t highSkeleton, BoneSet& output, BoneSet& displaced, std::uint32_t& target) noexcept {
    const auto* low = UniqueMap(clip.skeleton, lowBones);
    if (low == nullptr || highBones > kMaskBones) return false;
    const MaskMap* high{};
    if (lowBones != highBones) {
        for (const auto& map : gMaskMaps)
            if (map.source == low->target && map.target == highSkeleton && map.targetBones == highBones) { high = &map; break; }
        if (high == nullptr) return false;
    }
    output = MapBones(*low, clip.driven);
    displaced = MapBones(*low, clip.displaced);
    if (high != nullptr) { output = MapBones(*high, output); displaced = MapBones(*high, displaced); }
    target = high != nullptr ? high->target : low->target;
    return true;
}

template<class T> bool Read(const std::uintptr_t address, T& value) noexcept {
    return address >= 0x10000U && crash_guard::TryCopy(&value, reinterpret_cast<const void*>(address), sizeof(value));
}
bool Match(const std::uintptr_t rva, const std::initializer_list<std::uint8_t> bytes) noexcept {
    std::array<std::uint8_t, 64U> actual{};
    return bytes.size() <= actual.size() &&
        crash_guard::TryCopy(actual.data(), reinterpret_cast<const void*>(gBase.load() + rva), bytes.size()) &&
        std::memcmp(actual.data(), bytes.begin(), bytes.size()) == 0;
}
// Identical certification to the hiding presentation's constructor ABI.
bool Abi() noexcept {
    const auto base = gBase.load();
    std::uintptr_t update{}, initialize{}, marker{};
    return base != 0U && Read(base + kScriptedType + 0x20U, update) && update == base + 0x256BE0U &&
        Read(base + kScriptedType + 0x60U, initialize) && initialize == base + 0x26C4C0U &&
        Read(base + kScriptedType + 4U, marker) && marker == base + 0x24F290U &&
        Match(0x257A13U, {0x8B,0x06,0x8B,0x10,0x6A,0x01,0x8B,0xCE,0xFF,0xD2,
            0x8B,0x44,0x24,0x0C,0x8B,0x4C,0x24,0x08,0x50,0x51,0x8B,0xCE,
            0xE8,0x82,0xEE,0xFF,0xFF,0x8B,0x16,0x8B,0x42,0x60,0x8B,0xCE,0xFF,0xD0,
            0x5E,0xC2,0x08,0x00}) &&
        Match(0x256914U, {0x8D,0x78,0x44,0xB9,0x16,0x00,0x00,0x00}) &&
        Match(0x256936U, {0xF3,0xA5,0x5F,0x89,0x90,0x9C,0x00,0x00,0x00,0x5E,0xC2,0x08,0x00});
}
bool StreamAbi() noexcept {
    for (const auto rva : {0x639060U, 0x636970U, 0x636BB0U})
        if (!Match(rva, {0x55,0x8B,0xEC,0x83,0xEC,0x10,0x56,0x57}) ||
            !Match(rva + (rva == 0x636970U ? 0x14CU : 0x13AU), {0xC2,0x08,0x00})) return false;
    return true;
}
// Character+36C is the primary action cycle; +388 the equipment/upper-body
// cycle (holster, items). Both can run CATHODE scripted clips.
constexpr std::uintptr_t kPrimaryCycle = 0x36CU, kEquipmentCycle = 0x388U;
bool ReadCycle(const std::uintptr_t character, Cycle& cycle, const std::uintptr_t slot = kPrimaryCycle) noexcept {
    cycle = {};
    if (!Read(character + 0x50U, cycle.runtime) || cycle.runtime == 0U ||
        !Read(character + slot, cycle.address) || !Read(cycle.address, cycle.type) ||
        !Read(cycle.address + 4U, cycle.owner) || cycle.owner != character ||
        !Read(cycle.address + 8U, cycle.flags) || !Read(cycle.address + 0x19U, cycle.serial) ||
        !Read(cycle.address + 0x1AU, cycle.state)) return false;
    if (cycle.type == gBase.load() + kScriptedType && cycle.state == kScriptedState)
        return Read(cycle.address + 0x14U, cycle.graph) && Read(cycle.address + 0x68U, cycle.resource) &&
            Read(cycle.address + 0x6CU, cycle.animation);
    return true;
}
// Flag 4 = completed (0064F290 sets 6 at the end). Flag 2 alone is set at
// start (0066C4C0) from param +0x98 bit 0x20 (early-interruptible clip): the
// lever/hacking interactions use it, so it must not reject a live clip.
bool Scripted(const Cycle& cycle) noexcept {
    return cycle.type == gBase.load() + kScriptedType && cycle.state == kScriptedState &&
        cycle.graph != 0U && cycle.resource != 0U && cycle.animation != 0U && (cycle.flags & 4U) == 0U;
}
bool SameCycle(const Cycle& a, const Cycle& b) noexcept {
    return a.address == b.address && a.type == b.type && a.owner == b.owner &&
        a.runtime == b.runtime && a.state == b.state && a.serial == b.serial;
}
bool Owned(const Cycle& current, const Cycle& owned) noexcept {
    return owned.address != 0U && SameCycle(current, owned) && (owned.graph == 0U || current.graph == owned.graph) &&
        current.resource == owned.resource && current.animation == owned.animation;
}
bool IsLocal(const std::uintptr_t character, bool& local) noexcept {
    local = false;
    std::uintptr_t root{}, context{}, players{};
    std::uint32_t count{};
    if (!Read(gBase.load() + 0x12F0C88U, root) || !Read(root + 8U, context) ||
        !Read(context + 0x80U, players) || !Read(context + 0x84U, count) ||
        count == 0U || count > 4U || players < 0x10000U) return false;
    for (std::uint32_t i = 0; i < count; ++i) {
        std::uintptr_t player{};
        if (!Read(players + i * 4U, player)) return false;
        local = local || player == character;
    }
    return true;
}
bool RemoteReady(const std::uintptr_t character) noexcept {
    bool local{};
    std::uint32_t health{};
    std::uint8_t grappled{}, vent{};
    return IsLocal(character, local) && !local && Read(character + 0x32CU, health) && health < 3U &&
        Read(character + 0x297U, grappled) && grappled == 0U &&
        Read(character + 0x2B4U, vent) && vent == 0U;
}
// Retail 00A35FF0 base-table search in the representation's own clip DB.
bool LocalClip(const std::uintptr_t character, const std::uint32_t token,
    std::uint32_t& resource, std::uint32_t& path) noexcept {
    std::uintptr_t instance{}, database{}, table{}, keys{}, values{};
    std::uint32_t count{};
    if (!Read(character + 0x720U, instance) || !Read(instance + 0x10U, database) ||
        !Read(database + 0x10U, resource) || resource == 0U ||
        !Read(database + 0x20U, table) || !Read(table + 8U, keys) ||
        !Read(table + 12U, values) || !Read(table + 16U, count) || count > 100000U) return false;
    std::uint32_t first{}, last{count};
    while (first < last) {
        const auto middle = first + (last - first) / 2U;
        std::uint32_t key{}, index{};
        if (!Read(keys + middle * 8U, key)) return false;
        if (key > token) first = middle + 1U;
        else if (key < token) last = middle;
        else return Read(keys + middle * 8U + 4U, index) && index < count &&
            Read(values + index * 8U, path) && path != 0U;
    }
    return false;
}

// --- Streamed clip requests (public acquire/query/release, RET8) -------------
bool ClipOperation(const std::uintptr_t rva, const std::uintptr_t database, const std::uint32_t animation) noexcept {
#if defined(_MSC_VER) && defined(_M_IX86)
    const std::uint32_t alias[3]{0U, 0U, animation};
    const std::uint32_t variant[3]{0U, 0U, 0xADA7AFDBU}; // FNV-1a("none"), as retail
    __try {
        using Operation = std::uint8_t(__thiscall*)(void*, const std::uint32_t*, const std::uint32_t*);
        return reinterpret_cast<Operation>(gBase.load() + rva)(reinterpret_cast<void*>(database), alias, variant) != 0U;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
#else
    static_cast<void>(rva); static_cast<void>(database); static_cast<void>(animation); return false;
#endif
}
Slot* Find(const std::uintptr_t character) noexcept {
    for (auto& slot : gSlots) if (slot.character == character) return &slot;
    return nullptr;
}
void ReleaseStream(Stream& stream) noexcept {
    if (stream.acquired && stream.database)
        static_cast<void>(ClipOperation(0x636BB0U, stream.database->Object(), stream.animation));
    stream.acquired = false;
    stream.database.reset();
    stream.character = 0U;
}
void CollectStreams() noexcept {
    for (auto& stream : gStreams) {
        if (stream.character == 0U) continue;
        bool keep{};
        {
            const std::lock_guard lock(gMutex);
            if (const auto* slot = Find(stream.character); slot != nullptr && slot->binding == stream.binding)
                keep = (slot->owned.address != 0U && slot->owned.animation == stream.animation) ||
                    (slot->desired.transition == stream.transition && slot->handled != stream.transition);
        }
        if (!keep) ReleaseStream(stream);
    }
}
enum class StreamReadiness { Failed, Waiting, Ready };
StreamReadiness PrepareStream(const Slot& slot, const std::uint32_t animation) noexcept {
    if (!StreamAbi()) return StreamReadiness::Failed;
    Stream* stream{};
    for (auto& candidate : gStreams)
        if (candidate.character == slot.character && candidate.binding == slot.binding &&
            candidate.transition == slot.desired.transition && candidate.animation == animation) { stream = &candidate; break; }
    if (stream == nullptr) {
        for (auto& candidate : gStreams) if (candidate.character == 0U) { stream = &candidate; break; }
        if (stream == nullptr) return StreamReadiness::Failed;
        std::uintptr_t instance{}, database{};
        if (!Read(slot.character + 0x720U, instance) || !Read(instance + 0x10U, database)) return StreamReadiness::Failed;
        auto lease = std::make_unique<native_resource::StrongLease>(instance + 0x10U, 0x44U);
        if (!*lease || lease->Object() != database) return StreamReadiness::Failed;
        if (!ClipOperation(0x639060U, database, animation)) return StreamReadiness::Failed;
        stream->character = slot.character; stream->binding = slot.binding;
        stream->transition = slot.desired.transition; stream->animation = animation;
        stream->database = std::move(lease); stream->acquired = true;
    }
    return ClipOperation(0x636970U, stream->database->Object(), animation)
        ? StreamReadiness::Ready : StreamReadiness::Waiting;
}

// --- Native transitions ------------------------------------------------------
bool Invoke(const std::uintptr_t cycle, const ai_sync::native_intent::Payload& intent) noexcept {
#if defined(_MSC_VER) && defined(_M_IX86)
    __try {
        using Transition = void(__thiscall*)(void*, const std::uint32_t*, std::uint32_t);
        reinterpret_cast<Transition>(gBase.load() + 0x2579F0U)(
            reinterpret_cast<void*>(cycle), intent.words.data() + 12U, 0U);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
#else
    static_cast<void>(cycle); static_cast<void>(intent); return false;
#endif
}
bool InvokeIdle(const std::uintptr_t cycle) noexcept {
#if defined(_MSC_VER) && defined(_M_IX86)
    __try {
        reinterpret_cast<void(__thiscall*)(void*)>(gIdleTransition.load())(reinterpret_cast<void*>(cycle));
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
#else
    static_cast<void>(cycle); return false;
#endif
}
// Touches only our exact constructor result: never a retail replacement,
// paired action, death or a recycled same-address cycle.
bool Cancel(const std::uintptr_t character, const Cycle& owned) noexcept {
    Cycle current{};
    if (!ReadCycle(character, current) || !Owned(current, owned)) return true;
    if (!RemoteReady(character) || gIdleTransition.load() == 0U) return false;
    const character_direct_step_runtime::MovementResourceLease resource(character);
    if (!resource) return false;
    if (!InvokeIdle(current.address)) return false;
    return ReadCycle(character, current) && !Owned(current, owned);
}
// The owner's timeline, but none of its spatial/convergence references,
// external clock, camera or equipment flags: an isolated actor clip.
bool BuildIntent(const State& state, const std::uint32_t resource, ai_sync::native_intent::Payload& output) noexcept {
    if (!Valid(state) || !Active(state) || resource == 0U) return false;
    ai_sync::native_intent::Payload intent{};
    auto& w = intent.words;
    w[0] = 20U; w[21] = resource; w[22] = state.animation;
    w[23] = std::bit_cast<std::uint32_t>(state.startFrame);
    w[24] = std::bit_cast<std::uint32_t>(state.endFrame);
    w[25] = std::bit_cast<std::uint32_t>(state.loops);
    w[26] = std::bit_cast<std::uint32_t>(state.speed);
    w[27] = std::bit_cast<std::uint32_t>(state.blendIn);
    w[30] = std::bit_cast<std::uint32_t>(-1.0F);
    if (!ai_sync::native_intent::Validate(intent)) return false;
    output = intent; return true;
}

// The owner's interactions put the weapon away first. Use the retail NPC
// select/holster transition (FUN_00654E60, as AISyncCombat) on the remote
// representation's secondary cycle, so the weapon layer no longer masks the
// clip; the same slot is drawn back once the clip is over.
constexpr std::uintptr_t kInventoryItem = 0x00374A10U, kEquipmentTransition = 0x00254E60U;
bool EquipmentTransition(const std::uintptr_t character, const bool draw, std::int32_t& slot) noexcept {
    std::uintptr_t controller{}, owner{}, cycle{};
    std::uint32_t current{};
    std::uint16_t kind{};
    std::uint8_t pending{}, fire{}, ready{}, equipped{}, flags{};
    if (!Read(character + 0x1100U, controller) || controller == 0U || !Read(controller + 0x1528U, owner) ||
        owner != character || !Read(controller + 0xA0U, pending) || !Read(controller + 0xA3U, fire) ||
        !Read(controller + 0xA4U, ready) || pending != 0U || fire != 0U || ready != 0U ||
        !Read(controller + 0x150CU, current) || current > 10U || !Read(controller + 0x1510U, equipped) ||
        !Read(character + 0x388U, cycle) || cycle == 0U || !Read(cycle + 4U, owner) || owner != character ||
        !Read(cycle + 0x1EU, kind) || kind == 7U || !Read(cycle + 8U, flags) || (flags & 2U) == 0U) return false;
    if (!draw) {
        if (equipped == 0U) return false; // nothing in hand: nothing to restore
        slot = static_cast<std::int32_t>(current);
    } else if (slot < 0 || slot > 10) return false;
    const auto target = static_cast<std::uint32_t>(slot);
#if defined(_MSC_VER) && defined(_M_IX86)
    __try {
        if (draw && reinterpret_cast<std::uintptr_t(__thiscall*)(void*, std::uint32_t)>(
                gBase.load() + kInventoryItem)(reinterpret_cast<void*>(controller), target) == 0U) return false;
        using Transition = void(__thiscall*)(void*, std::uint32_t, std::uint32_t, std::uint32_t,
            std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t);
        reinterpret_cast<Transition>(gBase.load() + kEquipmentTransition)(reinterpret_cast<void*>(cycle),
            !draw || current != target ? 1U : 0U, current, 1U, draw ? 1U : 0U, target, 1U, 0U, 1U);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
#else
    return false;
#endif
}

bool LayerBones(const std::uintptr_t character, const std::uint32_t layer, std::uint32_t& bones) noexcept {
    std::uintptr_t holder{}, skeleton{};
    return Read(character + 0x3F0U + 0x210U + layer * 4U, holder) && Read(holder + 0x1CU, skeleton) &&
        Read(skeleton + 0x1CU, bones) && bones != 0U && bones <= 256U;
}
// Anim controller at Character+0x3F0; layer poses hold 0x30-byte local transforms
// (translation, rotation, scale float4). Layer composition (0x2A4F10 -> CALL
// 0x2A5026 -> 0x2A25A0(controller, from, to, toPose)) blends layers in order
// 0>1 ... 6>7; layer 7 then holds the final high-res pose. That call is
// redirected so the final pose can be corrected right after the last step,
// before anything consumes it. It may run on
// animation workers: the engine thread publishes each slot's state here and
// the redirect only touches its own entry's pose buffers.
enum : std::uint32_t { kMaskOff = 0U, kMaskCapture = 1U, kMaskApply = 2U };
struct MaskShare final {
    std::mutex mutex;
    std::atomic<std::uintptr_t> controller{};
    std::atomic<std::uint32_t> state{};
    std::uint64_t binding{};                  // protected by mutex, including actor reuse
    Cycle expected{};
    BoneSet driven{}, displaced{};
    std::atomic<std::uint32_t> baseBones{};    // 0 until an idle final pose was captured
    std::array<std::array<std::uint32_t, 12>, kMaskBones> base{}; // LOCAL translation, rotation, scale
    // Final-pose skeleton hierarchy (hkaSkeleton m_parentIndices), 0 = unknown.
    std::uint32_t parentCount{};
    std::array<std::int16_t, kMaskBones> parents{};
    // Render-only root displacement (model x, y, z, yaw), protected by mutex.
    std::atomic<bool> offsetActive{};
    std::array<float, 4> offset{};
    std::atomic<std::uint64_t> offsetFrames{};
    // Only native vent entry uses a stable visual root. The body clip keeps
    // its limb rotations and local animation; its moving root translation
    // must not be added to the replicated owner trajectory a second time.
    bool stableRoot{}, rootReferenceReady{};
    std::array<std::array<float, 3>, kMaskBones> rootReference{};
    std::uint64_t stableFrames{}, idleReferences{}, liveReferences{};
    float maxSuppressedRootDeltaY{};
    // Native vent only: probe the mapped high-res feet & hips, and apply
    // bounded visual-root support without modifying collision or limbs.
    bool ventMapped{};
    float ventRise{}, ventProgress{}, ventGroundLift{};
    float ventLastFootY{}, ventIdleFootY{}, ventLastHipDeltaY{};
    float ventMaxGroundLift{}, ventMaxDepthBeforeGround{}, ventMaxHipDrop{};
    std::uint64_t ventGroundFrames{}, ventGroundSkipped{}, ventGroundMapped{};
    std::atomic<std::uint64_t> applied{}, captured{};
    std::atomic<std::uint32_t> restored{};
    // Posture rebuild: the owner's body effectors keyed by its clip time for
    // the transition this representation plays (mutex), and its crew bones.
    struct PoseSample final { float time{}; std::array<float, 12> effectors{}; };
    struct PoseTrack final {
        std::uint64_t transition{};
        std::uint32_t count{};
        std::array<PoseSample, 256> samples{};
    } track{};
    std::uint64_t playing{};
    std::array<std::int16_t, 69> crewPose{};
    std::atomic<std::uint64_t> solved{};
    // Why the last apply frame did not rebuild the posture (0 = it did): 1 no
    // owner effectors, 2 other transition, 3 skeleton/idle, 4 crew bones,
    // 5 clip clock, 6 eased out, 7 pose read, 8 idle feet, 9 non-finite.
    std::atomic<std::uint32_t> poseSkip{};
    // Solved frames where the body had to be lifted back onto the floor.
    std::atomic<std::uint64_t> lifted{};
};
std::array<MaskShare, kSlots> gMaskShares{};
// The local first-person body, sampled from its own final pose by the layer
// blend hook (animation workers) and read by the owner capture (engine).
struct OwnerPose final {
    std::mutex mutex;
    std::atomic<std::uintptr_t> controller{};
    std::atomic<bool> clip{};          // a scripted clip is captured this frame
    std::atomic<std::uint64_t> frames{};
    MaskShare* skeleton{};             // parents only (mutex)
    std::uintptr_t checked{};
    bool firstPerson{}, calibrated{}, fresh{};
    float yaw{};                       // standing feet heading in model space
    std::array<float, 3> idleHips{}, idleHead{};
    std::array<float, 12> effectors{};
    std::uint64_t at{};
};
OwnerPose gOwnerPose{};
std::atomic<bool> gMaskHook{};

// hkaSkeleton (Havok 2012, 32-bit) of the final pose: m_parentIndices data
// +0xC, size +0x10; m_bones size +0x1C. Parents precede their children.
void ReadParents(const std::uintptr_t controller, MaskShare& share) noexcept {
    share.parentCount = 0U;
    std::uintptr_t holder{}, skeleton{}, parents{};
    std::uint32_t bones{}, count{};
    if (Read(controller + 0x210U + 7U * 4U, holder) && Read(holder + 0x1CU, skeleton) &&
        Read(skeleton + 0x1CU, bones) && Read(skeleton + 0x10U, count) && Read(skeleton + 0xCU, parents) &&
        bones == count && bones != 0U && bones <= kMaskBones &&
        crash_guard::TryCopy(share.parents.data(), reinterpret_cast<const void*>(parents), bones * 2U)) {
        bool ordered = true;
        for (std::uint32_t b = 0; b < bones; ++b) ordered = ordered && share.parents[b] < static_cast<std::int32_t>(b);
        if (ordered) share.parentCount = bones;
    }
}

// Publish BEFORE constructing a scripted graph. Publishing only after the
// CharacterManager let its first scripted frame overwrite the cached idle.
// The same lock protects immutable mask/base snapshots from animation workers.
void PublishMaskState(const Slot& slot, MaskShare& share, const bool starting = false) noexcept {
    const std::lock_guard lock(share.mutex);
    const auto controller = slot.character != 0U ? slot.character + 0x3F0U : 0U;
    if (share.binding != slot.binding || share.controller.load() != controller) {
        share.state.store(kMaskOff);
        share.baseBones.store(0U);
        share.applied.store(0U); share.captured.store(0U); share.restored.store(0U);
        share.offsetActive.store(false); share.parentCount = 0U;
        share.stableRoot = false; share.rootReferenceReady = false;
        share.stableFrames = share.idleReferences = share.liveReferences = 0U;
        share.maxSuppressedRootDeltaY = 0.0F;
        share.ventMapped = false;
        share.ventGroundLift = share.ventRise = share.ventProgress = 0.0F;
        share.ventLastFootY = share.ventIdleFootY = share.ventLastHipDeltaY = 0.0F;
        share.ventMaxGroundLift = share.ventMaxDepthBeforeGround = share.ventMaxHipDrop = 0.0F;
        share.ventGroundFrames = share.ventGroundSkipped = share.ventGroundMapped = 0U;
        share.track.transition = 0U; share.track.count = 0U; share.playing = 0U;
        share.binding = slot.binding;
        share.controller.store(controller);
    }
    if (controller == 0U) return;
    std::uint32_t wanted = kMaskOff;
    if (starting || slot.owned.address != 0U) {
        if (slot.maskActive) wanted = kMaskApply;
    } else {
        Cycle current{};
        // A foreign script, death or traversal must never become our idle.
        if (ReadCycle(slot.character, current) &&
            (current.state == 1U || current.state == 3U || current.state == 0x38U)) wanted = kMaskCapture;
    }
    if (wanted == kMaskApply) {
        share.driven = slot.crewDriven;
        share.displaced = slot.crewDisplaced;
        share.expected = slot.owned;
        share.crewPose = slot.crewPose;
        if (starting) {
            share.playing = slot.desired.transition;
            ReadParents(controller, share);
            ReadCycle(slot.character, share.expected);
            share.expected.serial = static_cast<std::uint8_t>(share.expected.serial + 1U);
            share.expected.type = gBase.load() + kScriptedType;
            share.expected.state = kScriptedState;
            share.expected.animation = slot.play;
            share.expected.graph = 0U;
            share.expected.resource = slot.diagnostics.resource;
        }
    }
    share.state.store(wanted, std::memory_order_release);
}

void ProbeLayers(Slot& slot) noexcept {
    auto& d = slot.diagnostics;
    const auto controller = slot.character + 0x3F0U;
    std::uint8_t flags{}, ready{};
    if (Read(controller + 0x3CCU, flags) && Read(controller + 0xD00U, ready))
        d.controllerFlags = static_cast<std::uint32_t>(flags) | (static_cast<std::uint32_t>(ready) << 8U);
    auto& target = slot.owned.address != 0U ? d.during : d.idle;
    for (std::size_t layer = 0; layer < 8U; ++layer) {
        std::uintptr_t holder{}, skeleton{}, pose{}, transforms{};
        std::uint32_t bones{};
        if (!Read(controller + 0x230U + layer * 4U, pose) || pose < 0x10000U ||
            !Read(controller + 0x210U + layer * 4U, holder) || !Read(holder + 0x1CU, skeleton) ||
            !Read(skeleton + 0x1CU, bones) || bones == 0U || bones > 256U ||
            !Read(pose + 4U, transforms) || transforms < 0x10000U) { target[layer].pose = 0U; continue; }
        // FNV-1a over every bone's local translation+rotation (first 0x20 bytes).
        std::uint32_t hash = 2166136261U;
        for (std::uint32_t bone = 0; bone < bones; ++bone) {
            std::array<std::uint32_t, 8> words{};
            if (!Read(transforms + bone * 0x30U, words)) { hash = 0U; break; }
            for (const auto word : words) hash = (hash ^ word) * 16777619U;
        }
        auto& probe = target[layer];
        if (probe.pose == pose && probe.bones == bones && probe.samples != 0U && hash != probe.hash) ++probe.changes;
        probe.pose = pose; probe.bones = bones; probe.hash = hash; ++probe.samples;
    }
}

constexpr std::uint64_t kHolsterTimeoutMs = 1500U, kUnarmedSettleMs = 250U;
// Weapon away and no equipment transition in flight (same fields as above).
bool Unarmed(const std::uintptr_t character) noexcept {
    std::uintptr_t controller{};
    std::uint8_t pending{}, fire{}, ready{}, equipped{};
    return Read(character + 0x1100U, controller) && controller != 0U && Read(controller + 0xA0U, pending) &&
        Read(controller + 0xA3U, fire) && Read(controller + 0xA4U, ready) && Read(controller + 0x1510U, equipped) &&
        pending == 0U && fire == 0U && ready == 0U && equipped == 0U;
}

// Clip segment [start, end] in seconds, as timeline::Project reads it.
void Segment(const State& s, float& start, float& end) noexcept {
    start = (std::max)(0.0F, static_cast<float>(s.startFrame) / 30.0F);
    end = s.endFrame > 0 ? (std::min)(s.durationSeconds, static_cast<float>(s.endFrame) / 30.0F) : s.durationSeconds;
}
// The owner's clip clock now: its latest sample projected by the sample age,
// or held when the owner's clock is stalled. False once the owner's clip is over.
bool OwnerPhase(const Slot& slot, const std::uint64_t now, float& phase, float& remaining) noexcept {
    if (!Active(slot.desired)) return false;
    auto age = static_cast<std::uint64_t>(slot.desired.elapsedMilliseconds) + now - slot.receivedAt;
    if (slot.ownerRate >= 0.0F && slot.ownerRate < 0.05F * slot.desired.speed) age = 0U;
    std::int32_t loops{};
    if (!timeline::Project(slot.desired, age, phase, loops)) return false;
    float start{}, end{};
    Segment(slot.desired, start, end);
    remaining = loops < 0 ? 1.0e6F : (end - phase) + static_cast<float>((std::max)(0, loops - 1)) * (end - start);
    return true;
}
void NoteEnding(Slot& slot, const char* result, const Cycle& cycle, const std::uint64_t now) noexcept {
    auto& d = slot.diagnostics;
    auto& row = d.endings[d.endingNext++ % d.endings.size()];
    row = {};
    row.transition = slot.desired.transition; row.animation = slot.owned.animation;
    row.start = slot.desired.startFrame; row.end = slot.desired.endFrame; row.loops = slot.desired.loops;
    row.result = result; row.duration = slot.desired.durationSeconds;
    float duration{}, remaining{};
    if (cycle.graph != 0U && !timeline::Read(cycle.graph, row.ours, duration)) row.ours = -1.0F;
    if (!OwnerPhase(slot, now, row.owner, remaining)) row.owner = -1.0F;
}
// Our replay of the owner's current clip: same transition, not a locker clip
// (those follow their recorded path instead).
bool Following(const Slot& slot) noexcept {
    return Active(slot.desired) && slot.handled == slot.desired.transition &&
        slot.play == slot.owned.animation &&
        player_hiding_presentation::ClassifyClip(slot.owned.animation).phase == 0U;
}
// Ours ended or was taken while the owner still plays it: replay it again at
// the owner's phase (bounded per transition).
void Readmit(Slot& slot, const bool following, const std::uint64_t now) noexcept {
    float phase{}, remaining{};
    if (!following || slot.readmits >= 4U || !OwnerPhase(slot, now, phase, remaining) || remaining < 0.5F) return;
    ++slot.readmits; ++slot.diagnostics.readmitted; slot.handled = 0U;
}
// A full-body NPC animation may differ in duration/trim from the FP action.
float TargetClipPhase(const Slot& slot, const float ownerPhase, const float npcDuration) noexcept {
    if (!slot.nativeClip || !(npcDuration > 0.0F) || !std::isfinite(npcDuration) ||
        !std::isfinite(ownerPhase)) return ownerPhase;
    float first{}, last{};
    Segment(slot.desired, first, last);
    if (!(last > first)) return 0.0F;
    return (std::clamp)((ownerPhase - first) / (last - first), 0.0F, 1.0F) * npcDuration;
}
// Keep our clip on the owner's clock: holds, slow stages and drift.
void FollowOwnerClock(Slot& slot, const Cycle& current, const std::uint64_t now) noexcept {
    if (current.graph == 0U || now - slot.startedAt < 400U || now - slot.resyncAt < 250U) return;
    float ours{}, duration{}, target{}, remaining{};
    if (!timeline::Read(current.graph, ours, duration) || !OwnerPhase(slot, now, target, remaining)) return;
    target = TargetClipPhase(slot, target, duration);
    float difference = std::abs(ours - target);
    if (slot.desired.loops != 1) {
        float start{}, end{};
        Segment(slot.desired, start, end);
        if (end > start) difference = (std::min)(difference, std::abs((end - start) - difference));
    }
    if (difference <= 0.3F) return;
    slot.resyncAt = now;
    if (timeline::Seek(current.graph, target)) ++slot.diagnostics.resynced;
}

void Advance(Slot& slot, const std::uint64_t now, MaskShare& maskShare) noexcept {
    ProbeLayers(slot);
    auto& d = slot.diagnostics;
    Cycle current{};
    if (!RemoteReady(slot.character) || !ReadCycle(slot.character, current)) {
        if (slot.owned.address != 0U) ++d.interrupted;
        slot.owned = {}; d.stage = "actor-unavailable";
        ++d.deferred; // temporary traversal/paired/streaming state can clear
        return;
    }
    if (slot.owned.address != 0U && slot.nativeClip &&
        Owned(current, slot.owned) && (current.flags & 4U) == 0U) {
        std::array<float, 3> position{};
        if (Read(slot.character + 0x70U, position)) {
            if (slot.nativeRootLastValid) {
                const auto dx = position[0] - slot.nativeRootLastPosition[0];
                const auto dy = position[1] - slot.nativeRootLastPosition[1];
                const auto dz = position[2] - slot.nativeRootLastPosition[2];
                const auto distance = std::sqrt(dx * dx + dy * dy + dz * dz);
                // A large step is a teleport or an engine discontinuity,
                // not evidence of smooth reference-frame displacement.
                if (std::isfinite(distance)) {
                    if (distance <= 0.35F) d.nativeRootDistance += distance;
                    else ++d.nativeRootDiscontinuities;
                }
            }
            slot.nativeRootLastPosition = position;
            slot.nativeRootLastValid = true;
            ++d.nativeRootSamples;
            // Record one point every 125 ms, preserving the Character's
            // physical path, the latest Host position, both headings and
            // native graph time. This is intentionally observation-only:
            // never change the root motion, graph or Character here.
            if (slot.lastSpatialAt == 0U || now - slot.lastSpatialAt >= 125U) {
                float facing{}, phase{-1.0F}, duration{};
                if (Read(slot.character + 0x174U, facing) && std::isfinite(facing)) {
                    if (current.graph != 0U &&
                        !timeline::Read(current.graph, phase, duration)) phase = -1.0F;
                    auto& spatial = d.spatial[d.spatialNext++ % d.spatial.size()];
                    spatial = {};
                    spatial.tickMs = now;
                    spatial.transition = slot.desired.transition != 0U
                        ? slot.desired.transition : slot.highest;
                    spatial.clipPath = d.clipPath;
                    spatial.phase = phase;
                    spatial.character = position;
                    spatial.characterYaw = facing;
                    spatial.hasOwner = Active(slot.desired);
                    if (spatial.hasOwner) {
                        spatial.owner = slot.desired.position;
                        spatial.ownerYaw = slot.desired.yaw;
                    }
                    slot.lastSpatialAt = now;
                }
            }
            if (Active(slot.desired)) {
                const auto dx = position[0] - slot.desired.position[0];
                const auto dy = position[1] - slot.desired.position[1];
                const auto dz = position[2] - slot.desired.position[2];
                d.nativeOwnerGap = std::sqrt(dx * dx + dy * dy + dz * dz);
                if (std::isfinite(d.nativeOwnerGap))
                    d.nativeOwnerGapMax = (std::max)(d.nativeOwnerGapMax, d.nativeOwnerGap);
            }
        }
    }
    if (slot.owned.address != 0U) {
        const bool expired = Active(slot.desired) &&
            now - slot.receivedAt + slot.desired.elapsedMilliseconds > kObservationTimeoutMs;
        // A locker entry/exit always plays to its end: the observer replays
        // it later than the owner and needs its whole path into/out of the prop.
        const auto ownedHiding = player_hiding_presentation::ClassifyClip(slot.owned.animation).phase;
        const bool cleared = !Active(slot.desired) && slot.clearedAt != 0U &&
            ownedHiding != 1U && ownedHiding != 3U &&
            (slot.ownedLoops != 1 || now - slot.clearedAt > kClearGraceMs + slot.holsterWait);
        const bool following = Following(slot);
        if (!Owned(current, slot.owned)) {
            NoteEnding(slot, "interrupted", Cycle{}, now);
            slot.owned = {}; ++d.interrupted; d.stage = "retail-interrupted";
            Readmit(slot, following, now);
        } else if ((current.flags & 6U) != 0U) {
            NoteEnding(slot, "completed", current, now);
            if (Cancel(slot.character, slot.owned)) {
                slot.owned = {}; ++d.completed; d.stage = "completed";
                Readmit(slot, following, now);
            }
        } else if (expired || cleared) {
            NoteEnding(slot, expired ? "timeout" : "cleared", current, now);
            if (Cancel(slot.character, slot.owned)) {
                slot.owned = {}; ++d.cancelled; d.stage = expired ? "observation-timeout" : "cancelled";
                if (expired) slot.handled = 0U; // a fresh sample may resume the ongoing action
            }
            else { ++d.deferred; d.stage = "cancel-deferred"; }
        } else {
            if (slot.owned.graph == 0U && current.graph != 0U) {
                slot.owned.graph = current.graph; d.stage = "playing";
            }
            if (following) FollowOwnerClock(slot, current, now);
        }
    }
    if (!Active(slot.desired) || slot.handled == slot.desired.transition) {
        slot.holsterStarted = slot.unarmedAt = 0U;
        // A first-person player hidden in a locker has no weapon out: draw it
        // back only once the owner has left the locker.
        if (slot.owned.address == 0U && slot.holstered >= 0 && !player_hiding::Hidden(slot.character) &&
            EquipmentTransition(slot.character, true, slot.holstered))
            slot.holstered = -1;
        return;
    }
    d.transition = slot.desired.transition; d.animation = slot.desired.animation;
    const auto waited = slot.holsterStarted != 0U ? now - slot.holsterStarted : 0U;
    const auto observed = static_cast<std::uint64_t>(slot.desired.elapsedMilliseconds) + now - slot.receivedAt;
    const auto age = observed > waited ? observed - waited : 0U;
    if (age > kMaximumStartAgeMs) {
        ++d.stale; d.stage = "stale-observation";
        // A later, fresh phase sample may still join this ongoing action.
        return;
    }
    if (!Abi() || gIdleTransition.load() == 0U) {
        slot.handled = slot.desired.transition; ++d.failed; d.stage = "abi"; return;
    }
    // The hidden idle loop starts once our locker entry clip has finished.
    const auto hidingPhase = player_hiding_presentation::ClassifyClip(slot.desired.animation).phase;
    if (hidingPhase == 2U && Owned(current, slot.owned) && (current.flags & 6U) == 0U &&
        player_hiding_presentation::ClassifyClip(slot.owned.animation).phase == 1U) {
        ++d.deferred; d.stage = "hiding-entry-finishing"; return;
    }
    // Replacing our own clip is allowed; other scripts, physical actions,
    // deaths and paired attacks keep priority over a presentation.
    if (!Owned(current, slot.owned) && current.state != 1U && current.state != 3U && current.state != 0x38U) {
        ++d.deferred; d.stage = "cycle-busy"; return;
    }
    // Use native third-person actions when their clip differs from the FP
    // alias. The Launcher retains the retail base alias and adds its FP
    // counterpart under ReplayAlias for actions missing a body clip.
    std::uint32_t npcResource{}, npcPath{}, fpResource{}, fpPath{};
    const bool hasNpc = LocalClip(slot.character, slot.desired.animation, npcResource, npcPath);
    const bool hasFp = hidingPhase == 0U &&
        LocalClip(slot.character, ReplayAlias(slot.desired.animation), fpResource, fpPath);
    if (!hasNpc && !hasFp) {
        slot.handled = slot.desired.transition; ++d.unavailable; d.stage = "clip-unavailable"; return;
    }
    // Locker aliases are injected FP fallbacks, never original NPC clips.
    slot.nativeClip = hidingPhase == 0U && hasNpc && (!hasFp || npcPath != fpPath);
    slot.play = hasFp && !slot.nativeClip ? ReplayAlias(slot.desired.animation) : slot.desired.animation;
    d.resource = hasFp && !slot.nativeClip ? fpResource : npcResource;
    d.clipPath = hasFp && !slot.nativeClip ? fpPath : npcPath;
    d.clipSource = slot.nativeClip ? "npc-native" : "fp-retargeted";
    if (slot.holstered < 0 && slot.holsterStarted == 0U && !Unarmed(slot.character)) {
        std::int32_t holstered{-1};
        if (EquipmentTransition(slot.character, false, holstered)) {
            slot.holstered = holstered; slot.holsterStarted = now; slot.unarmedAt = 0U;
            d.stage = "holstering"; return;
        }
        ++d.deferred; d.stage = "equipment-busy"; return;
    }
    if (slot.holsterStarted != 0U) {
        const bool unarmed = Unarmed(slot.character);
        if (!unarmed) {
            d.stage = now - slot.holsterStarted < kHolsterTimeoutMs ? "holstering" : "holster-not-ready";
            return;
        }
        if (slot.unarmedAt == 0U) slot.unarmedAt = now;
        if (unarmed && now - slot.unarmedAt < kUnarmedSettleMs) { d.stage = "holstering"; return; }
    }
    slot.maskActive = false;
    slot.crewPose.fill(-1);
    d.maskTarget = d.maskCrewBones = d.maskCrewDriven = 0U;
    if (slot.nativeClip) {
        // The game's own full-body skeleton and root motion must not be
        // overwritten by first-person bone masks, IK and floor correction.
        d.maskStatus = 0xFFFFFFFDU;
        d.maskSkeleton = d.maskTracks = d.maskMoving = d.maskHeld = 0U;
    } else if (const auto* mask = FindMask(d.clipPath)) {
        d.maskStatus = mask->status; d.maskSkeleton = mask->skeleton; d.maskTracks = mask->tracks;
        d.maskMoving = mask->moving; d.maskHeld = mask->held;
        std::uint32_t lowBones{}, highBones{};
        if (gMaskHook.load(std::memory_order_acquire) && mask->status == 0U &&
            LayerBones(slot.character, 0U, lowBones) && LayerBones(slot.character, 7U, highBones) &&
            CrewDriven(*mask, lowBones, highBones, d.resource, slot.crewDriven, slot.crewDisplaced, d.maskTarget)) {
            slot.maskActive = true; d.maskCrewBones = highBones;
            d.maskCrewDriven = static_cast<std::uint32_t>(slot.crewDriven.count());
            static_cast<void>(MapCrewBones(*mask, lowBones, highBones, d.resource, slot.crewPose));
        }
    } else {
        d.maskStatus = 0xFFFFFFFFU; d.maskSkeleton = d.maskTracks = d.maskMoving = d.maskHeld = 0U;
        // No mask for this clip (vents, ...): the clip keeps every bone, but
        // the body is still kept on the floor and rebuilt from the owner's.
        std::uint32_t lowBones{}, highBones{};
        MaskClip firstPerson{};
        firstPerson.skeleton = kFirstPersonSkeleton;
        if (gMaskHook.load(std::memory_order_acquire) && gMaskFile >= 0 &&
            LayerBones(slot.character, 0U, lowBones) && LayerBones(slot.character, 7U, highBones) && highBones <= kMaskBones) {
            slot.maskActive = true; d.maskStatus = 0xFFFFFFFEU; d.maskCrewBones = highBones;
            slot.crewDriven.set(); slot.crewDisplaced.set();
            static_cast<void>(MapCrewBones(firstPerson, lowBones, highBones, d.resource, slot.crewPose));
        }
    }
    const character_direct_step_runtime::MovementResourceLease resources(slot.character);
    if (!resources) { ++d.deferred; d.stage = "resources"; return; }
    const auto readiness = PrepareStream(slot, slot.play);
    if (readiness == StreamReadiness::Failed) {
        slot.handled = slot.desired.transition; ++d.failed; d.stage = "stream-request"; return;
    }
    if (readiness == StreamReadiness::Waiting) {
        // Polled again next frame, until ready or the start-age limit.
        ++d.deferred; d.stage = "stream-loading"; return;
    }
    ai_sync::native_intent::Payload intent{};
    auto playback = slot.desired;
    playback.animation = slot.play;
    float phase{};
    // A locker entry/exit plays whole from its start: the body follows the
    // owner's recorded path at this clip's own time, so lateness costs nothing.
    auto projectedAge = age;
    if (hidingPhase == 1U || hidingPhase == 3U) { playback.phaseSeconds = 0.0F; projectedAge = 0U; }
    if (!timeline::Project(slot.desired, projectedAge, phase, playback.loops)) {
        slot.handled = slot.desired.transition; ++d.stale; d.stage = "finished-before-admission"; return;
    }
    // FP trim frames do not correspond to an NPC animation's native frames.
    if (slot.nativeClip) playback.startFrame = playback.endFrame = -1;
    if (!BuildIntent(playback, d.resource, intent)) {
        slot.handled = slot.desired.transition; ++d.failed; d.stage = "parameters"; return;
    }
    PublishMaskState(slot, maskShare, true);
    if (slot.maskActive && maskShare.baseBones.load(std::memory_order_acquire) == 0U) {
        ++d.maskNoBase; d.stage = "waiting-idle-pose";
        PublishMaskState(slot, maskShare);
        return;
    }
    // One placement at the start; afterwards the actor follows the owner's
    // own network path (the owner's Character is the one converging).
    // For an original NPC clip, start at the first observed interaction
    // transform, not the owner's current in-flight position after streaming,
    // holstering or network latency. Subsequent root movement belongs to retail.
    // A first observation already late into the clip remains an approximation,
    // reported as nativeAnchorDrift until full reference-frame reconstruction.
    const auto& initial = slot.nativeClip && slot.hasFirstOwnerPosition
        ? slot.firstOwnerPosition : slot.desired.position;
    float groundY{};
    auto heightOffset = slot.owned.address != 0U ? slot.heightOffset : 0.0F;
    if (slot.owned.address == 0U && Read(slot.character + 0x74U, groundY) && std::isfinite(groundY)) {
        heightOffset = groundY - initial[1];
        if (std::abs(heightOffset) > 0.3F) heightOffset = 0.0F; // a real level change
    }
    auto placement = initial;
    placement[1] += heightOffset;
    const auto heading = slot.nativeClip && slot.hasFirstOwnerPosition
        ? slot.firstOwnerYaw : slot.desired.yaw;
    if (slot.nativeClip) {
        const auto dx = slot.desired.position[0] - initial[0];
        const auto dy = slot.desired.position[1] - initial[1];
        const auto dz = slot.desired.position[2] - initial[2];
        d.nativeAnchorDrift = std::sqrt(dx * dx + dy * dy + dz * dz);
    }
    // Locker exits preserve the Character's current position; the visual
    // hiding track owns the pose inside the prop instead.
    auto mutableHeading = heading;
    // Retail keeps the Character entity in front of a locker; only its
    // rendered body is inside (CharacterDirectStepRuntime). Hidden idle and
    // exit play where the entity is; the exit turns it out of the prop.
    if (hidingPhase == 2U || hidingPhase == 3U) {
        float yaw{};
        if (!Read(slot.character + 0x70U, placement) || !Read(slot.character + 0x174U, yaw)) {
            ++d.deferred; d.stage = "alignment"; return;
        }
        heightOffset = 0.0F;
        if (hidingPhase == 2U) mutableHeading = static_cast<float>(std::remainder(yaw, 6.28318530717958647692F));
    }
    if (!character_direct_step_runtime::ApplyAIRecoveryTransform(slot.character,
        placement.data(), mutableHeading)) { ++d.deferred; d.stage = "alignment"; return; }
    slot.heightOffset = heightOffset;
    const auto before = current;
    if (!ReadCycle(slot.character, current) || !SameCycle(before, current) || !RemoteReady(slot.character)) {
        ++d.deferred; d.stage = "actor-changed"; return;
    }
    slot.handled = slot.desired.transition; // native call is attempted exactly once
    slot.holsterWait = waited; slot.holsterStarted = slot.unarmedAt = 0U;
    const bool invoked = Invoke(current.address, intent);
    const bool constructed = ReadCycle(slot.character, current) && current.runtime == before.runtime &&
        current.type == gBase.load() + kScriptedType && current.state == kScriptedState &&
        current.serial == static_cast<std::uint8_t>(before.serial + 1U) &&
        current.resource == d.resource && current.animation == slot.play;
    // Keep provenance even for a synchronous initializer failure (graph=0),
    // so the next pump neutralizes exactly that constructor result.
    if (constructed) {
        slot.owned = current; slot.startedAt = now; slot.ownedLoops = playback.loops;
        slot.ownerStartY = slot.desired.position[1];
        slot.ownedYaw = mutableHeading;
        slot.nativeRootLastValid = false;
        slot.lastSpatialAt = 0U;
    }
    if (!invoked || !constructed || (current.flags & 4U) != 0U || current.graph == 0U) {
        ++d.failed; d.stage = "native-admission"; return;
    }
    // Project the owner's live native clock through local IPC/network/stream
    // waiting time. Seek keeps the original trim interval for future repeats;
    // shifting startFrame would incorrectly shorten every subsequent loop.
    float nativeNow{}, nativeDuration{};
    if (slot.nativeClip && timeline::Read(current.graph, nativeNow, nativeDuration))
        phase = TargetClipPhase(slot, phase, nativeDuration);
    if (!timeline::Seek(current.graph, phase)) {
        if (Cancel(slot.character, slot.owned)) slot.owned = {};
        ++d.failed; d.stage = "phase-seek"; return;
    }
    ++d.applied; d.stage = "playing";
    // Probe window: idle frames just before this clip, then the clip itself.
    d.before = d.idle; d.idle = {}; d.during = {};
    std::uintptr_t graphPose{};
    d.scriptedPose = 0U;
    for (std::uint32_t layer = 0; layer < 8U; ++layer)
        if (Read(slot.character + 0x3F0U + 0x230U + layer * 4U, graphPose) && graphPose != 0U)
            d.scriptedPose |= 1U << layer;
}
bool PumpOne(const std::uintptr_t character) noexcept {
    Slot copy{};
    std::size_t index{};
    { const std::lock_guard lock(gMutex); const auto* slot = Find(character); if (slot == nullptr) return false;
      copy = *slot; index = static_cast<std::size_t>(slot - gSlots.data()); }
    PublishMaskState(copy, gMaskShares[index]);
    Advance(copy, GetTickCount64(), gMaskShares[index]);
    const std::lock_guard lock(gMutex);
    auto* slot = Find(character);
    if (slot == nullptr || slot->binding != copy.binding) return false;
    if (slot->epoch == copy.epoch) { *slot = copy; return true; }
    // A packet arrived during native playback: keep its desired state and
    // retain the provenance of what this pump actually did.
    slot->owned = copy.owned; slot->startedAt = copy.startedAt; slot->ownedLoops = copy.ownedLoops;
    slot->holstered = copy.holstered;
    slot->maskActive = copy.maskActive; slot->crewDriven = copy.crewDriven; slot->crewDisplaced = copy.crewDisplaced;
    slot->crewPose = copy.crewPose; slot->play = copy.play; slot->nativeClip = copy.nativeClip;
    if (slot->desired.transition == copy.desired.transition) {
        slot->nativeRootLastPosition = copy.nativeRootLastPosition;
        slot->nativeRootLastValid = copy.nativeRootLastValid;
    }
    slot->holsterStarted = copy.holsterStarted; slot->unarmedAt = copy.unarmedAt; slot->holsterWait = copy.holsterWait;
    slot->heightOffset = copy.heightOffset; slot->ownerStartY = copy.ownerStartY;
    slot->ownedYaw = copy.ownedYaw;
    slot->diagnostics = copy.diagnostics; slot->resyncAt = copy.resyncAt;
    if (slot->desired.transition == copy.desired.transition) { slot->handled = copy.handled; slot->readmits = copy.readmits; }
    return true;
}

// --- Owner capture -----------------------------------------------------------
// Owner observation (diagnostic): every distinct cycle kind met on the local
// player's primary/equipment slots, and the first-person aliases its own clip
// database resolved, so an interaction that bypasses both is identifiable.
struct OwnerCycleRow final { std::uint32_t slot{}, type{}, state{}, animation{}; std::uint64_t frames{}; };
std::array<OwnerCycleRow, 48> gOwnerCycles{};
struct OwnerAliasRow final { std::uint32_t name{}, path{}; std::uint64_t count{}, lastTick{}; };
std::array<OwnerAliasRow, 64> gOwnerAliases{};
std::atomic<std::uint32_t> gOwnerDatabase{};
void ObserveRow(const std::uint32_t slot, const Cycle& cycle) noexcept {
    const auto base = gBase.load();
    const OwnerCycleRow row{slot, static_cast<std::uint32_t>(cycle.type - base), cycle.state,
        cycle.animation, 1U};
    const std::lock_guard lock(gLocalMutex);
    for (auto& entry : gOwnerCycles) {
        if (entry.frames == 0U) { entry = row; return; }
        if (entry.slot == row.slot && entry.type == row.type && entry.state == row.state && entry.animation == row.animation) {
            ++entry.frames; return;
        }
    }
}
void ObserveOwnerCycles(const std::uintptr_t character, const Cycle& primary) noexcept {
    ObserveRow(0x36CU, primary);
    Cycle equipment{};
    if (ReadCycle(character, equipment, kEquipmentCycle)) ObserveRow(0x388U, equipment);
    std::uintptr_t instance{}, database{};
    std::uint32_t identity{};
    if (Read(character + 0x720U, instance) && Read(instance + 0x10U, database) && Read(database + 0x10U, identity))
        gOwnerDatabase.store(identity, std::memory_order_relaxed);
}
float WrapYaw(const float yaw) noexcept {
    return static_cast<float>(std::remainder(yaw, 6.28318530717958647692F));
}
// true + inactive State = no scripted clip; false = torn/unavailable sample.
bool CaptureLocal(const std::uintptr_t character, const std::array<std::uint32_t, 4>& attachments, State& output) noexcept {
    output = {};
    Cycle before{}, after{};
    if (gBase.load() == 0U || !ReadCycle(character, before)) return false;
    ObserveOwnerCycles(character, before);
    // First-person item interactions (hacking device, tools...) may run their
    // clip on the equipment cycle while the primary one stays in idle.
    std::uintptr_t cycleSlot = kPrimaryCycle;
    if (!Scripted(before)) {
        Cycle equipment{};
        if (ReadCycle(character, equipment, kEquipmentCycle) && Scripted(equipment)) {
            before = equipment; cycleSlot = kEquipmentCycle;
        }
    }
    const auto now = GetTickCount64();
    // Hiding_logic_common sometimes animates only THE_PLAYER's first-person
    // camera/hands: Character+36C remains in idle. Use the already-validated
    // native door edge as an alternate observation, not another remote player.
    const auto hiding = player_hiding_local::Read();
    const bool hidingCurrent = hiding.valid && hiding.ticket.character == character &&
        character_incarnations::NativeRegistry().Current(hiding.ticket);
    if (!Scripted(before)) {
        if (hidingCurrent && hiding.state.animate &&
            hiding.state.phase >= 1U && hiding.state.phase <= 3U) {
            // Phase 2 is the hidden idle (crouched in a small locker), looped
            // for as long as the owner stays hidden.
            const bool idle = hiding.state.phase == 2U;
            const auto token = player_hiding_presentation::ClipToken(hiding.state.kind, hiding.state.phase);
            // When this same edge was already captured as a native Character
            // script, never replay it a second time after that script ends.
            if (token != 0U && gLocal.character == character &&
                gLocal.hidingTransition == hiding.state.transition && gLocal.cycle != 0U) {
                if (Active(gLocal.state)) {
                    const std::lock_guard lock(gLocalMutex); ++gLocalDiagnostics.cleared;
                }
                gLocal.state = {};
                return true;
            }
            // A late first sample must not start the clip far into it, but an
            // admitted transition keeps streaming its path to the clip's end.
            const bool streaming = gLocal.character == character && gLocal.cycle == 0U &&
                gLocal.hidingTransition == hiding.state.transition && Active(gLocal.state);
            if (token != 0U && (idle || ((streaming || hiding.state.elapsedMilliseconds <= 2500U) &&
                hiding.state.elapsedMilliseconds <= 7900U))) {
                const bool same = gLocal.character == character && gLocal.cycle == 0U &&
                    gLocal.runtime == before.runtime && gLocal.animation == token &&
                    gLocal.hidingTransition == hiding.state.transition && Active(gLocal.state);
                State candidate{};
                candidate.transition = same ? gLocal.state.transition : ++gLocalTransition;
                candidate.animation = token;
                // The first-person clip's real path is the owner's Character
                // transform itself (its root motion): send it live, keyed by
                // the owner's clip time, so observers replay exactly that path.
                std::array<float, 3> live{};
                float liveYaw{};
                if (Read(character + 0x70U, live) && Read(character + 0x174U, liveYaw) && std::isfinite(liveYaw)) {
                    candidate.position = live; candidate.yaw = WrapYaw(liveYaw);
                } else {
                    candidate.position = hiding.state.position; candidate.yaw = hiding.state.yaw;
                }
                // Hiding's edge clock is not an hkaAnimation graph clock. The
                // duration is a safe admission envelope, not a fabricated
                // retail clip duration; the receiver plays its own native clip.
                candidate.phaseSeconds = idle ? 0.0F : (std::min)(7.9F, hiding.state.elapsedMilliseconds * 0.001F);
                if (idle) candidate.loops = -1;
                candidate.durationSeconds = 8.0F;
                candidate.attachments = attachments;
                if (Valid(candidate)) {
                    if (!same) {
                        const std::lock_guard lock(gLocalMutex);
                        ++gLocalDiagnostics.captured; ++gLocalDiagnostics.fallbackCaptured;
                        gLocalDiagnostics.animation = token; gLocalDiagnostics.resource = 0U;
                    }
                    gLocal = {character, 0U, before.runtime, token, 0U, now,
                        candidate, hiding.state.transition};
                    output = candidate;
                    return true;
                }
            }
        }
        if (gLocal.character == character && Active(gLocal.state)) {
            const std::lock_guard lock(gLocalMutex); ++gLocalDiagnostics.cleared;
        }
        gLocal = {}; gLocal.character = character;
        return true;
    }
    const auto classified = player_hiding_presentation::ClassifyClip(before.animation);
    const auto hidingTransition = hidingCurrent && classified.phase == hiding.state.phase &&
        classified.kind == hiding.state.kind ? hiding.state.transition : 0U;
    const bool same = (gLocal.character == character && gLocal.cycle == before.address &&
        gLocal.serial == before.serial && gLocal.runtime == before.runtime &&
        gLocal.animation == before.animation && Active(gLocal.state)) ||
        // A first-person-only fallback that subsequently gains a Character
        // script is still the same interaction, not a second remote replay.
        (gLocal.character == character && gLocal.cycle == 0U &&
         gLocal.runtime == before.runtime && gLocal.animation == before.animation &&
         hidingTransition != 0U && gLocal.hidingTransition == hidingTransition &&
         Active(gLocal.state));
    // +70/+74 are the original bounds. The completion callback 0064F290
    // decrements +A8, not the constructor's +78 count; use the remaining
    // count when joining a finite multi-loop action after it has started.
    State candidate{};
    std::array<float, 3> position{};
    float yaw{};
    if (!Read(before.address + 0x70U, candidate.startFrame) || !Read(before.address + 0x74U, candidate.endFrame) ||
        !Read(before.address + 0xA8U, candidate.loops) || !Read(before.address + 0x7CU, candidate.speed) ||
        !Read(before.address + 0x80U, candidate.blendIn) ||
        !Read(character + 0x70U, position) || !Read(character + 0x174U, yaw) ||
        !timeline::Read(before.graph, candidate.phaseSeconds, candidate.durationSeconds) ||
        !ReadCycle(character, after, cycleSlot) || !SameCycle(before, after) || before.graph != after.graph ||
        before.resource != after.resource || before.animation != after.animation || before.flags != after.flags) {
        const std::lock_guard lock(gLocalMutex); ++gLocalDiagnostics.torn;
        return false;
    }
    candidate.transition = same ? gLocal.state.transition : ++gLocalTransition;
    candidate.animation = before.animation;
    candidate.attachments = attachments;
    candidate.position = position;
    candidate.yaw = WrapYaw(yaw);
    {
        // The first-person body as the pose hook last composed it in this clip.
        const std::lock_guard lock(gOwnerPose.mutex);
        if (gOwnerPose.fresh && now >= gOwnerPose.at && now - gOwnerPose.at <= 100U) {
            candidate.posed = true; candidate.effectors = gOwnerPose.effectors;
        }
    }
    // Out-of-contract parameters fall back to the retail defaults rather than
    // dropping the action: the receiver only needs the alias.
    if (candidate.startFrame < -1 || candidate.startFrame > 1000000) candidate.startFrame = -1;
    if (candidate.endFrame < -1 || candidate.endFrame > 1000000) candidate.endFrame = -1;
    if (candidate.loops < -1 || candidate.loops > 65535) candidate.loops = 1;
    if (!std::isfinite(candidate.speed) || candidate.speed <= 0.0F || candidate.speed > 64.0F) candidate.speed = 1.0F;
    if (!std::isfinite(candidate.blendIn) || candidate.blendIn < 0.0F || candidate.blendIn > 60.0F) candidate.blendIn = 0.2F;
    if (!Valid(candidate)) {
        const std::lock_guard lock(gLocalMutex); ++gLocalDiagnostics.unavailable;
        return false;
    }
    gLocal = {character, before.address, before.runtime, before.animation, before.serial, now,
        candidate, hidingTransition};
    {
        const std::lock_guard lock(gLocalMutex);
        if (!same) ++gLocalDiagnostics.captured;
        if (candidate.posed) ++gLocalDiagnostics.posed;
        gLocalDiagnostics.animation = before.animation; gLocalDiagnostics.resource = before.resource;
    }
    output = candidate;
    return true;
}
}

std::string Encode(const State& value) {
    if (!Active(value)) return "0";
    std::ostringstream text;
    text.imbue(std::locale::classic());
    text << std::setprecision(std::numeric_limits<float>::max_digits10)
         << value.transition << ',' << value.animation << ',' << value.elapsedMilliseconds << ','
         << value.startFrame << ',' << value.endFrame << ',' << value.loops << ','
         << value.speed << ',' << value.blendIn << ',' << value.position[0] << ','
         << value.position[1] << ',' << value.position[2] << ',' << value.yaw << ','
         << value.phaseSeconds << ',' << value.durationSeconds;
    for (const auto attachment : value.attachments) text << ',' << attachment;
    if (value.posed) for (const auto coordinate : value.effectors) text << ',' << coordinate;
    return text.str();
}

void Initialize(const std::uintptr_t moduleBase, const std::uintptr_t nativeIdleTransition) noexcept {
    ForgetAll();
    gBase.store(moduleBase);
    gIdleTransition.store(nativeIdleTransition);
    timeline::Initialize(moduleBase);
    LoadMasks();
}

void PumpLocalOnEngineThread() noexcept {
    auto& registry = character_incarnations::NativeRegistry();
    const auto ticket = registry.Local();
    LocalSnapshot capture{};
    if (ticket.character != 0U && registry.Current(ticket)) {
        capture.ticket = ticket;
        const auto attachments = props::CaptureOnEngineThread(ticket.character);
        capture.valid = CaptureLocal(ticket.character, attachments, capture.state) &&
            registry.Current(ticket) && registry.Local() == ticket;
        capture.capturedAt = GetTickCount64();
        // The pose hook samples this body: its standing frame between clips,
        // its effectors during a native scripted clip.
        gOwnerPose.controller.store(ticket.character + 0x3F0U, std::memory_order_release);
        gOwnerPose.clip.store(capture.valid && Active(capture.state) && gLocal.cycle != 0U, std::memory_order_release);
    } else {
        gLocal = {};
        static_cast<void>(props::CaptureOnEngineThread(0U));
        gOwnerPose.controller.store(0U, std::memory_order_release);
        gOwnerPose.clip.store(false, std::memory_order_release);
    }
    const std::lock_guard lock(gLocalMutex);
    gLocalSnapshot = capture;
}
void ObserveResolvedAlias(const std::uint32_t database, const std::uint32_t name, const std::uint32_t path) noexcept {
    if (database == 0U || database != gOwnerDatabase.load(std::memory_order_relaxed)) return;
    const auto now = GetTickCount64();
    const std::lock_guard lock(gLocalMutex);
    OwnerAliasRow* oldest = &gOwnerAliases[0];
    for (auto& row : gOwnerAliases) {
        if (row.count != 0U && row.name == name && row.path == path) { ++row.count; row.lastTick = now; return; }
        if (row.count == 0U) { oldest = &row; break; }
        if (row.lastTick < oldest->lastTick) oldest = &row;
    }
    *oldest = {name, path, 1U, now};
}
LocalSnapshot ReadLocal() noexcept {
    const std::lock_guard lock(gLocalMutex);
    return gLocalSnapshot;
}

// Owner body effectors keyed by its clip time (gMutex held; share.mutex nests).
void RecordPose(const std::size_t index, const State& state) noexcept {
    if (!state.posed || player_hiding_presentation::ClassifyClip(state.animation).phase != 0U) return;
    auto& share = gMaskShares[index];
    const std::lock_guard lock(share.mutex);
    auto& track = share.track;
    if (track.transition != state.transition) { track.transition = state.transition; track.count = 0U; }
    const MaskShare::PoseSample sample{state.phaseSeconds, state.effectors};
    if (track.count != 0U && (sample.time <= track.samples[track.count - 1U].time + 0.0001F ||
            track.count == track.samples.size())) track.samples[track.count - 1U] = sample;
    else track.samples[track.count++] = sample;
}

bool Publish(const std::uintptr_t character, const State& state, std::string& error) noexcept {
    if (character < 0x10000U || !Valid(state)) {
        error = "Animation sync requires a Character binding and a valid clip state"; return false;
    }
    const std::lock_guard lock(gMutex);
    auto* slot = Find(character);
    if (slot == nullptr) {
        // Bind on the first state, clip or not: the bone mask needs this
        // representation's idle pose from before its first interaction.
        for (auto& candidate : gSlots) if (candidate.character == 0U) {
            candidate = {}; candidate.character = character; candidate.binding = ++gEpoch;
            gTracks[static_cast<std::size_t>(&candidate - gSlots.data())] = {};
            slot = &candidate; break;
        }
        if (slot == nullptr) { error = "Animation sync mailbox is full"; return false; }
    }
    const auto now = GetTickCount64();
    if (!Active(state)) {
        if (Active(slot->desired)) { slot->desired = {}; slot->clearedAt = now; slot->epoch = ++gEpoch; }
        return true;
    }
    if (state.transition < slot->highest) return true;
    if (state.transition == slot->highest) {
        // A phase refresh renews the observation lease without restarting the
        // native cycle. A clear is final for this ordered transition.
        if (Active(slot->desired) && slot->desired.animation != state.animation) {
            error = "Animation sync transition identity changed its clip"; return false;
        }
        if (Active(slot->desired)) {
            // Owner clock rate between distinct samples (sample time = now - age).
            const auto sampleAt = now - state.elapsedMilliseconds;
            if (sampleAt > slot->rateSampleAt + 200U) {
                const float advanced = state.phaseSeconds - slot->rateSamplePhase;
                if (advanced >= 0.0F)
                    slot->ownerRate = advanced * 1000.0F / static_cast<float>(sampleAt - slot->rateSampleAt);
                slot->rateSamplePhase = state.phaseSeconds; slot->rateSampleAt = sampleAt;
            }
            slot->desired = state; slot->receivedAt = now; slot->epoch = ++gEpoch;
            Record(static_cast<std::size_t>(slot - gSlots.data()), state);
            RecordPose(static_cast<std::size_t>(slot - gSlots.data()), state);
        }
        return true;
    }
    slot->firstOwnerPosition = state.position; slot->firstOwnerYaw = state.yaw;
    slot->hasFirstOwnerPosition = true; slot->nativeRootLastValid = false;
    slot->lastSpatialAt = 0U;
    slot->ownerRate = -1.0F; slot->rateSamplePhase = state.phaseSeconds;
    slot->rateSampleAt = now - state.elapsedMilliseconds; slot->readmits = 0U;
    slot->desired = state; slot->highest = state.transition;
    slot->receivedAt = now; slot->clearedAt = 0U; slot->epoch = ++gEpoch;
    Record(static_cast<std::size_t>(slot - gSlots.data()), state);
    RecordPose(static_cast<std::size_t>(slot - gSlots.data()), state);
    return true;
}

void PumpAllOnEngineThread() noexcept {
    CollectStreams();
    std::array<std::uintptr_t, kSlots> characters{};
    { const std::lock_guard lock(gMutex); for (std::size_t i = 0; i < gSlots.size(); ++i) characters[i] = gSlots[i].character; }
    for (const auto previous : gPropCharacters)
        if (previous != 0U && std::find(characters.begin(), characters.end(), previous) == characters.end())
            props::ForgetOnEngineThread(previous);
    gPropCharacters = characters;
    for (const auto character : characters) if (character != 0U) {
        static_cast<void>(PumpOne(character));
        std::array<std::uint32_t, 4> attachments{};
        {
            const std::lock_guard lock(gMutex);
            if (const auto* slot = Find(character); slot != nullptr && slot->owned.address != 0U &&
                Active(slot->desired) && GetTickCount64() - slot->receivedAt + slot->desired.elapsedMilliseconds <= kObservationTimeoutMs)
                attachments = slot->desired.attachments;
        }
        // Tokens can change during a single clip. Reconcile after admission
        // and outside the mailbox lock: native graph creation calls back.
        props::ReconcileOnEngineThread(character, attachments);
    }
    CollectStreams();
}

bool OwnsMovement(const std::uintptr_t character) noexcept {
    const std::lock_guard lock(gMutex);
    const auto* slot = Find(character);
    // Deferred requests cannot freeze locomotion or steal a retail cycle.
    Cycle current{};
    return slot != nullptr && RemoteReady(character) && ReadCycle(character, current) && Owned(current, slot->owned);
}
float HeightOffset(const std::uintptr_t character, const float ownerY) noexcept {
    const std::lock_guard lock(gMutex);
    const auto* slot = Find(character);
    return slot != nullptr && slot->owned.address != 0U
        ? PosturalHeightOffset(slot->heightOffset, slot->ownerStartY, ownerY) : 0.0F;
}
float PlaybackYaw(const std::uintptr_t character, const float ownerYaw) noexcept {
    const std::lock_guard lock(gMutex);
    const auto* slot = Find(character);
    if (slot == nullptr || slot->owned.address == 0U) return ownerYaw;
    const auto phase = player_hiding_presentation::ClassifyClip(slot->owned.animation).phase;
    // An interaction bound to a locker handle needs the heading captured at
    // admission, not the moving first-person camera while inside the locker.
    return phase == 1U || phase == 3U ? slot->ownedYaw : ownerYaw;
}
MovementPolicy MovementPolicyFor(const std::uintptr_t character) noexcept {
    const std::lock_guard lock(gMutex);
    const auto* slot = Find(character);
    Cycle current{};
    // Keep ownership through the completed-but-not-yet-retired native
    // cycle. The next AnimationSync pump cancels it before PlayerSync resumes.
    if (slot == nullptr || !RemoteReady(character) ||
        !ReadCycle(character, current) || !Owned(current, slot->owned))
        return MovementPolicy::None;
    // enter_vent is the sole native clip known to lose physical convergence
    // around 1.41 seconds: retain its animation and correct the render root.
    return SelectMovementPolicy(true, slot->nativeClip,
        player_hiding_presentation::ClassifyClip(slot->owned.animation).phase != 0U,
        slot->nativeClip && slot->owned.animation == 256590389U);
}
bool ClipPlaying(const std::uintptr_t character) noexcept {
    return OwnsMovement(character);
}
bool HidingClipPlaying(const std::uintptr_t character, const std::uint32_t phase) noexcept {
    const std::lock_guard lock(gMutex);
    const auto* slot = Find(character);
    Cycle current{};
    return slot != nullptr && slot->owned.address != 0U && RemoteReady(character) &&
        ReadCycle(character, current) && Owned(current, slot->owned) &&
        current.graph != 0U && (current.flags & 6U) == 0U &&
        player_hiding_presentation::ClassifyClip(slot->owned.animation).phase == phase;
}
bool HidingEntryPlaying(const std::uintptr_t character) noexcept {
    return HidingClipPlaying(character, 1U);
}
bool SetVisualOffset(const std::uintptr_t character, const bool active,
    const std::array<float, 3>& offset, const float yaw, const bool stableRoot) noexcept {
    std::size_t index{};
    std::uint32_t resource{};
    float ownerStartY{};
    bool hasOwnerStart{};
    float ownerPhase{};
    {
        const std::lock_guard lock(gMutex);
        const auto* slot = Find(character);
        if (slot == nullptr) return false;
        index = static_cast<std::size_t>(slot - gSlots.data());
        if (active && stableRoot) {
            resource = slot->diagnostics.resource;
            ownerStartY = slot->firstOwnerPosition[1];
            hasOwnerStart = slot->hasFirstOwnerPosition;
            if (Active(slot->desired) && slot->desired.durationSeconds > 0.0F)
                ownerPhase = (std::clamp)(
                    slot->desired.phaseSeconds / slot->desired.durationSeconds, 0.0F, 1.0F);
            else ownerPhase = 1.0F; // owner already cleared its clip
        }
    }
    auto& share = gMaskShares[index];
    const std::lock_guard lock(share.mutex);
    const auto controller = character + 0x3F0U;
    if (share.controller.load() != controller) return false;
    if (!active) {
        share.offsetActive.store(false, std::memory_order_release);
        share.stableRoot = false; share.rootReferenceReady = false;
        share.ventGroundLift = 0.0F; share.ventMapped = false;
        return true;
    }
    if (!gMaskHook.load(std::memory_order_acquire)) return false;
    std::uint32_t bones{};
    if (share.parentCount == 0U || !LayerBones(character, 7U, bones) || bones != share.parentCount)
        ReadParents(controller, share);
    if (share.parentCount == 0U) return false;
    if (share.stableRoot != stableRoot ||
        !share.offsetActive.load(std::memory_order_acquire)) {
        share.rootReferenceReady = false;
        share.ventGroundLift = 0.0F;
        share.ventMapped = false;
    }
    share.stableRoot = stableRoot;
    if (stableRoot) {
        float actorY{};
        if (hasOwnerStart && Read(character + 0x74U, actorY) &&
            std::isfinite(actorY) && std::isfinite(offset[1]))
            share.ventRise = (std::clamp)(
                actorY + offset[1] - ownerStartY, 0.0F, 1.0F);
        share.ventProgress = ownerPhase;
        // The launcher already exports retail FEMALEFP->FEMALE->crew maps.
        // Avoid any guessed bone indices; fail closed if the mapping is not
        // unique or the current representation has a different skeleton.
        if (!share.ventMapped && resource != 0U) {
            std::uint32_t lowBones{}, highBones{};
            MaskClip firstPerson{};
            firstPerson.skeleton = kFirstPersonSkeleton;
            if (LayerBones(character, 0U, lowBones) &&
                LayerBones(character, 7U, highBones) &&
                highBones == share.parentCount &&
                MapCrewBones(firstPerson, lowBones, highBones,
                    resource, share.crewPose))
                share.ventMapped = true;
        }
    }
    share.offset = {offset[0], offset[1], offset[2], yaw};
    share.offsetActive.store(true, std::memory_order_release);
    return true;
}
VisualRootStats VisualRootSnapshot(const std::uintptr_t character) noexcept {
    std::size_t index{};
    {
        const std::lock_guard lock(gMutex);
        const auto* slot = Find(character);
        if (slot == nullptr) return {};
        index = static_cast<std::size_t>(slot - gSlots.data());
    }
    auto& share = gMaskShares[index];
    const std::lock_guard lock(share.mutex);
    return {share.offsetFrames.load(std::memory_order_relaxed),
        share.stableFrames, share.idleReferences, share.liveReferences,
        share.maxSuppressedRootDeltaY,
        share.ventGroundFrames, share.ventGroundSkipped, share.ventGroundMapped,
        share.ventMaxGroundLift, share.ventMaxDepthBeforeGround,
        share.ventMaxHipDrop, share.ventLastFootY, share.ventIdleFootY};
}
std::uint64_t VisualOffsetFrames(const std::uintptr_t character) noexcept {
    std::size_t index{};
    {
        const std::lock_guard lock(gMutex);
        const auto* slot = Find(character);
        if (slot == nullptr) return 0U;
        index = static_cast<std::size_t>(slot - gSlots.data());
    }
    return gMaskShares[index].offsetFrames.load(std::memory_order_relaxed);
}
bool HidingClipProgress(const std::uintptr_t character, const std::uint32_t hidingPhase, float& progress) noexcept {
    std::uintptr_t graph{};
    {
        const std::lock_guard lock(gMutex);
        const auto* slot = Find(character);
        Cycle current{};
        if (slot == nullptr || slot->owned.address == 0U || !ReadCycle(character, current) ||
            !Owned(current, slot->owned) || current.graph == 0U ||
            player_hiding_presentation::ClassifyClip(slot->owned.animation).phase != hidingPhase) return false;
        graph = current.graph;
    }
    float phase{}, duration{};
    if (!timeline::Read(graph, phase, duration) || !(duration > 0.0F)) return false;
    progress = (std::clamp)(phase / duration, 0.0F, 1.0F);
    return std::isfinite(progress);
}
bool HidingClipOwned(const std::uintptr_t character) noexcept {
    const std::lock_guard lock(gMutex);
    const auto* slot = Find(character);
    Cycle current{};
    return slot != nullptr && slot->owned.address != 0U && ReadCycle(character, current) &&
        Owned(current, slot->owned) && player_hiding_presentation::ClassifyClip(slot->owned.animation).phase != 0U;
}
bool HidingTarget(const std::uintptr_t character, std::array<float, 3>& position, float& yaw) noexcept {
    std::uintptr_t graph{};
    HidingTrack track{};
    {
        const std::lock_guard lock(gMutex);
        const auto* slot = Find(character);
        if (slot == nullptr) return false;
        auto& tracks = gTracks[static_cast<std::size_t>(slot - gSlots.data())];
        Cycle current{};
        const auto ownedPhase = slot->owned.address != 0U && ReadCycle(character, current) &&
            Owned(current, slot->owned) && current.graph != 0U
            ? player_hiding_presentation::ClassifyClip(slot->owned.animation).phase : 0U;
        if (ownedPhase != 0U && tracks[ownedPhase - 1U].count != 0U) {
            auto& live = tracks[ownedPhase - 1U];
            live.played = true;
            if (ownedPhase != 2U) graph = current.graph;
            track = live;
        } else {
            // No locker clip of ours runs: an exit not started yet holds its
            // start; otherwise the latest phase this body has reached.
            const auto& entry = tracks[0];
            const auto& hidden = tracks[1];
            const auto& exit = tracks[2];
            const HidingSample* sample{};
            if (exit.count != 0U) sample = exit.played ? &exit.samples[exit.count - 1U] : &exit.samples[0];
            else if (hidden.count != 0U && (entry.played || entry.count == 0U)) sample = &hidden.samples[hidden.count - 1U];
            else if (entry.count != 0U && entry.played) sample = &entry.samples[entry.count - 1U];
            if (sample == nullptr) return false;
            position = sample->position; yaw = sample->yaw;
            return true;
        }
    }
    const auto* first = &track.samples[0];
    const auto* last = &track.samples[track.count - 1U];
    if (graph == 0U) { position = last->position; yaw = last->yaw; return true; }
    float time{}, duration{};
    if (!timeline::Read(graph, time, duration) || !std::isfinite(time)) time = last->time;
    if (time <= first->time) { position = first->position; yaw = first->yaw; return true; }
    if (time >= last->time) { position = last->position; yaw = last->yaw; return true; }
    std::uint32_t i = 1U;
    while (i < track.count && track.samples[i].time < time) ++i;
    const auto& a = track.samples[i - 1U];
    const auto& b = track.samples[i];
    const auto span = b.time - a.time;
    const auto t = span > 0.0F ? (time - a.time) / span : 1.0F;
    for (std::size_t k = 0; k < 3U; ++k) position[k] = a.position[k] + (b.position[k] - a.position[k]) * t;
    yaw = static_cast<float>(std::remainder(
        a.yaw + std::remainder(b.yaw - a.yaw, 6.28318530717958647692F) * t, 6.28318530717958647692F));
    return true;
}
void ClearHidingTrack(const std::uintptr_t character) noexcept {
    const std::lock_guard lock(gMutex);
    if (const auto* slot = Find(character)) gTracks[static_cast<std::size_t>(slot - gSlots.data())] = {};
}
bool HidingEntryProgress(const std::uintptr_t character, float& progress) noexcept {
    return HidingClipProgress(character, 1U, progress);
}
bool HidingExitProgress(const std::uintptr_t character, float& progress) noexcept {
    return HidingClipProgress(character, 3U, progress);
}
bool HidingIdlePlaying(const std::uintptr_t character) noexcept {
    return HidingClipPlaying(character, 2U);
}
bool HidingExitPlaying(const std::uintptr_t character) noexcept {
    return HidingClipPlaying(character, 3U);
}
bool HidingTransitionPlaying(const std::uintptr_t character) noexcept {
    return HidingEntryPlaying(character) || HidingExitPlaying(character);
}
void Forget(const std::uintptr_t character) noexcept {
    const std::lock_guard lock(gMutex);
    if (auto* slot = Find(character)) { gTracks[static_cast<std::size_t>(slot - gSlots.data())] = {}; *slot = {}; }
}
void ForgetAll() noexcept {
    { const std::lock_guard lock(gMutex); gSlots = {}; gTracks = {}; ++gEpoch; }
    { const std::lock_guard lock(gLocalMutex); gLocalSnapshot = {}; }
}

std::string LayerText(const LayerProbes& probes) {
    std::ostringstream text;
    text.imbue(std::locale::classic());
    for (std::size_t layer = 0; layer < probes.size(); ++layer) {
        const auto& p = probes[layer];
        if (layer != 0U) text << ' ';
        text << 'L' << layer << ':';
        if (p.samples == 0U) text << '-';
        else text << p.bones << "b/" << p.samples << "s/" << p.changes << 'c';
    }
    return text.str();
}
using LayerBlend = void(__thiscall*)(void*, std::uint32_t, std::uint32_t, void*);
// hkaPose: skeleton +0, LOCAL hkArray +4, MODEL hkArray +0x10,
// bone flags +0x1C, sync flags +0x28/+0x29. The native accessors synchronize
// locals on read and invalidate every model transform on write. Writing +0x10
// alone is ineffective: lazy model evaluation overwrites it from stale locals.
std::uintptr_t LocalPoseTransforms(void* pose, const bool write) noexcept {
#if defined(_MSC_VER) && defined(_M_IX86)
    __try {
        using Access = std::uintptr_t*(__fastcall*)(void*);
        const auto accessor = reinterpret_cast<Access>(gBase.load() + (write ? 0x7B9B60U : 0x7BAF10U));
        return *accessor(pose);
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 0U; }
#else
    return 0U;
#endif
}
// Model-space bone positions of LOCAL hkQsTransforms (translation, quaternion
// x,y,z,w); scale is 1 on these rigs. Returns false on a non-finite pose.
using LocalBone = std::array<float, 12>;
bool ModelPositions(const LocalBone* local, const std::int16_t* parents, const std::uint32_t bones,
    std::array<float, 3>* position, std::array<float, 4>* rotation) noexcept {
    for (std::uint32_t b = 0; b < bones; ++b) {
        const auto& l = local[b];
        const std::array<float, 4> q{l[4], l[5], l[6], l[7]};
        const auto p = parents[b];
        if (p < 0) { position[b] = {l[0], l[1], l[2]}; rotation[b] = q; }
        else {
            const auto& r = rotation[p];
            // v' = v + 2w(u x v) + 2 u x (u x v), u = r.xyz
            const float tx = 2.0F * (r[1] * l[2] - r[2] * l[1]);
            const float ty = 2.0F * (r[2] * l[0] - r[0] * l[2]);
            const float tz = 2.0F * (r[0] * l[1] - r[1] * l[0]);
            position[b] = {position[p][0] + l[0] + r[3] * tx + (r[1] * tz - r[2] * ty),
                           position[p][1] + l[1] + r[3] * ty + (r[2] * tx - r[0] * tz),
                           position[p][2] + l[2] + r[3] * tz + (r[0] * ty - r[1] * tx)};
            rotation[b] = {r[3] * q[0] + r[0] * q[3] + r[1] * q[2] - r[2] * q[1],
                           r[3] * q[1] - r[0] * q[2] + r[1] * q[3] + r[2] * q[0],
                           r[3] * q[2] + r[0] * q[1] - r[1] * q[0] + r[2] * q[3],
                           r[3] * q[3] - r[0] * q[0] - r[1] * q[1] - r[2] * q[2]};
        }
        for (const auto value : position[b]) if (!std::isfinite(value)) return false;
    }
    return true;
}
// A first-person clip retargeted onto a crew body bends Ripley's legs under the
// crew's longer bones: the feet go through the floor and the body looks sunk.
// Keep the lowest limb extremity at the height it has in the representation's
// own idle pose by moving the skeleton roots (the placement) along the body's
// up axis. Only deep bones (depth >= 3: feet, toes, hands) count, never the
// roots or helpers hanging from them.
// The FP camera/root offset must never be mistaken for the body floor.
// Only the mapped FOOT and TOE bones define the grounded contact reference.
// Y is the engine's physical up axis (Character model space).
void GroundFinalPose(const std::uintptr_t transforms, MaskShare& share, const std::uint32_t bones) noexcept {
    if (share.parentCount != bones || bones == 0U) return;
    struct Scratch final {
        std::array<LocalBone, kMaskBones> idle, pose;
        std::array<std::array<float, 3>, kMaskBones> idleAt, poseAt;
        std::array<std::array<float, 4>, kMaskBones> rotation;
    };
    thread_local auto* scratch = new (std::nothrow) Scratch{};
    if (scratch == nullptr) return;
    auto& [idle, pose, idleAt, poseAt, rotation] = *scratch;
    if (!crash_guard::TryCopy(pose.data(), reinterpret_cast<const void*>(transforms),
        bones * sizeof(LocalBone))) return;
    for (std::uint32_t b = 0; b < bones; ++b)
        std::memcpy(idle[b].data(), share.base[b].data(), sizeof(LocalBone));
    if (!ModelPositions(idle.data(), share.parents.data(), bones, idleAt.data(), rotation.data()) ||
        !ModelPositions(pose.data(), share.parents.data(), bones, poseAt.data(), rotation.data())) return;
    float idleFloor{std::numeric_limits<float>::max()}, poseFloor{idleFloor};
    for (const auto bone : {kFpLeftFoot, kFpLeftToe, kFpRightFoot, kFpRightToe}) {
        const auto mapped = share.crewPose[bone];
        if (mapped < 0 || static_cast<std::uint32_t>(mapped) >= bones) return;
        idleFloor = (std::min)(idleFloor, idleAt[static_cast<std::size_t>(mapped)][1]);
        poseFloor = (std::min)(poseFloor, poseAt[static_cast<std::size_t>(mapped)][1]);
    }
    if (!std::isfinite(idleFloor) || !std::isfinite(poseFloor)) return;
    // Limit visual correction to a modest foot-contact adjustment. The old
    // 0.8m root lift (including hands and prop contacts) made bodies jump.
    const auto lift = (std::clamp)(idleFloor - poseFloor, -0.05F, 0.20F);
    if (!std::isfinite(lift) || std::abs(lift) < 0.003F) return;
    for (std::uint32_t b = 0; b < bones; ++b) {
        if (share.parents[b] >= 0) continue;
        const auto value = pose[b][1] + lift;
        static_cast<void>(crash_guard::TryWrite(
            reinterpret_cast<void*>(transforms + b * 0x30U + 4U), &value, sizeof(value)));
    }
    share.lifted.fetch_add(1U, std::memory_order_relaxed);
}

// --- Posture rebuild ---------------------------------------------------------
// Quaternions are x,y,z,w (hkQsTransform), composed parent * local.
using Vec3 = std::array<float, 3>;
using Quat = std::array<float, 4>;
Vec3 Add(const Vec3& a, const Vec3& b) noexcept { return {a[0] + b[0], a[1] + b[1], a[2] + b[2]}; }
Vec3 Sub(const Vec3& a, const Vec3& b) noexcept { return {a[0] - b[0], a[1] - b[1], a[2] - b[2]}; }
Vec3 Scale(const Vec3& a, const float s) noexcept { return {a[0] * s, a[1] * s, a[2] * s}; }
float Dot(const Vec3& a, const Vec3& b) noexcept { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
float Length(const Vec3& a) noexcept { return std::sqrt(Dot(a, a)); }
Vec3 Cross(const Vec3& a, const Vec3& b) noexcept {
    return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
}
Vec3 Lerp(const Vec3& a, const Vec3& b, const float t) noexcept { return Add(a, Scale(Sub(b, a), t)); }
Quat Mul(const Quat& a, const Quat& b) noexcept {
    return {a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1],
            a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0],
            a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3],
            a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2]};
}
Quat Conjugate(const Quat& q) noexcept { return {-q[0], -q[1], -q[2], q[3]}; }
Vec3 Rotate(const Quat& q, const Vec3& v) noexcept {
    const Vec3 u{q[0], q[1], q[2]};
    const auto t = Scale(Cross(u, v), 2.0F);
    return Add(Add(v, Scale(t, q[3])), Cross(u, t));
}
Quat AxisAngle(const Vec3& axis, const float angle) noexcept {
    const auto length = Length(axis);
    if (!(length > 1.0e-6F) || !std::isfinite(angle)) return {0.0F, 0.0F, 0.0F, 1.0F};
    const auto s = std::sin(angle * 0.5F) / length;
    return {axis[0] * s, axis[1] * s, axis[2] * s, std::cos(angle * 0.5F)};
}
float SafeAcos(const float value) noexcept { return std::acos((std::clamp)(value, -1.0F, 1.0F)); }
// Shortest rotation taking direction a onto b, limited to maxAngle radians and
// to the given share of its angle.
Quat Swing(const Vec3& a, const Vec3& b, const float share, const float maxAngle) noexcept {
    const auto la = Length(a), lb = Length(b);
    if (!(la > 1.0e-4F) || !(lb > 1.0e-4F)) return {0.0F, 0.0F, 0.0F, 1.0F};
    const auto angle = SafeAcos(Dot(a, b) / (la * lb));
    return AxisAngle(Cross(a, b), (std::min)(angle * share, maxAngle));
}
Vec3 ToBodyFrame(const Vec3& p, const float yaw) noexcept {
    const auto c = std::cos(yaw), s = std::sin(yaw);
    return {p[0] * c - p[2] * s, p[1], p[2] * c + p[0] * s};
}
Vec3 FromBodyFrame(const Vec3& p, const float yaw) noexcept {
    const auto c = std::cos(yaw), s = std::sin(yaw);
    return {p[0] * c + p[2] * s, p[1], p[2] * c - p[0] * s};
}
// Heading of the standing feet (heel -> toe) in model space.
bool FeetYaw(const Vec3* at, const std::uint32_t lf, const std::uint32_t lt, const std::uint32_t rf,
    const std::uint32_t rt, float& yaw) noexcept {
    const auto x = at[lt][0] - at[lf][0] + at[rt][0] - at[rf][0];
    const auto z = at[lt][2] - at[lf][2] + at[rt][2] - at[rf][2];
    if (!(x * x + z * z > 0.0025F)) return false;
    yaw = std::atan2(x, z);
    return std::isfinite(yaw);
}

// Owner side, animation worker: the local first-person final pose gives the
// standing frame between clips, and the body effectors during a clip.
void CaptureOwnerPose(const std::uintptr_t controller, void* pose) noexcept {
    auto& owner = gOwnerPose;
    std::uint32_t bones{};
    if (!LayerBones(controller - 0x3F0U, 7U, bones) || bones != kFirstPersonBones) return;
    const std::lock_guard lock(owner.mutex);
    if (owner.skeleton == nullptr) owner.skeleton = new (std::nothrow) MaskShare{};
    if (owner.skeleton == nullptr) return;
    if (owner.checked != controller) {
        owner.checked = controller;
        ReadParents(controller, *owner.skeleton);
        owner.firstPerson = owner.skeleton->parentCount == kFirstPersonBones &&
            gFirstPersonParents.size() == kFirstPersonBones &&
            std::equal(gFirstPersonParents.begin(), gFirstPersonParents.end(), owner.skeleton->parents.begin());
        owner.calibrated = owner.fresh = false;
    }
    // Raw local array (hkaPose +4), not the synced accessor: syncing the
    // local player's pose here, mid-composition, must not alter what it renders.
    std::uintptr_t transforms{};
    if (!owner.firstPerson || !Read(reinterpret_cast<std::uintptr_t>(pose) + 4U, transforms) || transforms < 0x10000U) return;
    std::array<LocalBone, kFirstPersonBones> local{};
    std::array<Vec3, kFirstPersonBones> at{};
    std::array<Quat, kFirstPersonBones> rotation{};
    if (!crash_guard::TryCopy(local.data(), reinterpret_cast<const void*>(transforms), sizeof(local)) ||
        !ModelPositions(local.data(), owner.skeleton->parents.data(), kFirstPersonBones, at.data(), rotation.data())) return;
    if (!owner.clip.load(std::memory_order_acquire)) {
        float yaw{};
        if (!FeetYaw(at.data(), kFpLeftFoot, kFpLeftToe, kFpRightFoot, kFpRightToe, yaw)) return;
        owner.yaw = yaw;
        owner.idleHips = ToBodyFrame(at[kFpHips], yaw);
        owner.idleHead = ToBodyFrame(at[kFpHead], yaw);
        owner.calibrated = true; owner.fresh = false;
        return;
    }
    if (!owner.calibrated) return;
    const auto hips = Sub(ToBodyFrame(at[kFpHips], owner.yaw), owner.idleHips);
    const auto head = Sub(ToBodyFrame(at[kFpHead], owner.yaw), owner.idleHead);
    const auto left = ToBodyFrame(at[kFpLeftHand], owner.yaw), right = ToBodyFrame(at[kFpRightHand], owner.yaw);
    std::array<float, 12> effectors{hips[0], hips[1], hips[2], head[0], head[1], head[2],
        left[0], left[1], left[2], right[0], right[1], right[2]};
    for (const auto value : effectors) if (!std::isfinite(value) || std::abs(value) > 10.0F) return;
    owner.effectors = effectors; owner.at = GetTickCount64(); owner.fresh = true;
    owner.frames.fetch_add(1U, std::memory_order_relaxed);
}

// Observer side, animation worker, after the mask: rebuild the owner's body on
// the crew skeleton. Hips and head move as the owner's moved from its pose
// before the clip (crouch, lean), feet stay planted, the spine bends to carry
// the head, and both arms reach where the owner's hands really are. Each stage
// is two-bone IK or a distributed swing; fingers and wrists keep the clip.
void SolvePose(MaskShare& share, const Cycle& current, const std::uintptr_t transforms, const std::uint32_t bones) noexcept {
    const auto& track = share.track;
    const auto skip = [&](const std::uint32_t reason) noexcept { share.poseSkip.store(reason, std::memory_order_relaxed); };
    if (track.count == 0U || track.transition == 0U) return skip(1U);
    if (track.transition != share.playing) return skip(2U);
    if (current.graph == 0U || share.parentCount != bones || share.baseBones.load(std::memory_order_acquire) != bones)
        return skip(3U);
    for (const auto bone : kPoseBones)
        if (share.crewPose[bone] < 0 || static_cast<std::uint32_t>(share.crewPose[bone]) >= bones) return skip(4U);
    float time{}, duration{};
    if (!timeline::Read(current.graph, time, duration) || !std::isfinite(time)) return skip(5U);
    // The owner's effectors at our clip time (held at the ends of the track).
    const auto* samples = track.samples.data();
    const auto count = track.count;
    std::array<float, 12> e = samples[count - 1U].effectors;
    if (time <= samples[0].time) e = samples[0].effectors;
    else for (std::uint32_t i = 1; i < count; ++i) {
        if (time > samples[i].time) continue;
        const auto& a = samples[i - 1U];
        const auto& b = samples[i];
        const auto span = b.time - a.time;
        const auto t = span > 1.0e-5F ? (time - a.time) / span : 1.0F;
        for (std::size_t k = 0; k < e.size(); ++k) e[k] = a.effectors[k] + (b.effectors[k] - a.effectors[k]) * t;
        break;
    }
    // Ease the rebuilt posture in and out with the clip's own blend.
    auto weight = (std::clamp)(time / 0.25F, 0.0F, 1.0F);
    if (duration > 0.5F) weight = (std::min)(weight, (std::clamp)((duration - time) / 0.25F, 0.0F, 1.0F));
    if (!(weight > 0.001F)) return skip(6U);
    struct Scratch final {
        std::array<LocalBone, kMaskBones> local, idle;
        std::array<Vec3, kMaskBones> at, idleAt;
        std::array<Quat, kMaskBones> rotation, idleRotation;
    };
    thread_local auto* scratch = new (std::nothrow) Scratch{};
    if (scratch == nullptr) return skip(7U);
    auto& [local, idle, at, idleAt, rotation, idleRotation] = *scratch;
    const auto* parents = share.parents.data();
    if (!crash_guard::TryCopy(local.data(), reinterpret_cast<const void*>(transforms), bones * sizeof(LocalBone))) return skip(7U);
    for (std::uint32_t b = 0; b < bones; ++b) std::memcpy(idle[b].data(), share.base[b].data(), sizeof(LocalBone));
    const auto solve = [&]() noexcept { return ModelPositions(local.data(), parents, bones, at.data(), rotation.data()); };
    if (!ModelPositions(idle.data(), parents, bones, idleAt.data(), idleRotation.data()) || !solve()) return skip(9U);
    const auto crew = [&](const FirstPersonBone bone) noexcept { return static_cast<std::uint32_t>(share.crewPose[bone]); };
    float yaw{};
    if (!FeetYaw(idleAt.data(), crew(kFpLeftFoot), crew(kFpLeftToe), crew(kFpRightFoot), crew(kFpRightToe), yaw)) return skip(8U);
    const auto setRotation = [&](const std::uint32_t b, const Quat& model) noexcept {
        const auto p = parents[b];
        const auto q = p < 0 ? model : Mul(Conjugate(rotation[static_cast<std::uint32_t>(p)]), model);
        local[b][4] = q[0]; local[b][5] = q[1]; local[b][6] = q[2]; local[b][7] = q[3];
    };
    const auto setPosition = [&](const std::uint32_t b, const Vec3& model) noexcept {
        const auto p = parents[b];
        const auto t = p < 0 ? model : Rotate(Conjugate(rotation[static_cast<std::uint32_t>(p)]),
            Sub(model, at[static_cast<std::uint32_t>(p)]));
        local[b][0] = t[0]; local[b][1] = t[1]; local[b][2] = t[2];
    };
    const auto localRotation = [&](const std::uint32_t b) noexcept { return Quat{local[b][4], local[b][5], local[b][6], local[b][7]}; };
    const auto setLocalRotation = [&](const std::uint32_t b, const Quat& q) noexcept {
        local[b][4] = q[0]; local[b][5] = q[1]; local[b][6] = q[2]; local[b][7] = q[3];
    };
    // Two-bone IK (a: upper limb, b: middle joint, c: end; Holden's analytic
    // form). The bend plane is the current one, else the idle one.
    const auto reach = [&](const std::uint32_t a, const std::uint32_t b, const std::uint32_t c, const Vec3& target) noexcept {
        const auto A = at[a], B = at[b], C = at[c];
        const auto lab = Length(Sub(B, A)), lcb = Length(Sub(C, B)), ltarget = Length(Sub(target, A));
        if (!(lab > 0.01F) || !(lcb > 0.01F) || !(ltarget > 0.01F) || !(Length(Sub(C, A)) > 0.01F)) return false;
        const auto lat = (std::clamp)(ltarget, std::abs(lab - lcb) + 0.005F, lab + lcb - 0.005F);
        const auto ac = Scale(Sub(C, A), 1.0F / Length(Sub(C, A)));
        const auto ab = Scale(Sub(B, A), 1.0F / lab);
        const auto bc = Scale(Sub(C, B), 1.0F / lcb);
        const auto toward = Scale(Sub(target, A), 1.0F / ltarget);
        const auto acab0 = SafeAcos(Dot(ac, ab)), babc0 = SafeAcos(Dot(Scale(ab, -1.0F), bc));
        const auto acat0 = SafeAcos(Dot(ac, toward));
        const auto acab1 = SafeAcos((lcb * lcb - lab * lab - lat * lat) / (-2.0F * lab * lat));
        const auto babc1 = SafeAcos((lat * lat - lab * lab - lcb * lcb) / (-2.0F * lab * lcb));
        auto bend = Cross(ac, ab);
        if (!(Length(bend) > 1.0e-3F)) bend = Cross(Sub(idleAt[c], idleAt[a]), Sub(idleAt[b], idleAt[a]));
        if (!(Length(bend) > 1.0e-6F)) return false;
        const auto aInverse = Conjugate(rotation[a]);
        const auto r0 = AxisAngle(Rotate(aInverse, bend), acab1 - acab0);
        const auto r1 = AxisAngle(Rotate(Conjugate(rotation[b]), bend), babc1 - babc0);
        const auto turn = Cross(ac, toward);
        const auto r2 = Length(turn) > 1.0e-6F ? AxisAngle(Rotate(aInverse, turn), acat0) : Quat{0.0F, 0.0F, 0.0F, 1.0F};
        setLocalRotation(a, Mul(localRotation(a), Mul(r0, r2)));
        setLocalRotation(b, Mul(localRotation(b), r1));
        return true;
    };
    const auto hips = crew(kFpHips), head = crew(kFpHead);
    const std::array<std::uint32_t, 2> hands{crew(kFpLeftHand), crew(kFpRightHand)};
    const std::array<std::array<std::uint32_t, 3>, 2> arms{{{crew(kFpLeftArm), crew(kFpLeftForearm), hands[0]},
        {crew(kFpRightArm), crew(kFpRightForearm), hands[1]}}};
    const std::array<std::array<std::uint32_t, 3>, 2> legs{{{crew(kFpLeftThigh), crew(kFpLeftCalf), crew(kFpLeftFoot)},
        {crew(kFpRightThigh), crew(kFpRightCalf), crew(kFpRightFoot)}}};
    const auto headRotation = rotation[head];
    const std::array<Quat, 2> handRotation{rotation[hands[0]], rotation[hands[1]]};
    std::array<Vec3, 2> feet{at[legs[0][2]], at[legs[1][2]]};
    const std::array<Quat, 2> footRotation{rotation[legs[0][2]], rotation[legs[1][2]]};
    // The floor: the lowest foot or toe of the representation's own idle pose.
    // The legs: hip-to-foot reach of the idle pose (thigh + calf).
    float floor{std::numeric_limits<float>::max()}, leg{std::numeric_limits<float>::max()};
    for (const auto bone : {kFpLeftFoot, kFpLeftToe, kFpRightFoot, kFpRightToe}) floor = (std::min)(floor, idleAt[crew(bone)][1]);
    for (const auto& [thigh, calf, foot] : legs)
        leg = (std::min)(leg, Length(Sub(idleAt[calf], idleAt[thigh])) + Length(Sub(idleAt[foot], idleAt[calf])));
    if (!(leg > 0.2F) || !std::isfinite(floor)) return skip(8U);
    // 1. Hips (crouch/lean), feet kept where the clip put them. A first-person
    // crouch or vent entry drops the owner's camera rig far more than a body
    // can: never lower the hips below a deep crouch over the higher foot, nor
    // push them sideways past what the planted legs reach.
    auto hipsShift = FromBodyFrame({e[0], e[1], e[2]}, yaw);
    const auto sideways = std::sqrt(hipsShift[0] * hipsShift[0] + hipsShift[2] * hipsShift[2]);
    if (sideways > 0.3F) { hipsShift[0] *= 0.3F / sideways; hipsShift[2] *= 0.3F / sideways; }
    auto hipsTarget = Lerp(at[hips], Add(idleAt[hips], hipsShift), weight);
    hipsTarget[1] = (std::max)(hipsTarget[1], (std::min)(at[hips][1], (std::max)(feet[0][1], feet[1][1]) + 0.5F * leg));
    hipsTarget[1] = (std::min)(hipsTarget[1], (std::max)(at[hips][1], idleAt[hips][1]) + 0.1F);
    setPosition(hips, hipsTarget);
    if (!solve()) return;
    for (std::size_t side = 0; side < 2U; ++side) {
        const auto& [thigh, calf, foot] = legs[side];
        if (reach(thigh, calf, foot, feet[side]) && solve()) setRotation(foot, footRotation[side]);
        if (!solve()) return;
    }
    // 2. Spine: bend the joints between hips and head toward the owner's head.
    std::array<std::uint32_t, 8> spine{};
    std::size_t joints{};
    auto joint = parents[head];
    while (joint >= 0 && static_cast<std::uint32_t>(joint) != hips && joints < spine.size()) {
        spine[joints++] = static_cast<std::uint32_t>(joint); joint = parents[static_cast<std::uint32_t>(joint)];
    }
    if (joint >= 0 && static_cast<std::uint32_t>(joint) == hips && joints != 0U) {
        const auto headShift = FromBodyFrame({e[3], e[4], e[5]}, yaw);
        const auto target = Lerp(at[head], Add(idleAt[head], headShift), weight);
        for (auto k = joints; k-- > 0U;) {
            const auto j = spine[k];
            const auto swing = Swing(Sub(at[head], at[j]), Sub(target, at[j]), 1.0F / static_cast<float>(k + 1U), 0.6F);
            setRotation(j, Mul(swing, rotation[j]));
            if (!solve()) return;
        }
    }
    setRotation(head, headRotation);
    if (!solve()) return;
    // 3. Arms: hands where the owner's hands are; wrists keep the clip.
    for (std::size_t side = 0; side < 2U; ++side) {
        const auto& [arm, forearm, hand] = arms[side];
        const auto target = FromBodyFrame({e[6U + side * 3U], e[7U + side * 3U], e[8U + side * 3U]}, yaw);
        if (reach(arm, forearm, hand, Lerp(at[hand], target, weight)) && solve()) setRotation(hand, handRotation[side]);
        if (!solve()) return;
    }
    // The root is grounded once, after the posture solve, using feet only.
    // Hand/finger contact with a vent or terminal must not lift the body.
    for (std::uint32_t b = 0; b < bones; ++b)
        for (std::size_t i = 0; i < 8U; ++i) if (!std::isfinite(local[b][i])) return skip(9U);
    for (std::uint32_t b = 0; b < bones; ++b)
        static_cast<void>(crash_guard::TryWrite(reinterpret_cast<void*>(transforms + b * 0x30U), local[b].data(), 0x20U));
    share.solved.fetch_add(1U, std::memory_order_relaxed);
    skip(0U);
}

void ApplyMask(MaskShare& share, const std::uintptr_t address, void* pose, const std::uint32_t bones) noexcept {
    const auto state = share.state.load(std::memory_order_acquire);
    if (state == kMaskOff) return;
    Cycle current{};
    if (!ReadCycle(address - 0x3F0U, current)) return;
    if (state == kMaskApply && !Owned(current, share.expected)) return;
    if (state == kMaskCapture && current.state != 1U && current.state != 3U && current.state != 0x38U) return;
    auto transforms = LocalPoseTransforms(pose, false);
    if (transforms < 0x10000U) return;
    if (state == kMaskCapture) {
        std::uint32_t b = 0;
        for (; b < bones; ++b) if (!Read(transforms + b * 0x30U, share.base[b])) break;
        share.baseBones.store(b == bones ? bones : 0U, std::memory_order_release);
        share.captured.fetch_add(1U, std::memory_order_relaxed);
    } else if (share.baseBones.load(std::memory_order_acquire) == bones) {
        // Invalidate the MODEL cache through the engine before restoring
        // locals, so skinning, IK and attachments all consume the same pose.
        transforms = LocalPoseTransforms(pose, true);
        if (transforms < 0x10000U) return;
        std::uint32_t restored{};
        for (std::uint32_t b = 0; b < bones; ++b) {
            const auto bytes = !share.driven.test(b) ? 0x30U : share.displaced.test(b) ? 0U : 0x10U;
            if (bytes != 0U && crash_guard::TryWrite(reinterpret_cast<void*>(transforms + b * 0x30U), share.base[b].data(), bytes) &&
                bytes == 0x30U)
                ++restored;
        }
        share.restored.store(restored, std::memory_order_relaxed);
        SolvePose(share, current, transforms, bones);
        GroundFinalPose(transforms, share, bones);
        share.applied.fetch_add(1U, std::memory_order_relaxed);
    }
}
// Rig-space floor support for native vent entry. The 535 diagnostic showed
// zero native-root Y delta despite visible sinking, so use actual high-res
// feet/toe model positions rather than guessing an offset on the skeleton root.
// This function only measures/returns a bounded visual lift; it never edits
// character world position, physics, or individual limb transforms.
float VentVisualFloorLift(MaskShare& share, const std::uintptr_t transforms,
    const std::uint32_t bones) noexcept {
    bool valid = share.ventMapped && share.parentCount == bones &&
        share.baseBones.load(std::memory_order_acquire) == bones;
    for (const auto fp : {kFpHips, kFpLeftFoot, kFpLeftToe, kFpRightFoot, kFpRightToe}) {
        const auto mapped = share.crewPose[fp];
        valid = valid && mapped >= 0 && static_cast<std::uint32_t>(mapped) < bones;
    }
    if (!valid) { ++share.ventGroundSkipped; return 0.0F; }
    std::array<LocalBone, kMaskBones> current{}, idle{};
    std::array<Vec3, kMaskBones> currentAt{}, idleAt{};
    std::array<Quat, kMaskBones> currentRot{}, idleRot{};
    for (std::uint32_t b = 0; b < bones; ++b) {
        if (!Read(transforms + b * 0x30U, current[b])) {
            ++share.ventGroundSkipped; return 0.0F;
        }
        std::memcpy(idle[b].data(), share.base[b].data(), 0x30U);
    }
    if (!ModelPositions(current.data(), share.parents.data(), bones,
            currentAt.data(), currentRot.data()) ||
        !ModelPositions(idle.data(), share.parents.data(), bones,
            idleAt.data(), idleRot.data())) {
        ++share.ventGroundSkipped; return 0.0F;
    }
    float currentFoot = std::numeric_limits<float>::max();
    float idleFoot = std::numeric_limits<float>::max();
    for (const auto fp : {kFpLeftFoot, kFpLeftToe, kFpRightFoot, kFpRightToe}) {
        const auto bone = static_cast<std::uint32_t>(share.crewPose[fp]);
        currentFoot = (std::min)(currentFoot, currentAt[bone][1]);
        idleFoot = (std::min)(idleFoot, idleAt[bone][1]);
    }
    const auto hip = static_cast<std::uint32_t>(share.crewPose[kFpHips]);
    const float hipDrop = idleAt[hip][1] - currentAt[hip][1];
    float rootShift{};
    for (std::uint32_t b = 0; b < bones; ++b)
        if (share.parents[b] < 0) {
            const auto referenceY = share.rootReferenceReady
                ? share.rootReference[b][1]
                : share.baseBones.load(std::memory_order_acquire) == bones
                    ? std::bit_cast<float>(share.base[b][1]) : current[b][1];
            rootShift = referenceY - current[b][1];
            break;
        }
    if (!std::isfinite(currentFoot) || !std::isfinite(idleFoot) ||
        !std::isfinite(hipDrop) || !std::isfinite(rootShift)) {
        ++share.ventGroundSkipped; return 0.0F;
    }
    share.ventLastFootY = currentFoot;
    share.ventIdleFootY = idleFoot;
    share.ventLastHipDeltaY = hipDrop;
    share.ventMaxHipDrop = (std::max)(share.ventMaxHipDrop, hipDrop);
    const auto depth = VentFootDepth(idleFoot, currentFoot, rootShift,
        share.ventRise, share.ventProgress);
    share.ventMaxDepthBeforeGround = (std::max)(share.ventMaxDepthBeforeGround, depth);
    const auto lift = VentGroundLift(idleFoot, currentFoot, rootShift,
        share.ventRise, share.ventProgress, share.ventGroundLift);
    share.ventGroundLift = lift;
    share.ventMaxGroundLift = (std::max)(share.ventMaxGroundLift, lift);
    ++share.ventGroundMapped;
    if (lift > 0.005F) ++share.ventGroundFrames;
    return lift;
}

// Render-only root displacement (Character model space: Y up, heading 0 = +Z):
// skeleton roots are turned by `yaw` about the Character origin, then moved by
// `offset`. The Character entity itself does not move.
void ApplyVisualOffset(MaskShare& share, void* pose, const std::uint32_t bones) noexcept {
    if (!share.offsetActive.load(std::memory_order_acquire) || share.parentCount != bones) return;
    const auto transforms = LocalPoseTransforms(pose, true);
    if (transforms < 0x10000U) return;
    const auto [ox, oy, oz, yaw] = share.offset;
    const std::array<float, 3> localOffset{ox, oy, oz};
    const float ventLift = share.stableRoot
        ? VentVisualFloorLift(share, transforms, bones) : 0.0F;
    const std::array<float, 4> turn{0.0F, std::sin(yaw * 0.5F), 0.0F, std::cos(yaw * 0.5F)};
    const bool firstStableFrame = share.stableRoot && !share.rootReferenceReady;
    const bool idleAvailable = share.baseBones.load(std::memory_order_acquire) == bones;
    bool wrote = false;
    for (std::uint32_t b = 0; b < bones; ++b) {
        if (share.parents[b] >= 0) continue;
        LocalBone root{};
        if (!crash_guard::TryCopy(root.data(), reinterpret_cast<const void*>(transforms + b * 0x30U), 0x20U)) continue;
        const std::array<float, 3> nativeRoot{root[0], root[1], root[2]};
        if (firstStableFrame) {
            // Prefer the captured retail idle pose. If the proxy had no idle
            // sample before streaming, freeze the first rendered root instead.
            bool fromIdle = idleAvailable;
            if (fromIdle) {
                for (std::size_t axis = 0; axis < 3U; ++axis) {
                    const auto candidate = std::bit_cast<float>(share.base[b][axis]);
                    if (!std::isfinite(candidate)) { fromIdle = false; break; }
                    share.rootReference[b][axis] = candidate;
                }
            }
            if (!fromIdle) share.rootReference[b] = nativeRoot;
            if (fromIdle) ++share.idleReferences;
            else ++share.liveReferences;
        }
        const auto target = VisualRootPosition(nativeRoot, share.rootReference[b],
            localOffset, yaw, share.stableRoot);
        root[0] = target[0]; root[1] = target[1] + ventLift; root[2] = target[2];
        const std::array<float, 4> q{root[4], root[5], root[6], root[7]};
        root[4] = turn[3] * q[0] + turn[1] * q[2];
        root[5] = turn[3] * q[1] + turn[1] * q[3];
        root[6] = turn[3] * q[2] - turn[1] * q[0];
        root[7] = turn[3] * q[3] - turn[1] * q[1];
        bool finite = true;
        for (std::size_t i = 0; i < 8U; ++i) finite = finite && std::isfinite(root[i]);
        if (finite && crash_guard::TryWrite(
                reinterpret_cast<void*>(transforms + b * 0x30U), root.data(), 0x20U)) {
            wrote = true;
            if (share.stableRoot) {
                const float rawDifference = std::abs(nativeRoot[1] - share.rootReference[b][1]);
                if (std::isfinite(rawDifference))
                    share.maxSuppressedRootDeltaY = (std::max)(share.maxSuppressedRootDeltaY, rawDifference);
            }
        }
    }
    if (wrote) {
        if (share.stableRoot) {
            share.rootReferenceReady = true;
            ++share.stableFrames;
        }
        share.offsetFrames.fetch_add(1U, std::memory_order_relaxed);
    }
}

void __fastcall LayerBlendDetour(void* controller, void*, const std::uint32_t from, const std::uint32_t to, void* pose) noexcept {
    const auto original = reinterpret_cast<LayerBlend>(gBase.load(std::memory_order_relaxed) + 0x2A25A0U);
    original(controller, from, to, pose);
    // Layers 0..7 are composed in order (0>1 ... 6>7); layer 7 then holds the
    // final high-res pose. A bone no layer weights falls back to the
    // skeleton reference pose (0x2983D0): the T-posed arm. Fix the final pose.
    if (to != 7U || !gMaskHook.load(std::memory_order_acquire) || pose == nullptr) return;
    const auto address = reinterpret_cast<std::uintptr_t>(controller);
    if (address == gOwnerPose.controller.load(std::memory_order_acquire)) { CaptureOwnerPose(address, pose); return; }
    for (auto& share : gMaskShares) {
        if (share.controller.load(std::memory_order_acquire) != address) continue;
        const std::lock_guard lock(share.mutex);
        if (share.controller.load() != address) continue;
        std::uint32_t bones{};
        if (!LayerBones(address - 0x3F0U, 7U, bones) || bones > kMaskBones) return;
        ApplyMask(share, address, pose, bones);
        ApplyVisualOffset(share, pose, bones);
        return;
    }
}

bool InstallMaskHook(HookManager& hooks, const std::uintptr_t moduleBase, std::string& error) noexcept {
    // CALL 0x2A25A0 at 0x2A5026 (push ecx/ebx/eax; mov ecx,esi), thiscall,
    // three stack arguments, RET 0Ch at 0x2A2C7E.
    std::array<std::uint8_t, 3> ret{};
    if (!hooks.ExactBuildValidated() ||
        !Match(0x7BAF10U, {0x56,0x8B,0xF1,0xE8,0x58,0xF5,0xFF,0xFF,0x8D,0x46,0x04,0x5E,0xC3}) ||
        !Match(0x7B9B60U, {0x8B,0x01,0x8B,0x50,0x1C,0x33,0xC0,0x85,0xD2}) ||
        !crash_guard::TryCopy(ret.data(), reinterpret_cast<const void*>(moduleBase + 0x2A2C7EU), ret.size()) ||
        ret != std::array<std::uint8_t, 3>{0xC2, 0x0C, 0x00} ||
        !Match(0x2A5021U, {0x51, 0x53, 0x50, 0x8B, 0xCE}) ||
        !Match(0x2A25A0U, {0x6A, 0xFF, 0x68})) {
        error = "AnimationSync layer blend: certified call site/ABI mismatch"; return false;
    }
    CallRedirectRequest request;
    request.name = "animationsync_layer_blend";
    request.callSite = reinterpret_cast<void*>(moduleBase + 0x2A5026U);
    request.expectedTarget = reinterpret_cast<void*>(moduleBase + 0x2A25A0U);
    request.detour = reinterpret_cast<void*>(&LayerBlendDetour);
    if (!hooks.InstallCallRedirect(request, error)) return false;
    gMaskHook.store(true, std::memory_order_release);
    return true;
}

// Engine thread: lifecycle reconciliation. Admission also publishes BEFORE
// constructing the clip, freezing the last idle before its first evaluation.
void ApplyMasksOnEngineThread() noexcept {
    const std::lock_guard lock(gMutex);
    for (std::size_t i = 0; i < gSlots.size(); ++i) {
        auto& slot = gSlots[i];
        auto& share = gMaskShares[i];
        PublishMaskState(slot, share);
    }
}

void Export(Diagnostics& diagnostics, const std::uintptr_t remoteCharacter) {
    LocalDiagnostics local{};
    LocalSnapshot snapshot{};
    { const std::lock_guard lock(gLocalMutex); local = gLocalDiagnostics; snapshot = gLocalSnapshot; }
    RemoteDiagnostics remote{};
    struct { std::uint64_t restored{}, applied{}, captured{}, solved{}, skip{}, track{}, lifted{}; } share{};
    bool playing{};
    std::uint32_t slots{};
    {
        const std::lock_guard lock(gMutex);
        for (const auto& slot : gSlots) slots += slot.character != 0U ? 1U : 0U;
        if (const auto* slot = Find(remoteCharacter)) {
            remote = slot->diagnostics; playing = slot->owned.address != 0U;
            const auto& found = gMaskShares[static_cast<std::size_t>(slot - gSlots.data())];
            share = {found.restored.load(), found.applied.load(), found.captured.load(), found.solved.load(),
                found.poseSkip.load(), found.track.count, found.lifted.load()};
        }
    }
    // transition:alias[start..end xloops]=result@ours/owner/duration (s)
    std::ostringstream endings;
    endings.imbue(std::locale::classic());
    endings << std::fixed << std::setprecision(2);
    for (std::size_t i = 0; i < remote.endings.size(); ++i) {
        const auto& row = remote.endings[(remote.endingNext + i) % remote.endings.size()];
        if (row.result == nullptr) continue;
        endings << row.transition << ':' << row.animation << '[' << row.start << ".." << row.end << " x" << row.loops
                << "]=" << row.result << '@' << row.ours << '/' << row.owner << '/' << row.duration << ' ';
    }
    // The prior build recorded total distance but not its direction. These
    // points distinguish a root that advances in the wrong world direction
    // from one that only shifts the rendered mesh inside a stationary actor.
    std::ostringstream spatialText;
    spatialText.imbue(std::locale::classic());
    spatialText << std::fixed << std::setprecision(2);
    const auto available = (std::min)(static_cast<std::size_t>(remote.spatialNext),
        remote.spatial.size());
    for (std::size_t i = 0; i < available; ++i) {
        const auto& point = remote.spatial[
            (static_cast<std::size_t>(remote.spatialNext) + remote.spatial.size() - available + i)
                % remote.spatial.size()];
        if (point.tickMs == 0U) continue;
        spatialText << point.tickMs << '#' << point.transition << ':' << point.clipPath
            << '@' << point.phase << " c(" << point.character[0] << ','
            << point.character[1] << ',' << point.character[2] << ')';
        if (point.hasOwner)
            spatialText << " o(" << point.owner[0] << ',' << point.owner[1]
                << ',' << point.owner[2] << ')';
        else spatialText << " o(-)";
        spatialText << " h(" << point.characterYaw << ','
            << (point.hasOwner ? point.ownerYaw : 0.0F) << ") ; ";
    }
    diagnostics.Log("info", "native_animation_sync_snapshot",
        "Generic scripted-animation replication: local THE_PLAYER capture and remote representation playback.",
        {{"localCaptured", Diagnostics::Unsigned(local.captured), true},
         {"localPoseFrames", Diagnostics::Unsigned(gOwnerPose.frames.load()), true},
         {"localPosed", Diagnostics::Unsigned(local.posed), true},
         {"localCleared", Diagnostics::Unsigned(local.cleared), true},
         {"localHidingSkipped", Diagnostics::Unsigned(local.hidingSkipped), true},
         {"localHidingFallbackCaptured", Diagnostics::Unsigned(local.fallbackCaptured), true},
         {"localTorn", Diagnostics::Unsigned(local.torn), true},
         {"localUnavailable", Diagnostics::Unsigned(local.unavailable), true},
         {"localAnimation", Diagnostics::Unsigned(local.animation), true},
         {"localResource", Diagnostics::Unsigned(local.resource), true},
         {"localActive", Diagnostics::Boolean(snapshot.valid && Active(snapshot.state)), true},
         {"remoteSlots", Diagnostics::Unsigned(slots), true},
         {"stage", remote.stage, false},
         {"transition", Diagnostics::Unsigned(remote.transition), true},
         {"animation", Diagnostics::Unsigned(remote.animation), true},
         {"resource", Diagnostics::Unsigned(remote.resource), true},
         {"clipPath", Diagnostics::Unsigned(remote.clipPath), true},
         {"clipSource", remote.clipSource, false},
         {"playing", Diagnostics::Boolean(playing), true},
         {"applied", Diagnostics::Unsigned(remote.applied), true},
         {"failed", Diagnostics::Unsigned(remote.failed), true},
         {"stale", Diagnostics::Unsigned(remote.stale), true},
         {"clipUnavailable", Diagnostics::Unsigned(remote.unavailable), true},
         {"interrupted", Diagnostics::Unsigned(remote.interrupted), true},
         {"completed", Diagnostics::Unsigned(remote.completed), true},
         {"resynced", Diagnostics::Unsigned(remote.resynced), true},
         {"readmitted", Diagnostics::Unsigned(remote.readmitted), true},
         {"nativeRootSamples", Diagnostics::Unsigned(remote.nativeRootSamples), true},
         {"nativeRootDistanceMeters", std::to_string(remote.nativeRootDistance), true},
         {"nativeRootDiscontinuities", Diagnostics::Unsigned(remote.nativeRootDiscontinuities), true},
         {"nativeOwnerGapMeters", std::to_string(remote.nativeOwnerGap), true},
         {"nativeOwnerMaxGapMeters", std::to_string(remote.nativeOwnerGapMax), true},
         {"nativeAnchorDriftMeters", std::to_string(remote.nativeAnchorDrift), true},
         {"nativeSpatialSampleCount", Diagnostics::Unsigned(remote.spatialNext), true},
         {"nativeSpatialTrace", spatialText.str(), false},
         {"endings", endings.str(), false},
         {"cancelled", Diagnostics::Unsigned(remote.cancelled), true},
         {"cancelDeferred", Diagnostics::Unsigned(remote.deferred), true},
         // bones/samples/changes per pose layer; a base layer still changing
         // "during" the clip proves a live idle/locomotion pose under it.
         {"layersBefore", LayerText(remote.before), false},
         {"layersDuring", LayerText(remote.during), false},
         {"layersIdleNow", LayerText(remote.idle), false},
         {"controllerFlags", Diagnostics::Unsigned(remote.controllerFlags), true},
         {"layerPoseMask", Diagnostics::Unsigned(remote.scriptedPose), true},
         {"maskFileClips", std::to_string(gMaskFile), true},
         {"maskStatus", Diagnostics::Unsigned(remote.maskStatus), true},
         {"maskSkeleton", Diagnostics::Unsigned(remote.maskSkeleton), true},
         {"maskTracks", Diagnostics::Unsigned(remote.maskTracks), true},
         {"maskMoving", Diagnostics::Unsigned(remote.maskMoving), true},
         {"maskHeld", Diagnostics::Unsigned(remote.maskHeld), true},
         {"maskTarget", Diagnostics::Unsigned(remote.maskTarget), true},
         {"maskCrewBones", Diagnostics::Unsigned(remote.maskCrewBones), true},
         {"maskCrewDriven", Diagnostics::Unsigned(remote.maskCrewDriven), true},
         {"maskHook", Diagnostics::Boolean(gMaskHook.load()), true},
         {"maskRestored", Diagnostics::Unsigned(share.restored), true},
         {"maskFrames", Diagnostics::Unsigned(share.applied), true},
         {"maskCaptures", Diagnostics::Unsigned(share.captured), true},
         {"poseSolved", Diagnostics::Unsigned(share.solved), true},
         {"poseSkip", Diagnostics::Unsigned(share.skip), true},
         {"poseTrack", Diagnostics::Unsigned(share.track), true},
         {"poseLifted", Diagnostics::Unsigned(share.lifted), true},
         {"maskNoBase", Diagnostics::Unsigned(remote.maskNoBase), true}});
    const auto vent = character_direct_step_runtime::VentVisualSnapshot();
    const auto visualRoot = VisualRootSnapshot(remoteCharacter);
    diagnostics.Log("info", "native_animation_vent_visual",
        "Native vent body follows owner visually; Character retains retail collision.",
        {{"frames", Diagnostics::Unsigned(vent.frames), true},
         {"rejected", Diagnostics::Unsigned(vent.rejected), true},
         {"relocations", Diagnostics::Unsigned(vent.relocations), true},
         {"relocationFailures", Diagnostics::Unsigned(vent.relocationFailures), true},
         {"maxOffsetMillimeters", Diagnostics::Unsigned(vent.maxOffsetMm), true},
         {"renderFramesApplied", Diagnostics::Unsigned(visualRoot.renderedFrames), true},
         {"stableRootFramesApplied", Diagnostics::Unsigned(visualRoot.stableFrames), true},
         {"stableRootIdleReferences", Diagnostics::Unsigned(visualRoot.idleReferences), true},
         {"stableRootLiveReferences", Diagnostics::Unsigned(visualRoot.liveReferences), true},
         {"maxSuppressedRootDeltaYMeters", std::to_string(visualRoot.maxSuppressedRootDeltaY), true},
         {"ventMappedFootSamples", Diagnostics::Unsigned(visualRoot.ventGroundMapped), true},
         {"ventFootProbeSkipped", Diagnostics::Unsigned(visualRoot.ventGroundSkipped), true},
         {"ventFloorSupportFrames", Diagnostics::Unsigned(visualRoot.ventGroundFrames), true},
         {"maxVisualFloorLiftMeters", std::to_string(visualRoot.ventMaxGroundLift), true},
         {"maxDetectedFootDepthMeters", std::to_string(visualRoot.ventMaxDepthBeforeGround), true},
         {"maxAnimatedHipDropMeters", std::to_string(visualRoot.ventMaxHipDrop), true},
         {"lastAnimatedFootY", std::to_string(visualRoot.ventLastFootY), true},
         {"idleFootReferenceY", std::to_string(visualRoot.ventIdleFootY), true}});
    decltype(gOwnerCycles) cycles{};
    decltype(gOwnerAliases) aliases{};
    { const std::lock_guard lock(gLocalMutex); cycles = gOwnerCycles; aliases = gOwnerAliases; }
    std::ostringstream cycleText, aliasText;
    cycleText.imbue(std::locale::classic()); aliasText.imbue(std::locale::classic());
    for (const auto& row : cycles) if (row.frames != 0U)
        cycleText << std::hex << row.slot << ':' << row.type << '/' << row.state << '/' << std::dec << row.animation
                  << 'x' << row.frames << ' ';
    for (const auto& row : aliases) if (row.count != 0U)
        aliasText << row.name << '>' << row.path << 'x' << row.count << '@' << row.lastTick << ' ';
    diagnostics.Log("info", "animation_sync_owner_observation",
        "Local player cycle kinds (slot:typeRva/state/clip x frames) and aliases its own clip database resolved (name>path x count @tick).",
        {{"ownerDatabase", Diagnostics::Unsigned(gOwnerDatabase.load()), true},
         {"cycles", cycleText.str(), false},
         {"aliases", aliasText.str(), false}});
}
}
