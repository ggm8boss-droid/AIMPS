#include "aimp/WorldEntityRegistry.h"

#include "aimp/CrashGuard.h"
#include "aimp/Diagnostics.h"
#include "aimp/LevelGenerationTracker.h"
#include "aimp/SignatureResolver.h"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <chrono>
#include <cmath>
#include <cstring>
#include <fstream>
#include <limits>
#include <unordered_set>
#include <vector>

namespace aimp {
namespace {

constexpr std::array<char, 8> kManifestMagic{'A','I','M','P','W','M','0','9'};
constexpr std::uint32_t kMaximumRuntimeEntities = 2'000'000U;
constexpr std::uint32_t kMaximumCompositePublicInputs = 4'000'000U;
constexpr std::uint32_t kMaximumCompositePublicInputCanonicalAnchors = 4'000'000U;
constexpr std::uint32_t kMaximumCompositePublicInputCausalProfiles = 4'000'000U;
constexpr std::size_t kSharedObjectOffset = 0x0CU;
constexpr std::size_t kManagerRootCollectionOffset = 0x28U;
constexpr std::size_t kManagerLoadedCollectionOffset = 0xACU;
constexpr std::size_t kNativeCollectionCountOffset = 0x08U;
constexpr std::size_t kNativeCollectionDataOffset = 0x0CU;
constexpr std::size_t kProviderCollectionPointerOffset = 0x00U;
constexpr std::size_t kProviderEntryStride = 0x08U;
constexpr std::size_t kProviderEntryKeyOffset = 0x04U;
constexpr std::uint32_t kControlResolverBudget = 16U;

using ResolveEntityFn = int* (__thiscall*)(
    void* manager,
    void** outputShared,
    const CathodeRuntimeKey* key);
using ExtractRuntimeKeyFn = void (__thiscall*)(
    const void* runtimeEntity,
    CathodeRuntimeKey* output);
using AssignEntityRefFn = int* (__thiscall*)(void* destination, void** source);

bool ManifestFileIdentity(HANDLE file, std::array<std::uint32_t, 7>& identity) noexcept {
    BY_HANDLE_FILE_INFORMATION info{};
    if (!GetFileInformationByHandle(file, &info)) return false;
    identity = {info.dwVolumeSerialNumber, info.nFileIndexHigh, info.nFileIndexLow,
        info.nFileSizeHigh, info.nFileSizeLow,
        info.ftLastWriteTime.dwHighDateTime, info.ftLastWriteTime.dwLowDateTime};
    return true;
}

bool ReadExact(std::ifstream& stream, void* destination, const std::size_t bytes) {
    stream.read(static_cast<char*>(destination), static_cast<std::streamsize>(bytes));
    return static_cast<std::size_t>(stream.gcount()) == bytes;
}

template <typename T>
bool ReadValue(std::ifstream& stream, T& value) {
    return ReadExact(stream, &value, sizeof(value));
}

std::uint64_t PackKey(const CathodeRuntimeKey& key) noexcept {
    return (static_cast<std::uint64_t>(key.entityB) << 32U) | key.entityA;
}

// Keep SEH in tiny POD-only helpers. MSVC rejects __try in functions that need
// C++ object unwinding (C2712).
bool InvokeResolveSafely(
    const ResolveEntityFn resolve,
    void* manager,
    void** outputShared,
    const CathodeRuntimeKey* key) noexcept {
#if defined(_MSC_VER) && defined(_M_IX86)
    __try {
        resolve(manager, outputShared, key);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
#else
    resolve(manager, outputShared, key);
    return true;
#endif
}

bool InvokeExtractRuntimeKeySafely(
    const ExtractRuntimeKeyFn extract,
    const void* runtimeEntity,
    CathodeRuntimeKey* output) noexcept {
#if defined(_MSC_VER) && defined(_M_IX86)
    __try {
        extract(runtimeEntity, output);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
#else
    extract(runtimeEntity, output);
    return true;
#endif
}

bool InvokeAssignNullSafely(
    const AssignEntityRefFn assign,
    void** shared) noexcept {
    void* empty = nullptr;
#if defined(_MSC_VER) && defined(_M_IX86)
    __try {
        assign(shared, &empty);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
#else
    assign(shared, &empty);
    return true;
#endif
}

bool ReadPointerSafely(const void* address, void*& value) noexcept {
    value = nullptr;
#if defined(_MSC_VER) && defined(_M_IX86)
    __try {
        value = *static_cast<void* const*>(address);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
#else
    value = *static_cast<void* const*>(address);
    return true;
#endif
}

bool ReadUint32Safely(const void* address, std::uint32_t& value) noexcept {
    value = 0U;
#if defined(_MSC_VER) && defined(_M_IX86)
    __try {
        value = *static_cast<const std::uint32_t*>(address);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
#else
    value = *static_cast<const std::uint32_t*>(address);
    return true;
#endif
}

std::size_t CopyOpaquePrefix(
    std::byte* destination,
    const std::size_t capacity,
    const void* source) noexcept {
    if (destination == nullptr || source == nullptr || capacity == 0U) {
        return 0U;
    }
    if (crash_guard::TryCopy(destination, source, capacity)) {
        return capacity;
    }

    // A failed large copy is retried in small fixed blocks so a provider whose
    // readable allocation ends inside the requested diagnostic prefix can
    // still contribute typed-neutral evidence. No read is ever written back.
    constexpr std::size_t kChunk = 16U;
    std::size_t copied = 0U;
    while (copied + kChunk <= capacity) {
        if (!crash_guard::TryCopy(
                destination + copied,
                static_cast<const std::byte*>(source) + copied,
                kChunk)) {
            break;
        }
        copied += kChunk;
    }
    return copied;
}

std::uint64_t HashOpaqueBytes(
    const std::byte* data,
    const std::size_t bytes) noexcept {
    constexpr std::uint64_t kOffset = 1469598103934665603ULL;
    constexpr std::uint64_t kPrime = 1099511628211ULL;
    std::uint64_t hash = kOffset;
    if (data == nullptr) return hash;
    for (std::size_t index = 0U; index < bytes; ++index) {
        hash ^= static_cast<std::uint8_t>(data[index]);
        hash *= kPrime;
    }
    return hash;
}

bool LooksLikePointerShapedWord(const std::uint32_t value) noexcept {
    // Diagnostic redaction is deliberately conservative. Any aligned value in
    // the normal 32-bit user-address shape is suppressed from raw scalar logs
    // even when it is not currently readable. False positives merely remove a
    // scalar observation; false negatives could expose a process address.
    return value >= 0x00010000U && value < 0xFFF00000U &&
        (value & 0x3U) == 0U;
}

bool IsProbeableNonImagePointer(const std::uint32_t value) noexcept {
    if (!LooksLikePointerShapedWord(value)) {
        return false;
    }
    const auto address = reinterpret_cast<const void*>(
        static_cast<std::uintptr_t>(value));
    if (!crash_guard::IsReadable(address, sizeof(std::uint32_t))) {
        return false;
    }
    MEMORY_BASIC_INFORMATION information{};
    if (VirtualQuery(address, &information, sizeof(information)) == 0U ||
        information.State != MEM_COMMIT || information.Type == MEM_IMAGE) {
        return false;
    }
    return crash_guard::IsReadable(address, kWorldOpaqueStatePointeeBytes);
}

} // namespace

WorldEntityLease::WorldEntityLease(
    void* sharedRef,
    void* executionObject,
    const void* assignAddress) noexcept
    : sharedRef_(sharedRef),
      executionObject_(executionObject),
      assignAddress_(assignAddress) {}

WorldEntityLease::~WorldEntityLease() noexcept {
    Reset();
}

WorldEntityLease::WorldEntityLease(WorldEntityLease&& other) noexcept
    : sharedRef_(other.sharedRef_),
      executionObject_(other.executionObject_),
      assignAddress_(other.assignAddress_) {
    other.sharedRef_ = nullptr;
    other.executionObject_ = nullptr;
    other.assignAddress_ = nullptr;
}

WorldEntityLease& WorldEntityLease::operator=(WorldEntityLease&& other) noexcept {
    if (this != &other) {
        Reset();
        sharedRef_ = other.sharedRef_;
        executionObject_ = other.executionObject_;
        assignAddress_ = other.assignAddress_;
        other.sharedRef_ = nullptr;
        other.executionObject_ = nullptr;
        other.assignAddress_ = nullptr;
    }
    return *this;
}

void WorldEntityLease::Reset() noexcept {
    if (sharedRef_ != nullptr && assignAddress_ != nullptr) {
        const auto assign = reinterpret_cast<AssignEntityRefFn>(
            const_cast<void*>(assignAddress_));
        (void)InvokeAssignNullSafely(assign, &sharedRef_);
    }
    sharedRef_ = nullptr;
    executionObject_ = nullptr;
    assignAddress_ = nullptr;
}

std::size_t WorldEntityRegistry::WorldEntityIdHash::operator()(
    const WorldEntityId& value) const noexcept {
    const auto mixed = value.low ^ (value.high + 0x9E3779B97F4A7C15ULL +
        (value.low << 6U) + (value.low >> 2U));
    return static_cast<std::size_t>(mixed ^ (mixed >> 32U));
}

std::size_t WorldEntityRegistry::CompositeInputKeyHash::operator()(
    const CompositeInputKey& value) const noexcept {
    const auto entityHash = WorldEntityIdHash{}(value.entity);
    const auto mixed = static_cast<std::uint64_t>(entityHash) ^
        (static_cast<std::uint64_t>(value.inputShortGuid) + 0x9E3779B97F4A7C15ULL +
         (static_cast<std::uint64_t>(entityHash) << 6U) +
         (static_cast<std::uint64_t>(entityHash) >> 2U));
    return static_cast<std::size_t>(mixed ^ (mixed >> 32U));
}

std::size_t WorldEntityRegistry::CompositeRuntimeInputKeyHash::operator()(
    const CompositeRuntimeInputKey& value) const noexcept {
    auto mixed = static_cast<std::uint64_t>(value.entity.entityA) |
        (static_cast<std::uint64_t>(value.entity.entityB) << 32U);
    mixed ^= static_cast<std::uint64_t>(value.inputShortGuid) +
        0x9E3779B97F4A7C15ULL + (mixed << 6U) + (mixed >> 2U);
    return static_cast<std::size_t>(mixed ^ (mixed >> 32U));
}

std::size_t WorldEntityRegistry::CompositeOwnerScopeRuntimeInputKeyHash::operator()(
    const CompositeOwnerScopeRuntimeInputKey& value) const noexcept {
    auto mixed = static_cast<std::uint64_t>(value.ownerScopeB) |
        (static_cast<std::uint64_t>(value.internalEntityA) << 32U);
    mixed ^= static_cast<std::uint64_t>(value.inputShortGuid) +
        0x9E3779B97F4A7C15ULL + (mixed << 6U) + (mixed >> 2U);
    return static_cast<std::size_t>(mixed ^ (mixed >> 32U));
}

std::size_t WorldEntityRegistry::CompositeDirectInputKeyHash::operator()(
    const CompositeDirectInputKey& value) const noexcept {
    const auto sourceHash = WorldEntityIdHash{}(value.interfaceVariable);
    const auto targetHash = WorldEntityIdHash{}(value.internalTarget);
    auto mixed = static_cast<std::uint64_t>(sourceHash) ^
        (static_cast<std::uint64_t>(value.interfaceLinkParameter) << 1U);
    mixed ^= static_cast<std::uint64_t>(targetHash) + 0x9E3779B97F4A7C15ULL +
        (mixed << 6U) + (mixed >> 2U);
    mixed ^= static_cast<std::uint64_t>(value.internalTargetInputShortGuid) +
        0xBF58476D1CE4E5B9ULL + (mixed << 6U) + (mixed >> 2U);
    return static_cast<std::size_t>(mixed ^ (mixed >> 32U));
}

WorldEntityRegistry::WorldEntityRegistry(
    const SignatureResolver& resolver,
    LevelGenerationTracker& generations,
    Diagnostics& diagnostics) noexcept
    : resolver_(&resolver), generations_(&generations), diagnostics_(&diagnostics) {}

bool WorldEntityRegistry::Initialize(std::string& error) noexcept {
    try {
        error.clear();
        managerRootAddress_ = resolver_->Resolve(SymbolId::CathodeEntityManagerRoot);
        resolveEntityAddress_ = resolver_->Resolve(SymbolId::CathodeResolveEntity);
        extractRuntimeKeyAddress_ = resolver_->Resolve(SymbolId::CathodeExtractRuntimeKey);
        assignAddress_ = resolver_->Resolve(SymbolId::CathodeAssignEntityRef);
        providerRegistryRootAddress_ = resolver_->Resolve(SymbolId::CathodeProviderRegistryRoot);
        if (managerRootAddress_ == nullptr || resolveEntityAddress_ == nullptr ||
            extractRuntimeKeyAddress_ == nullptr || assignAddress_ == nullptr ||
            providerRegistryRootAddress_ == nullptr) {
            error = "CATHODE manager/generic resolver/runtime identity/shared-ref/provider-registry symbols are unavailable";
            return false;
        }
        std::lock_guard lock(mutex_);
        initialized_ = true;
        return true;
    } catch (...) {
        error = "WorldEntityRegistry initialization threw unexpectedly";
        return false;
    }
}

bool WorldEntityRegistry::PrepareManifestRuntimeIndex(
    const std::filesystem::path& path, std::string& error) noexcept {
    try {
        error.clear();
        if (!initialized_) { error = "WorldEntityRegistry is not initialized"; return false; }
        const auto canonical = std::filesystem::weakly_canonical(path);
        std::lock_guard lock(preparationMutex_);
        if (preparationWorker_.valid()) {
            if (preparationWorker_.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
                error = "WORLD_MANIFEST_PREPARING";
                return false;
            }
            auto result = preparationWorker_.get();
            preparedManifest_ = std::move(result.index);
            preparationError_ = std::move(result.error);
        }
        if (preparationPath_ == canonical) {
            if (preparedManifest_) return true;
            if (!preparationError_.empty()) { error = preparationError_; return false; }
        }
        preparationPath_ = canonical;
        preparationError_.clear();
        // Destruction of replaced tables is performed by the worker as well.
        auto retired = std::move(retiredManifest_);
        auto previous = std::move(preparedManifest_);
        preparationWorker_ = std::async(std::launch::async,
            [canonical, retired = std::move(retired), previous = std::move(previous)]() mutable {
                retired.reset();
                previous.reset();
                ManifestPreparation result;
                auto parsed = std::make_shared<PreparedManifest>();
                if (ParseManifestRuntimeIndex(canonical, *parsed, result.error))
                    result.index = std::move(parsed);
                return result;
            });
        error = "WORLD_MANIFEST_PREPARING";
        return false;
    } catch (...) { error = "WorldManifest background preparation failed"; return false; }
}

bool WorldEntityRegistry::LoadManifestRuntimeIndex(
    const std::filesystem::path& path, const std::uint32_t levelGeneration,
    std::string& error) noexcept {
    try {
        // Live generation is checked again at the exact activation boundary.
        if (generations_ == nullptr || levelGeneration == 0U ||
            generations_->CurrentGeneration() != levelGeneration) {
            error = "WorldManifest activation generation is stale";
            return false;
        }
        const auto canonical = std::filesystem::weakly_canonical(path);
        {
            std::lock_guard lock(mutex_);
            // Poll/reconnect commands are idempotent within this exact world.
            if (manifestLoaded_ && manifestGeneration_ == levelGeneration &&
                captureReady_.load(std::memory_order_acquire) && activeManifestPath_ == canonical) {
                const auto file = CreateFileW(canonical.c_str(), GENERIC_READ, FILE_SHARE_READ,
                    nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
                std::array<std::uint32_t, 7> identity{};
                const bool same = file != INVALID_HANDLE_VALUE &&
                    ManifestFileIdentity(file, identity) && identity == activeManifestFileIdentity_;
                if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
                if (same) { error.clear(); return true; }
            }
        }
        if (!PrepareManifestRuntimeIndex(path, error)) return false;
        std::lock_guard preparationLock(preparationMutex_);
        if (canonical != preparationPath_ || !preparedManifest_) {
            error = "WORLD_MANIFEST_PREPARING"; return false;
        }
        struct FileLock final {
            HANDLE handle;
            ~FileLock() { if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle); }
        } file{CreateFileW(canonical.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr)};
        std::array<std::uint32_t, 7> identity{};
        if (file.handle == INVALID_HANDLE_VALUE || !ManifestFileIdentity(file.handle, identity) ||
            identity != preparedManifest_->fileIdentity) {
            preparationError_ = "WorldManifest changed after preparation; prepare the current file again";
            error = preparationError_;
            return false;
        }
        std::lock_guard lock(mutex_);
        if (generations_->CurrentGeneration() != levelGeneration) {
            error = "WorldManifest activation generation changed"; return false;
        }
        captureReady_.store(false, std::memory_order_release);
        std::swap(entries_, preparedManifest_->entries_);
        std::swap(idsByRuntimeKey_, preparedManifest_->idsByRuntimeKey_);
        std::swap(compositeInputDirect_, preparedManifest_->compositeInputDirect_);
        std::swap(compositeInputReverse_, preparedManifest_->compositeInputReverse_);
        std::swap(compositeInputRuntimeReverse_, preparedManifest_->compositeInputRuntimeReverse_);
        std::swap(compositeInputOwnerScopeRuntimeReverse_, preparedManifest_->compositeInputOwnerScopeRuntimeReverse_);
        std::swap(compositeInputCanonicalAnchors_, preparedManifest_->compositeInputCanonicalAnchors_);
        std::swap(compositeInputInterfaceTargets_, preparedManifest_->compositeInputInterfaceTargets_);
        std::swap(compositeInputForward_, preparedManifest_->compositeInputForward_);
        std::swap(compositePublicRootAmbiguous_, preparedManifest_->compositePublicRootAmbiguous_);
        std::swap(compositePublicRoots_, preparedManifest_->compositePublicRoots_);
        std::swap(compositeInputForwardBridges_, preparedManifest_->compositeInputForwardBridges_);
        std::swap(compositeInputCausalProfiles_, preparedManifest_->compositeInputCausalProfiles_);
        std::swap(emittedOutputCausalProfiles_, preparedManifest_->emittedOutputCausalProfiles_);
        std::swap(scheduledInputCausalProfiles_, preparedManifest_->scheduledInputCausalProfiles_);
        std::swap(certifiedStateTransitionCarrierSemantics_, preparedManifest_->certifiedStateTransitionCarrierSemantics_);
        std::swap(compositePublicInputShortGuids_, preparedManifest_->compositePublicInputShortGuids_);
        std::swap(compositeInternalInputShortGuids_, preparedManifest_->compositeInternalInputShortGuids_);

        loadedZones_.clear();
        mapContentId_ = preparedManifest_->mapContentId;
        manifestGeneration_ = levelGeneration;
        manifestLoaded_ = true;
        activeManifestPath_ = canonical;
        activeManifestFileIdentity_ = identity;
        retiredManifest_ = std::move(preparedManifest_);
        preparationPath_.clear();
        captureReady_.store(true, std::memory_order_release);
        ++invalidations_;
        diagnostics_->Log("info", "world_registry_manifest_loaded",
            "WorldManifest index prepared on an isolated worker and activated by table swap; no runtime entity scan.",
            {{"entities", Diagnostics::Unsigned(entries_.size()), true},
             {"levelGeneration", Diagnostics::Unsigned(levelGeneration), true},
             {"preparedOffEngineThread", "true", true}});
        return true;
    } catch (...) { error = "WorldManifest activation failed"; return false; }
}

bool WorldEntityRegistry::ParseManifestRuntimeIndex(
    const std::filesystem::path& path,
    PreparedManifest& prepared,
    std::string& error) noexcept {
    try {
        error.clear();
        struct FileLock final {
            HANDLE handle;
            ~FileLock() { if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle); }
        } file{CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr)};
        if (file.handle == INVALID_HANDLE_VALUE ||
            !ManifestFileIdentity(file.handle, prepared.fileIdentity)) {
            error = "unable to acquire stable WorldManifest file";
            return false;
        }
        std::ifstream stream(path, std::ios::binary);
        if (!stream) {
            error = "unable to open WorldManifest runtime index";
            return false;
        }

        std::array<char, 8> magic{};
        std::uint32_t formatVersion = 0;
        std::uint32_t compilerVersion = 0;
        WorldEntityId map{};
        std::uint32_t root = 0;
        std::uint32_t global = 0;
        std::uint32_t pause = 0;
        std::int32_t countSigned = 0;
        if (!ReadExact(stream, magic.data(), magic.size()) ||
            !ReadValue(stream, formatVersion) ||
            !ReadValue(stream, compilerVersion) ||
            !ReadValue(stream, map.high) ||
            !ReadValue(stream, map.low) ||
            !ReadValue(stream, root) ||
            !ReadValue(stream, global) ||
            !ReadValue(stream, pause) ||
            !ReadValue(stream, countSigned)) {
            error = "WorldManifest runtime header is truncated";
            return false;
        }
        (void)root;
        (void)global;
        (void)pause;
        if (magic != kManifestMagic ||
            formatVersion != kWorldManifestFormatVersion ||
            compilerVersion != kWorldManifestCompilerVersion ||
            countSigned < 0 ||
            static_cast<std::uint32_t>(countSigned) > kMaximumRuntimeEntities) {
            error = "WorldManifest runtime header is incompatible with WS3 semantic-contract/AIMPWM09";
            return false;
        }

        const auto count = static_cast<std::uint32_t>(countSigned);
        std::unordered_map<WorldEntityId, RuntimeEntry, WorldEntityIdHash> entries;
        entries.reserve(count);
        std::unordered_map<std::uint64_t, WorldEntityId> idsByRuntimeKey;
        idsByRuntimeKey.reserve(count);

        for (std::uint32_t index = 0; index < count; ++index) {
            RuntimeEntry entry{};
            std::uint8_t domain = 0;
            std::uint8_t kind = 0;
            if (!ReadValue(stream, entry.id.high) ||
                !ReadValue(stream, entry.id.low) ||
                !ReadValue(stream, entry.key.entityA) ||
                !ReadValue(stream, entry.key.entityB) ||
                !ReadValue(stream, entry.zoneId) ||
                !ReadValue(stream, entry.flags) ||
                !ReadValue(stream, domain) ||
                !ReadValue(stream, kind) ||
                !ReadValue(stream, entry.primitiveType)) {
                error = "WorldManifest runtime index is truncated";
                return false;
            }
            entry.domain = static_cast<WorldDomain>(domain);
            entry.kind = static_cast<WorldEntityKind>(kind);
            if (!entry.id.Valid() || entry.key.entityA == 0U) {
                error = "WorldManifest runtime index contains an invalid entity key";
                return false;
            }
            const auto packed = PackKey(entry.key);
            if (const auto duplicate = idsByRuntimeKey.find(packed);
                duplicate != idsByRuntimeKey.end() && !(duplicate->second == entry.id)) {
                error = "WorldManifest contains a duplicate CATHODE runtime key";
                return false;
            }
            if (!entries.emplace(entry.id, entry).second) {
                error = "WorldManifest runtime index contains a duplicate WorldEntityId";
                return false;
            }
            idsByRuntimeKey[packed] = entry.id;
        }

        std::int32_t compositeInputCountSigned = 0;
        if (!ReadValue(stream, compositeInputCountSigned) || compositeInputCountSigned < 0 ||
            static_cast<std::uint32_t>(compositeInputCountSigned) > kMaximumCompositePublicInputs) {
            error = "WorldManifest Composite public-input runtime index is truncated or invalid";
            return false;
        }

        std::unordered_map<CompositeInputKey, CompositeInputReverseEntry, CompositeInputKeyHash>
            compositeInputReverse;
        std::unordered_map<CompositeRuntimeInputKey, CompositeInputReverseEntry, CompositeRuntimeInputKeyHash>
            compositeInputRuntimeReverse;
        std::unordered_map<CompositeOwnerScopeRuntimeInputKey, CompositeInputReverseEntry, CompositeOwnerScopeRuntimeInputKeyHash>
            compositeInputOwnerScopeRuntimeReverse;
        std::unordered_map<CompositeDirectInputKey, CompositeInputReverseEntry, CompositeDirectInputKeyHash>
            compositeInputDirect;
        std::unordered_map<CompositeInputKey, CompositeInputReverseEntry, CompositeInputKeyHash>
            compositeInputInterfaceTargets;
        std::unordered_map<CompositeInputKey, bool, CompositeInputKeyHash> compositeInputForward;
        std::unordered_set<CompositeInputKey, CompositeInputKeyHash> compositePublicRootAmbiguous;
        std::unordered_map<CompositeInputKey, std::vector<WorldCompositePublicInputDescriptor>, CompositeInputKeyHash>
            compositeInputForwardBridges;
        std::unordered_set<std::uint32_t> compositePublicInputShortGuids;
        std::unordered_set<std::uint32_t> compositeInternalInputShortGuids;
        const auto compositeInputCount = static_cast<std::uint32_t>(compositeInputCountSigned);
        compositeInputDirect.reserve(compositeInputCount);
        compositeInputReverse.reserve(compositeInputCount);
        compositeInputRuntimeReverse.reserve(compositeInputCount);
        compositeInputOwnerScopeRuntimeReverse.reserve(compositeInputCount);
        compositeInputInterfaceTargets.reserve(compositeInputCount);
        compositeInputForward.reserve(compositeInputCount);
        compositeInputForwardBridges.reserve(compositeInputCount);
        compositePublicInputShortGuids.reserve(compositeInputCount);
        compositeInternalInputShortGuids.reserve(compositeInputCount);

        for (std::uint32_t index = 0; index < compositeInputCount; ++index) {
            WorldCompositePublicInputDescriptor descriptor{};
            std::uint8_t noTypedData = 0U;
            std::uint8_t internalFallbackExclusive = 0U;
            std::uint16_t reserved16 = 0U;
            if (!ReadValue(stream, descriptor.ownerCompositeInstance.high) ||
                !ReadValue(stream, descriptor.ownerCompositeInstance.low) ||
                !ReadValue(stream, descriptor.interfaceVariableEntity.high) ||
                !ReadValue(stream, descriptor.interfaceVariableEntity.low) ||
                !ReadValue(stream, descriptor.publicInputShortGuid) ||
                !ReadValue(stream, descriptor.interfaceLinkParameter) ||
                !ReadValue(stream, descriptor.internalTargetEntity.high) ||
                !ReadValue(stream, descriptor.internalTargetEntity.low) ||
                !ReadValue(stream, descriptor.internalTargetInputShortGuid) ||
                !ReadValue(stream, descriptor.dataType) ||
                !ReadValue(stream, noTypedData) ||
                !ReadValue(stream, internalFallbackExclusive) ||
                !ReadValue(stream, reserved16)) {
                error = "WorldManifest Composite public-input runtime index is truncated";
                return false;
            }
            if (noTypedData > 1U || internalFallbackExclusive > 1U || reserved16 != 0U ||
                !descriptor.ownerCompositeInstance.Valid() ||
                !descriptor.interfaceVariableEntity.Valid() ||
                !descriptor.internalTargetEntity.Valid() ||
                descriptor.publicInputShortGuid == 0U ||
                descriptor.internalTargetInputShortGuid == 0U) {
                error = "WorldManifest Composite public-input runtime index contains an invalid bridge";
                return false;
            }
            descriptor.noTypedData = noTypedData != 0U;
            descriptor.internalFallbackExclusive = internalFallbackExclusive != 0U;
            // CATHODE action-like public Methods (DataType.NONE) may have an
            // invalid/zero source connector parameter. Their stable public
            // identity is descriptor.publicInputShortGuid; typed inputs still
            // require a concrete interfaceLinkParameter.
            if (!descriptor.noTypedData && descriptor.interfaceLinkParameter == 0U) {
                error = "WorldManifest typed Composite public-input bridge has a zero interface parameter";
                return false;
            }
            compositePublicInputShortGuids.insert(descriptor.publicInputShortGuid);
            if (descriptor.noTypedData) {
                compositeInternalInputShortGuids.insert(descriptor.internalTargetInputShortGuid);
            }

            const auto ownerEntry = entries.find(descriptor.ownerCompositeInstance);
            const auto variableEntry = entries.find(descriptor.interfaceVariableEntity);
            const auto targetEntry = entries.find(descriptor.internalTargetEntity);
            if (ownerEntry == entries.end() || variableEntry == entries.end() ||
                targetEntry == entries.end() ||
                ownerEntry->second.kind != WorldEntityKind::CompositeInstance ||
                variableEntry->second.kind != WorldEntityKind::Variable) {
                error = "WorldManifest Composite public-input bridge is not runtime-addressable";
                return false;
            }

            // Fix3.27 BuildFix01: index the exact public interface VariableEntity
            // identity alongside the owner CompositeInstance. Multiple childLinks
            // from the same public variable are valid only when they resolve to
            // the same canonical owner+public input; otherwise this path is
            // permanently ambiguous and therefore ineligible.
            const CompositeInputKey interfaceTargetKey{
                descriptor.interfaceVariableEntity, descriptor.publicInputShortGuid};
            const auto interfaceTargetFound =
                compositeInputInterfaceTargets.find(interfaceTargetKey);
            if (interfaceTargetFound == compositeInputInterfaceTargets.end()) {
                compositeInputInterfaceTargets.emplace(
                    interfaceTargetKey, CompositeInputReverseEntry{descriptor, false});
            } else {
                const auto& prior = interfaceTargetFound->second.descriptor;
                const bool sameCanonical =
                    prior.ownerCompositeInstance == descriptor.ownerCompositeInstance &&
                    prior.publicInputShortGuid == descriptor.publicInputShortGuid &&
                    prior.noTypedData == descriptor.noTypedData;
                if (!sameCanonical) interfaceTargetFound->second.ambiguous = true;
            }

            const CompositeDirectInputKey directKey{
                descriptor.interfaceVariableEntity,
                descriptor.interfaceLinkParameter,
                descriptor.internalTargetEntity,
                descriptor.internalTargetInputShortGuid};
            const auto directFound = compositeInputDirect.find(directKey);
            if (directFound == compositeInputDirect.end()) {
                compositeInputDirect.emplace(
                    directKey, CompositeInputReverseEntry{descriptor, false});
            } else {
                const auto& prior = directFound->second.descriptor;
                const bool sameCanonical =
                    prior.ownerCompositeInstance == descriptor.ownerCompositeInstance &&
                    prior.publicInputShortGuid == descriptor.publicInputShortGuid &&
                    prior.noTypedData == descriptor.noTypedData;
                if (!sameCanonical) directFound->second.ambiguous = true;
            }

            // Fix3.24 BuildFix01: reverse canonicalization is based on the
            // uniqueness of the PUBLIC mapping, not on whether other internal
            // graph links can also drive the same input. `internalFallbackExclusive`
            // remains serialized diagnostic structure, but is no longer a hard
            // permission gate. If two different public inputs map to the same
            // internal endpoint, the reverse entry is marked ambiguous and the
            // fallback still fails closed.
            const CompositeInputKey reverseKey{
                descriptor.internalTargetEntity, descriptor.internalTargetInputShortGuid};
            const auto reverseFound = compositeInputReverse.find(reverseKey);
            if (reverseFound == compositeInputReverse.end()) {
                compositeInputReverse.emplace(
                    reverseKey, CompositeInputReverseEntry{descriptor, false});
            } else {
                const auto& prior = reverseFound->second.descriptor;
                const bool sameCanonical =
                    prior.ownerCompositeInstance == descriptor.ownerCompositeInstance &&
                    prior.publicInputShortGuid == descriptor.publicInputShortGuid &&
                    prior.noTypedData == descriptor.noTypedData;
                if (!sameCanonical) reverseFound->second.ambiguous = true;
            }

            // Fix3.25: mirror the exact static reverse edge by the manifest
            // CATHODE RuntimeKey of the internal target. Runtime evidence shows
            // that nested Composite inputs may reach QueueInput only after the
            // public ShortGuid has been resolved to an internal work item. The
            // key stays process-local; only the stable public Composite input
            // is ever submitted to WorldSync.
            const CompositeRuntimeInputKey runtimeReverseKey{
                targetEntry->second.key, descriptor.internalTargetInputShortGuid};
            const auto runtimeReverseFound = compositeInputRuntimeReverse.find(runtimeReverseKey);
            if (runtimeReverseFound == compositeInputRuntimeReverse.end()) {
                compositeInputRuntimeReverse.emplace(
                    runtimeReverseKey, CompositeInputReverseEntry{descriptor, false});
            } else {
                const auto& prior = runtimeReverseFound->second.descriptor;
                const bool sameCanonical =
                    prior.ownerCompositeInstance == descriptor.ownerCompositeInstance &&
                    prior.publicInputShortGuid == descriptor.publicInputShortGuid &&
                    prior.noTypedData == descriptor.noTypedData;
                if (!sameCanonical) runtimeReverseFound->second.ambiguous = true;
            }

            // Fix3.26: nested Composite runtime objects can retain the owner
            // CompositeInstance's scope key B while COMMANDS-derived child
            // entities use the child-path-derived B. Keep this alternate index
            // entirely runtime-local and fail closed on any canonical
            // ambiguity. The target entity A and exact internal input remain
            // part of the key, so this is not a broad input-name fallback.
            if (descriptor.noTypedData && ownerEntry->second.key.entityB != 0U &&
                targetEntry->second.key.entityA != 0U) {
                const CompositeOwnerScopeRuntimeInputKey ownerScopeKey{
                    ownerEntry->second.key.entityB,
                    targetEntry->second.key.entityA,
                    descriptor.internalTargetInputShortGuid};
                const auto ownerScopeFound =
                    compositeInputOwnerScopeRuntimeReverse.find(ownerScopeKey);
                if (ownerScopeFound == compositeInputOwnerScopeRuntimeReverse.end()) {
                    compositeInputOwnerScopeRuntimeReverse.emplace(
                        ownerScopeKey, CompositeInputReverseEntry{descriptor, false});
                } else {
                    const auto& prior = ownerScopeFound->second.descriptor;
                    const bool sameCanonical =
                        prior.ownerCompositeInstance == descriptor.ownerCompositeInstance &&
                        prior.publicInputShortGuid == descriptor.publicInputShortGuid &&
                        prior.noTypedData == descriptor.noTypedData;
                    if (!sameCanonical) ownerScopeFound->second.ambiguous = true;
                }
            }

            const CompositeInputKey forwardKey{
                descriptor.ownerCompositeInstance, descriptor.publicInputShortGuid};
            const auto forwardFound = compositeInputForward.find(forwardKey);
            if (forwardFound == compositeInputForward.end()) {
                compositeInputForward.emplace(forwardKey, descriptor.noTypedData);
            } else {
                forwardFound->second = forwardFound->second && descriptor.noTypedData;
            }

            // Preserve every exact COMMANDS childLink behind the public input.
            // A public parameter is allowed to fan out to multiple internal
            // entities; the receiver must replay the complete local bridge set.
            auto& bridges = compositeInputForwardBridges[forwardKey];
            // WS3 retail ROOT authorizes a public port, not a particular childLink or
            // static value type. Cache conflicting interface identities once
            // at load time; normal fan-out stays one unambiguous public input.
            if (!bridges.empty() &&
                !(bridges.front().interfaceVariableEntity == descriptor.interfaceVariableEntity)) {
                compositePublicRootAmbiguous.insert(forwardKey);
            }
            const bool duplicateBridge = std::any_of(
                bridges.begin(), bridges.end(), [&](const auto& prior) {
                    return prior.interfaceVariableEntity == descriptor.interfaceVariableEntity &&
                        prior.interfaceLinkParameter == descriptor.interfaceLinkParameter &&
                        prior.internalTargetEntity == descriptor.internalTargetEntity &&
                        prior.internalTargetInputShortGuid == descriptor.internalTargetInputShortGuid;
                });
            if (!duplicateBridge) bridges.push_back(descriptor);
        }

        std::int32_t canonicalAnchorCountSigned = 0;
        if (!ReadValue(stream, canonicalAnchorCountSigned) || canonicalAnchorCountSigned < 0 ||
            static_cast<std::uint32_t>(canonicalAnchorCountSigned) >
                kMaximumCompositePublicInputCanonicalAnchors) {
            error = "WorldManifest Composite public-input canonical-anchor index is truncated or invalid";
            return false;
        }

        std::unordered_map<CompositeInputKey, CompositeInputReverseEntry, CompositeInputKeyHash>
            compositeInputCanonicalAnchors;
        const auto canonicalAnchorCount =
            static_cast<std::uint32_t>(canonicalAnchorCountSigned);
        compositeInputCanonicalAnchors.reserve(canonicalAnchorCount);
        for (std::uint32_t index = 0; index < canonicalAnchorCount; ++index) {
            WorldCompositePublicInputCanonicalAnchor anchor{};
            if (!ReadValue(stream, anchor.ownerCompositeInstance.high) ||
                !ReadValue(stream, anchor.ownerCompositeInstance.low) ||
                !ReadValue(stream, anchor.publicInputShortGuid) ||
                !ReadValue(stream, anchor.internalTargetEntity.high) ||
                !ReadValue(stream, anchor.internalTargetEntity.low) ||
                !ReadValue(stream, anchor.internalTargetInputShortGuid)) {
                error = "WorldManifest Composite public-input canonical-anchor index is truncated";
                return false;
            }
            if (!anchor.ownerCompositeInstance.Valid() ||
                anchor.publicInputShortGuid == 0U ||
                !anchor.internalTargetEntity.Valid() ||
                anchor.internalTargetInputShortGuid == 0U) {
                error = "WorldManifest Composite public-input canonical-anchor contains an invalid identity";
                return false;
            }

            const auto ownerEntry = entries.find(anchor.ownerCompositeInstance);
            const auto targetEntry = entries.find(anchor.internalTargetEntity);
            if (ownerEntry == entries.end() || targetEntry == entries.end() ||
                ownerEntry->second.kind != WorldEntityKind::CompositeInstance) {
                error = "WorldManifest Composite public-input canonical-anchor is not runtime-addressable";
                return false;
            }

            const CompositeInputKey forwardKey{
                anchor.ownerCompositeInstance, anchor.publicInputShortGuid};
            const auto allowed = compositeInputForward.find(forwardKey);
            const auto bridges = compositeInputForwardBridges.find(forwardKey);
            if (allowed == compositeInputForward.end() || !allowed->second ||
                bridges == compositeInputForwardBridges.end() || bridges->second.empty()) {
                error = "WorldManifest Composite public-input canonical-anchor references a blocked public input";
                return false;
            }

            // Reuse the canonical public descriptor already validated above.
            // The anchor's internal identity is lookup-only and never replaces
            // the exact childLinks used by the receiver.
            auto descriptor = bridges->second.front();
            const CompositeInputKey anchorKey{
                anchor.internalTargetEntity, anchor.internalTargetInputShortGuid};
            const auto found = compositeInputCanonicalAnchors.find(anchorKey);
            if (found == compositeInputCanonicalAnchors.end()) {
                compositeInputCanonicalAnchors.emplace(
                    anchorKey, CompositeInputReverseEntry{descriptor, false});
            } else {
                const auto& prior = found->second.descriptor;
                const bool sameCanonical =
                    prior.ownerCompositeInstance == descriptor.ownerCompositeInstance &&
                    prior.publicInputShortGuid == descriptor.publicInputShortGuid &&
                    prior.noTypedData == descriptor.noTypedData;
                if (!sameCanonical) found->second.ambiguous = true;
            }
        }

        std::int32_t causalProfileCountSigned = 0;
        if (!ReadValue(stream, causalProfileCountSigned) || causalProfileCountSigned < 0 ||
            static_cast<std::uint32_t>(causalProfileCountSigned) >
                kMaximumCompositePublicInputCausalProfiles) {
            error = "WorldManifest Composite public-input causal-profile index is truncated or invalid";
            return false;
        }
        const auto causalProfileCount = static_cast<std::uint32_t>(causalProfileCountSigned);
        std::unordered_map<CompositeInputKey, WorldCompositePublicInputCausalProfile, CompositeInputKeyHash>
            compositeInputCausalProfiles, emittedOutputCausalProfiles, scheduledInputCausalProfiles;
        std::unordered_set<CompositeInputKey, CompositeInputKeyHash>
            certifiedStateTransitionCarrierSemantics;
        compositeInputCausalProfiles.reserve(causalProfileCount);
        certifiedStateTransitionCarrierSemantics.reserve(causalProfileCount / 32U + 1U);
        constexpr std::uint32_t kKnownCausalDependencyFlags =
            WorldCausalDependencyWorldEffect |
            WorldCausalDependencyPlayer |
            WorldCausalDependencyAI |
            WorldCausalDependencyUI |
            WorldCausalDependencyAIMP |
            WorldCausalDependencyUnknown |
            WorldCausalDependencyTruncated | WorldCausalDependencyBranching;
        for (std::uint32_t index = 0; index < causalProfileCount; ++index) {
            WorldCompositePublicInputCausalProfile profile{};
            if (!ReadValue(stream, profile.ownerCompositeInstance.high) ||
                !ReadValue(stream, profile.ownerCompositeInstance.low) ||
                !ReadValue(stream, profile.publicInputShortGuid) ||
                !ReadValue(stream, profile.dependencyFlags) ||
                !ReadValue(stream, profile.endpointKind) ||
                !ReadValue(stream, profile.replicationRelevance) ||
                !ReadValue(stream, profile.replicationContractKind) ||
                !ReadValue(stream, profile.semanticTargetEntity.high) ||
                !ReadValue(stream, profile.semanticTargetEntity.low) ||
                !ReadValue(stream, profile.semanticOperation)) {
                error = "WorldManifest Composite public-input causal-profile index is truncated";
                return false;
            }
            const auto disposition = static_cast<std::uint32_t>(profile.RelevanceDisposition());
            const auto semanticClass = static_cast<std::uint32_t>(profile.SemanticClass());
            const auto contractKind = static_cast<std::uint32_t>(profile.replicationContractKind);
            const auto knownRelevanceMask = kWorldReplicationDispositionMask |
                kWorldReplicationSemanticClassMask | kWorldReplicationKnownEvidenceMask;
            const auto contractMetadataValid =
                profile.replicationContractKind == WorldReplicationContractKind::Unknown
                    ? !profile.semanticTargetEntity.Valid() && profile.semanticOperation == 0U
                    : profile.semanticTargetEntity.Valid() && profile.semanticOperation != 0U &&
                        entries.find(profile.semanticTargetEntity) != entries.end();
            if (!profile.ownerCompositeInstance.Valid() ||
                profile.publicInputShortGuid == 0U ||
                (profile.dependencyFlags & ~kKnownCausalDependencyFlags) != 0U ||
                (profile.replicationRelevance & ~knownRelevanceMask) != 0U ||
                disposition > static_cast<std::uint32_t>(
                    WorldReplicationRelevanceDisposition::ReplicateCarrier) ||
                semanticClass > static_cast<std::uint32_t>(
                    WorldReplicationSemanticClass::CompositeRouting) ||
                contractKind > static_cast<std::uint32_t>(
                    WorldReplicationContractKind::StateTransition) ||
                !contractMetadataValid) {
                error = "WorldManifest Composite public-input causal profile contains invalid metadata";
                return false;
            }
            const CompositeInputKey key{
                profile.ownerCompositeInstance, profile.publicInputShortGuid};
            auto& indexMap = profile.endpointKind == 1U ? compositeInputCausalProfiles
                : profile.endpointKind == 2U ? emittedOutputCausalProfiles : scheduledInputCausalProfiles;
            if ((profile.endpointKind == 1U
                    ? compositeInputForwardBridges.find(key) == compositeInputForwardBridges.end()
                    : (profile.endpointKind != 2U && profile.endpointKind != 3U) || entries.find(profile.ownerCompositeInstance) == entries.end()) ||
                !indexMap.emplace(key, profile).second) {
                error = "WorldManifest Composite public-input causal profile references an unknown or duplicate public input";
                return false;
            }
            if (profile.CertifiedStateTransitionCarrierBoundary()) {
                certifiedStateTransitionCarrierSemantics.emplace(
                    CompositeInputKey{profile.semanticTargetEntity, profile.semanticOperation});
            }
        }
        if (compositeInputCausalProfiles.size() != compositeInputForwardBridges.size()) {
            error = "WorldManifest Composite public-input causal profiles do not cover every public input";
            return false;
        }

        world_actions::PublicRootIndex publicRoots;
        for (const auto& [key, bridges] : compositeInputForwardBridges) {
            (void)key;
            for (const auto& bridge : bridges) {
                const auto interfaceDomain = entries.at(bridge.interfaceVariableEntity).domain;
                const auto targetDomain = entries.at(bridge.internalTargetEntity).domain;
                const auto local = [](WorldDomain domain) {
                    return domain != WorldDomain::World && domain != WorldDomain::Unknown;
                };
                publicRoots.Add(bridge,
                    entries.at(bridge.ownerCompositeInstance).domain == WorldDomain::World,
                    interfaceDomain == WorldDomain::World,
                    targetDomain == WorldDomain::World,
                    local(interfaceDomain) || local(targetDomain));
            }
        }
        prepared.entries_ = std::move(entries);
        prepared.idsByRuntimeKey_ = std::move(idsByRuntimeKey);
        prepared.compositeInputDirect_ = std::move(compositeInputDirect);
        prepared.compositeInputReverse_ = std::move(compositeInputReverse);
        prepared.compositeInputRuntimeReverse_ = std::move(compositeInputRuntimeReverse);
        prepared.compositeInputOwnerScopeRuntimeReverse_ = std::move(compositeInputOwnerScopeRuntimeReverse);
        prepared.compositeInputCanonicalAnchors_ = std::move(compositeInputCanonicalAnchors);
        prepared.compositeInputInterfaceTargets_ = std::move(compositeInputInterfaceTargets);
        prepared.compositeInputForward_ = std::move(compositeInputForward);
        prepared.compositePublicRootAmbiguous_ = std::move(compositePublicRootAmbiguous);
        prepared.compositePublicRoots_ = std::move(publicRoots);
        prepared.compositeInputForwardBridges_ = std::move(compositeInputForwardBridges);
        prepared.compositeInputCausalProfiles_ = std::move(compositeInputCausalProfiles);
        prepared.emittedOutputCausalProfiles_ = std::move(emittedOutputCausalProfiles);
        prepared.scheduledInputCausalProfiles_ = std::move(scheduledInputCausalProfiles);
        prepared.certifiedStateTransitionCarrierSemantics_ = std::move(certifiedStateTransitionCarrierSemantics);
        prepared.compositePublicInputShortGuids_ = std::move(compositePublicInputShortGuids);
        prepared.compositeInternalInputShortGuids_ = std::move(compositeInternalInputShortGuids);
        prepared.mapContentId = map;
        return true;
    } catch (...) {
        error = "WorldManifest runtime index load failed unexpectedly";
        return false;
    }
}

WorldRegistryResolveResult WorldEntityRegistry::Resolve(
    const WorldEntityId& id) noexcept {
    WorldRegistryResolveResult result{};
    RuntimeEntry entry{};
    {
        std::lock_guard lock(mutex_);
        ++resolveRequests_;
        if (!manifestLoaded_) {
            result.status = WorldRegistryResolveStatus::ManifestNotLoaded;
            return result;
        }
        const auto currentGeneration = static_cast<std::uint32_t>(
            generations_->CurrentGeneration());
        if (manifestGeneration_ != 0U && currentGeneration != manifestGeneration_) {
            result.status = WorldRegistryResolveStatus::GenerationMismatch;
            return result;
        }
        const auto found = entries_.find(id);
        if (found == entries_.end()) {
            result.status = WorldRegistryResolveStatus::EntityNotInManifest;
            return result;
        }
        entry = found->second;
        result.key = entry.key;
        result.zoneId = entry.zoneId;

        if (entry.zoneId != 0U) {
            const auto zone = loadedZones_.find(entry.zoneId);
            if (zone != loadedZones_.end() && !zone->second) {
                ++pendingMisses_;
                result.status = WorldRegistryResolveStatus::PendingUntilAvailable;
                return result;
            }
        }
    }

    bool nativeFault = false;
    auto lease = ResolveNative(entry.key, nativeFault);
    if (!lease) {
        std::lock_guard lock(mutex_);
        if (nativeFault) {
            ++nativeFaults_;
            result.status = WorldRegistryResolveStatus::NativeFault;
        } else {
            ++pendingMisses_;
            result.status = WorldRegistryResolveStatus::PendingUntilAvailable;
        }
        return result;
    }

    {
        std::lock_guard lock(mutex_);
        const auto generation = static_cast<std::uint32_t>(
            generations_->CurrentGeneration());
        if (manifestGeneration_ != 0U && generation != manifestGeneration_) {
            result.status = WorldRegistryResolveStatus::GenerationMismatch;
            return result;
        }
        if (entry.zoneId != 0U &&
            (entry.flags & WorldEntityFlagZonePrimitive) != 0U) {
            loadedZones_[entry.zoneId] = true;
        }
        ++resolveHits_;
    }

    result.status = WorldRegistryResolveStatus::Resolved;
    result.lease = std::move(lease);
    return result;
}

std::optional<WorldEntityId> WorldEntityRegistry::FindWorldEntityId(
    const CathodeRuntimeKey& key) const noexcept {
    if (key.entityA == 0U) {
        return std::nullopt;
    }
    std::lock_guard lock(mutex_);
    if (!manifestLoaded_) {
        return std::nullopt;
    }
    const auto found = idsByRuntimeKey_.find(PackKey(key));
    if (found == idsByRuntimeKey_.end()) {
        return std::nullopt;
    }
    return found->second;
}

std::optional<WorldCompositePublicInputDescriptor>
WorldEntityRegistry::FindCompositePublicInputForDirectEdge(
    const WorldEntityId& interfaceVariable,
    const std::uint32_t interfaceLinkParameter,
    const WorldEntityId& internalTarget,
    const std::uint32_t internalTargetInputShortGuid) const noexcept {
    if (!interfaceVariable.Valid() || interfaceLinkParameter == 0U ||
        !internalTarget.Valid() || internalTargetInputShortGuid == 0U) {
        return std::nullopt;
    }
    std::lock_guard lock(mutex_);
    if (!manifestLoaded_ || !captureReady_.load(std::memory_order_acquire)) {
        return std::nullopt;
    }
    const auto found = compositeInputDirect_.find(CompositeDirectInputKey{
        interfaceVariable, interfaceLinkParameter,
        internalTarget, internalTargetInputShortGuid});
    if (found == compositeInputDirect_.end() || found->second.ambiguous ||
        !found->second.descriptor.noTypedData) {
        return std::nullopt;
    }
    return found->second.descriptor;
}

std::optional<WorldCompositePublicInputDescriptor>
WorldEntityRegistry::FindCompositePublicInputForInternalEdge(
    const WorldEntityId& internalTarget,
    const std::uint32_t internalTargetInputShortGuid) const noexcept {
    if (!internalTarget.Valid() || internalTargetInputShortGuid == 0U) return std::nullopt;
    std::lock_guard lock(mutex_);
    if (!manifestLoaded_ || !captureReady_.load(std::memory_order_acquire)) {
        return std::nullopt;
    }
    const auto found = compositeInputReverse_.find(
        CompositeInputKey{internalTarget, internalTargetInputShortGuid});
    if (found == compositeInputReverse_.end() || found->second.ambiguous ||
        !found->second.descriptor.noTypedData) {
        return std::nullopt;
    }
    return found->second.descriptor;
}

std::optional<WorldCompositePublicInputDescriptor>
WorldEntityRegistry::FindCompositePublicInputForRuntimeEdge(
    const CathodeRuntimeKey& internalTarget,
    const std::uint32_t internalTargetInputShortGuid) const noexcept {
    if (internalTarget.entityA == 0U || internalTargetInputShortGuid == 0U) {
        return std::nullopt;
    }
    std::lock_guard lock(mutex_);
    if (!manifestLoaded_ || !captureReady_.load(std::memory_order_acquire)) {
        return std::nullopt;
    }
    const auto found = compositeInputRuntimeReverse_.find(
        CompositeRuntimeInputKey{internalTarget, internalTargetInputShortGuid});
    if (found == compositeInputRuntimeReverse_.end() || found->second.ambiguous ||
        !found->second.descriptor.noTypedData) {
        return std::nullopt;
    }
    return found->second.descriptor;
}

std::optional<WorldCompositePublicInputDescriptor>
WorldEntityRegistry::FindCompositePublicInputForOwnerScopeRuntimeEdge(
    const CathodeRuntimeKey& internalTarget,
    const std::uint32_t internalTargetInputShortGuid) const noexcept {
    if (internalTarget.entityA == 0U || internalTarget.entityB == 0U ||
        internalTargetInputShortGuid == 0U) {
        return std::nullopt;
    }
    std::lock_guard lock(mutex_);
    if (!manifestLoaded_ || !captureReady_.load(std::memory_order_acquire)) {
        return std::nullopt;
    }
    const auto found = compositeInputOwnerScopeRuntimeReverse_.find(
        CompositeOwnerScopeRuntimeInputKey{
            internalTarget.entityB,
            internalTarget.entityA,
            internalTargetInputShortGuid});
    if (found == compositeInputOwnerScopeRuntimeReverse_.end() ||
        found->second.ambiguous || !found->second.descriptor.noTypedData) {
        return std::nullopt;
    }
    return found->second.descriptor;
}

std::optional<WorldCompositePublicInputDescriptor>
WorldEntityRegistry::FindCompositePublicInputForCanonicalAnchor(
    const WorldEntityId& internalTarget,
    const std::uint32_t internalTargetInputShortGuid) const noexcept {
    if (!internalTarget.Valid() || internalTargetInputShortGuid == 0U) return std::nullopt;
    std::lock_guard lock(mutex_);
    if (!manifestLoaded_ || !captureReady_.load(std::memory_order_acquire)) {
        return std::nullopt;
    }
    const auto found = compositeInputCanonicalAnchors_.find(
        CompositeInputKey{internalTarget, internalTargetInputShortGuid});
    if (found == compositeInputCanonicalAnchors_.end() || found->second.ambiguous ||
        !found->second.descriptor.noTypedData) {
        return std::nullopt;
    }
    return found->second.descriptor;
}

bool WorldEntityRegistry::IsCompositePublicInputAllowed(
    const WorldEntityId& ownerCompositeInstance,
    const std::uint32_t publicInputShortGuid) const noexcept {
    if (!ownerCompositeInstance.Valid() || publicInputShortGuid == 0U) return false;
    std::lock_guard lock(mutex_);
    if (!manifestLoaded_ || !captureReady_.load(std::memory_order_acquire)) {
        return false;
    }
    const auto found = compositeInputForward_.find(
        CompositeInputKey{ownerCompositeInstance, publicInputShortGuid});
    return found != compositeInputForward_.end() && found->second;
}

bool WorldEntityRegistry::IsCompositePublicRootKnown(
    const WorldEntityId& ownerCompositeInstance,
    const std::uint32_t publicInputShortGuid) const noexcept {
    if (!ownerCompositeInstance.Valid() || publicInputShortGuid == 0U) return false;
    std::lock_guard lock(mutex_);
    if (!manifestLoaded_ || !captureReady_.load(std::memory_order_acquire)) return false;

    return compositePublicRoots_.Known(ownerCompositeInstance, publicInputShortGuid);
}

bool WorldEntityRegistry::IsCompositePublicRootActionKnown(
    const WorldEntityId& owner, const std::uint32_t input,
    const WorldEntityId& target, const double delay, const std::uint8_t flags) const noexcept {
    if (!owner.Valid() || !target.Valid() || input == 0U ||
        !std::isfinite(delay) || delay < 0.0 || delay > 600.0 ||
        (flags & ~world_actions::kPublicRootKnownFlags) != 0U) return false;
    const bool external =
        (flags & world_actions::kPublicRootFlagExternalInvocation) != 0U;
    const bool detachedCallback =
        (flags & world_actions::kPublicRootFlagDetachedCallback) != 0U;
    // These describe mutually exclusive local invocation shapes.
    if (external && detachedCallback) return false;
    std::lock_guard lock(mutex_);
    if (!manifestLoaded_ || !captureReady_.load(std::memory_order_acquire)) return false;
    const auto entry = entries_.find(target);
    if (entry == entries_.end() || entry->second.domain != WorldDomain::World) return false;
    return compositePublicRoots_.Allows(owner, input, target,
        delay != 0.0 || external || detachedCallback, external || detachedCallback);
}

CompositePublicInputManifestDiagnostics
WorldEntityRegistry::InspectCompositePublicInputManifest(
    const WorldEntityId& ownerCompositeInstance,
    const std::uint32_t publicInputShortGuid) const noexcept {
    CompositePublicInputManifestDiagnostics result{};
    if (!ownerCompositeInstance.Valid() || publicInputShortGuid == 0U) return result;

    std::lock_guard lock(mutex_);
    result.manifestReady = manifestLoaded_ && captureReady_.load(std::memory_order_acquire);
    if (!result.manifestReady) return result;

    result.guidKnown = compositePublicInputShortGuids_.contains(publicInputShortGuid);
    const CompositeInputKey exactKey{ownerCompositeInstance, publicInputShortGuid};
    if (const auto found = compositeInputForward_.find(exactKey);
        found != compositeInputForward_.end()) {
        result.exactForwardKeyPresent = true;
        result.exactForwardAllowed = found->second;
    }
    if (const auto bridges = compositeInputForwardBridges_.find(exactKey);
        bridges != compositeInputForwardBridges_.end()) {
        result.exactBridgeCount = static_cast<std::uint32_t>(bridges->second.size());
        for (const auto& descriptor : bridges->second) {
            if (descriptor.noTypedData) ++result.exactPayloadlessBridgeCount;
            else ++result.exactTypedBridgeCount;
        }
    }

    std::unordered_set<WorldEntityId, WorldEntityIdHash> owners;
    bool firstDescriptor = true;
    for (const auto& [key, bridges] : compositeInputForwardBridges_) {
        if (key.inputShortGuid != publicInputShortGuid) continue;
        owners.insert(key.entity);
        if (key.entity == ownerCompositeInstance) result.exactOwnerSeenForGuid = true;
        for (const auto& descriptor : bridges) {
            ++result.guidBridgeCount;
            if (firstDescriptor) {
                result.firstGuidOwner = descriptor.ownerCompositeInstance;
                result.firstGuidDataType = descriptor.dataType;
                result.firstGuidNoTypedData = descriptor.noTypedData;
                firstDescriptor = false;
            }
        }
    }
    result.guidOwnerCount = static_cast<std::uint32_t>(owners.size());
    return result;
}

std::optional<WorldCompositePublicInputCausalProfile>
WorldEntityRegistry::FindCompositePublicInputCausalProfile(
    const WorldEntityId& ownerCompositeInstance,
    const std::uint32_t publicInputShortGuid) const noexcept {
    if (!ownerCompositeInstance.Valid() || publicInputShortGuid == 0U) return std::nullopt;
    std::lock_guard lock(mutex_);
    if (!manifestLoaded_ || !captureReady_.load(std::memory_order_acquire)) return std::nullopt;
    const auto found = compositeInputCausalProfiles_.find(
        CompositeInputKey{ownerCompositeInstance, publicInputShortGuid});
    if (found == compositeInputCausalProfiles_.end()) return std::nullopt;
    return found->second;
}

std::optional<WorldCompositePublicInputCausalProfile>
WorldEntityRegistry::FindEmittedOutputCausalProfile(const WorldEntityId& owner,
    const std::uint32_t output) const noexcept {
    if (!owner.Valid() || output == 0U) return std::nullopt;
    std::lock_guard lock(mutex_);
    if (!manifestLoaded_ || !captureReady_.load(std::memory_order_acquire)) return std::nullopt;
    const auto found = emittedOutputCausalProfiles_.find(CompositeInputKey{owner, output});
    return found == emittedOutputCausalProfiles_.end() ? std::nullopt
        : std::optional<WorldCompositePublicInputCausalProfile>{found->second};
}

std::optional<WorldCompositePublicInputCausalProfile>
WorldEntityRegistry::FindScheduledInputCausalProfile(const WorldEntityId& owner,
    const std::uint32_t output) const noexcept {
    if (!owner.Valid() || output == 0U) return std::nullopt;
    std::lock_guard lock(mutex_);
    if (!manifestLoaded_ || !captureReady_.load(std::memory_order_acquire)) return std::nullopt;
    const auto found = scheduledInputCausalProfiles_.find(CompositeInputKey{owner, output});
    return found == scheduledInputCausalProfiles_.end() ? std::nullopt
        : std::optional<WorldCompositePublicInputCausalProfile>{found->second};
}

bool WorldEntityRegistry::HasCertifiedStateTransitionCarrier(
    const WorldEntityId& semanticTarget, const std::uint32_t semanticOperation) const noexcept {
    if (!semanticTarget.Valid() || semanticOperation == 0U) return false;
    std::lock_guard lock(mutex_);
    if (!manifestLoaded_ || !captureReady_.load(std::memory_order_acquire)) return false;
    return certifiedStateTransitionCarrierSemantics_.contains(
        CompositeInputKey{semanticTarget, semanticOperation});
}

bool WorldEntityRegistry::CouldBeCompositePublicInputShortGuid(
    const std::uint32_t publicInputShortGuid) const noexcept {
    if (publicInputShortGuid == 0U) return false;
    std::lock_guard lock(mutex_);
    if (!manifestLoaded_ || !captureReady_.load(std::memory_order_acquire)) {
        return false;
    }
    return compositePublicInputShortGuids_.contains(publicInputShortGuid);
}

bool WorldEntityRegistry::CouldBeCompositeInternalInputShortGuid(
    const std::uint32_t internalInputShortGuid) const noexcept {
    if (internalInputShortGuid == 0U) return false;
    std::lock_guard lock(mutex_);
    if (!manifestLoaded_ || !captureReady_.load(std::memory_order_acquire)) {
        return false;
    }
    return compositeInternalInputShortGuids_.contains(internalInputShortGuid);
}

std::optional<WorldCompositePublicInputDescriptor>
WorldEntityRegistry::FindCompositePublicInputForTarget(
    const WorldEntityId& ownerCompositeInstance,
    const std::uint32_t publicInputShortGuid) const noexcept {
    if (!ownerCompositeInstance.Valid() || publicInputShortGuid == 0U) {
        return std::nullopt;
    }
    std::lock_guard lock(mutex_);
    if (!manifestLoaded_ || !captureReady_.load(std::memory_order_acquire)) {
        return std::nullopt;
    }
    const CompositeInputKey key{ownerCompositeInstance, publicInputShortGuid};
    const auto allowed = compositeInputForward_.find(key);
    if (allowed == compositeInputForward_.end() || !allowed->second) {
        return std::nullopt;
    }
    const auto bridges = compositeInputForwardBridges_.find(key);
    if (bridges == compositeInputForwardBridges_.end() || bridges->second.empty()) {
        return std::nullopt;
    }
    return bridges->second.front();
}

std::optional<WorldCompositePublicInputDescriptor>
WorldEntityRegistry::FindCompositePublicInputForInterfaceTarget(
    const WorldEntityId& interfaceVariableEntity,
    const std::uint32_t publicInputShortGuid) const noexcept {
    if (!interfaceVariableEntity.Valid() || publicInputShortGuid == 0U) {
        return std::nullopt;
    }
    std::lock_guard lock(mutex_);
    if (!manifestLoaded_ || !captureReady_.load(std::memory_order_acquire)) {
        return std::nullopt;
    }
    const auto found = compositeInputInterfaceTargets_.find(
        CompositeInputKey{interfaceVariableEntity, publicInputShortGuid});
    if (found == compositeInputInterfaceTargets_.end() || found->second.ambiguous ||
        !found->second.descriptor.noTypedData) {
        return std::nullopt;
    }
    return found->second.descriptor;
}

std::vector<WorldCompositePublicInputDescriptor>
WorldEntityRegistry::FindCompositePublicInputBridges(
    const WorldEntityId& ownerCompositeInstance,
    const std::uint32_t publicInputShortGuid) const noexcept {
    if (!ownerCompositeInstance.Valid() || publicInputShortGuid == 0U) return {};
    std::lock_guard lock(mutex_);
    if (!manifestLoaded_ || !captureReady_.load(std::memory_order_acquire)) return {};

    const CompositeInputKey key{ownerCompositeInstance, publicInputShortGuid};
    const auto allowed = compositeInputForward_.find(key);
    if (allowed == compositeInputForward_.end() || !allowed->second) return {};
    const auto found = compositeInputForwardBridges_.find(key);
    if (found == compositeInputForwardBridges_.end() || found->second.empty()) return {};
    return found->second;
}

std::optional<WorldEntityId> WorldEntityRegistry::FindRuntimeWorldEntityId(
    const CathodeRuntimeKey& key) const noexcept {
    if (key.entityA == 0U) return std::nullopt;
    std::lock_guard lock(mutex_);
    if (!manifestLoaded_) return std::nullopt;
    const auto idFound = idsByRuntimeKey_.find(PackKey(key));
    if (idFound == idsByRuntimeKey_.end()) return std::nullopt;
    const auto entryFound = entries_.find(idFound->second);
    if (entryFound == entries_.end() || entryFound->second.domain != WorldDomain::World) {
        return std::nullopt;
    }
    return entryFound->second.id;
}

std::optional<CathodeRuntimeKey> WorldEntityRegistry::FindRuntimeWorldEntityKey(
    const WorldEntityId& id) const noexcept {
    if (!id.Valid()) return std::nullopt;
    std::lock_guard lock(mutex_);
    if (!manifestLoaded_) return std::nullopt;
    const auto found = entries_.find(id);
    if (found == entries_.end() || found->second.domain != WorldDomain::World ||
        found->second.key.entityA == 0U) {
        return std::nullopt;
    }
    return found->second.key;
}

std::optional<WorldEntityKind> WorldEntityRegistry::FindWorldEntityKind(
    const WorldEntityId& id) const noexcept {
    if (!id.Valid()) return std::nullopt;
    std::lock_guard lock(mutex_);
    if (!manifestLoaded_) return std::nullopt;
    const auto found = entries_.find(id);
    if (found == entries_.end()) return std::nullopt;
    return found->second.kind;
}

std::optional<WorldDomain> WorldEntityRegistry::FindWorldEntityDomain(
    const WorldEntityId& id) const noexcept {
    if (!id.Valid()) return std::nullopt;
    std::lock_guard lock(mutex_);
    if (!manifestLoaded_) return std::nullopt;
    const auto found = entries_.find(id);
    if (found == entries_.end()) return std::nullopt;
    return found->second.domain;
}

bool WorldEntityRegistry::IsPlayerMissionCompleteEndpoint(const WorldEntityId& id) const noexcept {
    std::lock_guard lock(mutex_);
    if (!manifestLoaded_) return false;
    const auto found = entries_.find(id);
    return found != entries_.end() &&
        (found->second.flags & WorldEntityFlagPlayerMissionComplete) != 0U;
}

std::optional<std::uint32_t> WorldEntityRegistry::FindWorldEntityPrimitiveType(
    const WorldEntityId& id) const noexcept {
    if (!id.Valid()) return std::nullopt;
    std::lock_guard lock(mutex_);
    if (!manifestLoaded_) return std::nullopt;
    const auto found = entries_.find(id);
    if (found == entries_.end()) return std::nullopt;
    return found->second.primitiveType;
}

std::vector<WorldEntityId> WorldEntityRegistry::FindWorldEntitiesOfPrimitiveType(
    const std::uint32_t primitiveType, const std::size_t limit) const noexcept {
    std::vector<WorldEntityId> result;
    try {
        std::lock_guard lock(mutex_);
        if (!manifestLoaded_ || primitiveType == 0U) return result;
        for (const auto& [id, entry] : entries_) {
            if (entry.primitiveType != primitiveType) continue;
            if (result.size() >= limit) break;
            result.push_back(id);
        }
    } catch (...) {
        result.clear();
    }
    return result;
}

std::optional<WorldEntityId> WorldEntityRegistry::IdentifyRuntimeEntity(
    const void* runtimeEntity,
    CathodeRuntimeKey* observedKey) const noexcept {
    CathodeRuntimeKey key{};
    if (!ExtractRuntimeKeyNative(runtimeEntity, key)) {
        return std::nullopt;
    }
    if (observedKey != nullptr) {
        *observedKey = key;
    }
    return FindWorldEntityId(key);
}

std::optional<WorldDomain> WorldEntityRegistry::IdentifyRuntimeEntityDomain(
    const void* runtimeEntity) const noexcept {
    CathodeRuntimeKey key{};
    if (!ExtractRuntimeKeyNative(runtimeEntity, key)) {
        return std::nullopt;
    }
    std::lock_guard lock(mutex_);
    if (!manifestLoaded_) return std::nullopt;
    const auto idFound = idsByRuntimeKey_.find(PackKey(key));
    if (idFound == idsByRuntimeKey_.end()) return std::nullopt;
    const auto entryFound = entries_.find(idFound->second);
    if (entryFound == entries_.end()) return std::nullopt;
    return entryFound->second.domain;
}

bool WorldEntityRegistry::TryExtractRuntimeKey(
    const void* runtimeEntity,
    CathodeRuntimeKey& key) const noexcept {
    return ExtractRuntimeKeyNative(runtimeEntity, key);
}

std::optional<WorldEntityId> WorldEntityRegistry::IdentifyRuntimeWorldEntity(
    const void* runtimeEntity,
    CathodeRuntimeKey* observedKey) const noexcept {
    CathodeRuntimeKey key{};
    if (!ExtractRuntimeKeyNative(runtimeEntity, key)) {
        return std::nullopt;
    }
    if (observedKey != nullptr) {
        *observedKey = key;
    }

    std::lock_guard lock(mutex_);
    if (!manifestLoaded_) {
        return std::nullopt;
    }
    const auto idFound = idsByRuntimeKey_.find(PackKey(key));
    if (idFound == idsByRuntimeKey_.end()) {
        return std::nullopt;
    }
    const auto entryFound = entries_.find(idFound->second);
    if (entryFound == entries_.end()) {
        return std::nullopt;
    }
    const auto& entry = entryFound->second;
    // Finding this entry from a live runtime key already proves runtime
    // addressability for the current process. Restrict only by domain here;
    // some retail World entities do not carry the offline convenience flag.
    if (entry.domain != WorldDomain::World) {
        return std::nullopt;
    }
    return entry.id;
}

WorldRegistryProbeSummary WorldEntityRegistry::ProbeWorldEnvironment(
    const std::uint32_t maximumEntities) noexcept {
    struct Candidate final {
        WorldEntityId id{};
        std::uint8_t priority{};
    };

    WorldRegistryProbeSummary summary{};

    // Enumerate the provider/context keys used by FUN_009236A0 for nested
    // composites. STATIC CONFIRMED: DAT_01754678 points to a registry whose
    // entries are {shared provider, hierarchy key B}. This is diagnostic-only.
    std::unordered_set<std::uint32_t> activeProviderKeys;
    activeProviderKeys.reserve(1024U);
    void* providerRegistry = nullptr;
    if (!ReadPointerSafely(providerRegistryRootAddress_, providerRegistry)) {
        ++summary.nativeFaults;
    } else if (providerRegistry != nullptr) {
        void* providerCollection = nullptr;
        const auto* registryBytes = static_cast<const std::byte*>(providerRegistry);
        if (!ReadPointerSafely(
                registryBytes + kProviderCollectionPointerOffset,
                providerCollection)) {
            ++summary.nativeFaults;
        } else if (providerCollection != nullptr) {
            std::uint32_t count = 0U;
            const auto* collectionBytes = static_cast<const std::byte*>(providerCollection);
            if (!ReadUint32Safely(
                    collectionBytes + kNativeCollectionCountOffset,
                    count) || count > kMaximumRuntimeEntities) {
                ++summary.nativeFaults;
            } else {
                summary.activeProviders = count;
                void* data = nullptr;
                if (count != 0U &&
                    (!ReadPointerSafely(
                         collectionBytes + kNativeCollectionDataOffset,
                         data) || data == nullptr)) {
                    ++summary.nativeFaults;
                } else if (data != nullptr) {
                    const auto* dataBytes = static_cast<const std::byte*>(data);
                    for (std::uint32_t index = 0U; index < count; ++index) {
                        std::uint32_t key = 0U;
                        if (!ReadUint32Safely(
                                dataBytes + static_cast<std::size_t>(index) *
                                    kProviderEntryStride + kProviderEntryKeyOffset,
                                key)) {
                            ++summary.nativeFaults;
                            continue;
                        }
                        if (key != 0U) {
                            activeProviderKeys.insert(key);
                        }
                    }
                }
            }
        }
    }

    std::vector<Candidate> candidates;
    std::unordered_set<std::uint32_t> matchedProviderContexts;
    {
        std::lock_guard lock(mutex_);
        if (!manifestLoaded_) {
            return summary;
        }
        candidates.reserve(entries_.size());
        matchedProviderContexts.reserve(activeProviderKeys.size());
        constexpr std::uint16_t kRelevantFlags =
            WorldEntityFlagEnvironmentPrimitive |
            WorldEntityFlagZonePrimitive |
            WorldEntityFlagCompositeInstance;
        for (const auto& [id, entry] : entries_) {
            if (entry.domain != WorldDomain::World ||
                (entry.flags & kRelevantFlags) == 0U) {
                continue;
            }
            ++summary.eligible;
            if (entry.key.entityB != 0U &&
                activeProviderKeys.contains(entry.key.entityB)) {
                ++summary.activeEligible;
                matchedProviderContexts.insert(entry.key.entityB);
                candidates.push_back(Candidate{id, 0U});
            } else if (entry.key.entityB == 0U) {
                ++summary.activeEligible;
                candidates.push_back(Candidate{id, 1U});
            }
        }
    }
    summary.manifestActiveProviderContexts = matchedProviderContexts.size();

    std::sort(
        candidates.begin(),
        candidates.end(),
        [](const Candidate& left, const Candidate& right) noexcept {
            if (left.priority != right.priority) {
                return left.priority < right.priority;
            }
            if (left.id.high != right.id.high) {
                return left.id.high < right.id.high;
            }
            return left.id.low < right.id.low;
        });

    const auto probeCount = std::min<std::size_t>(
        candidates.size(),
        static_cast<std::size_t>(maximumEntities));
    summary.probed = probeCount;
    for (std::size_t index = 0; index < probeCount; ++index) {
        const auto resolved = Resolve(candidates[index].id);
        switch (resolved.status) {
        case WorldRegistryResolveStatus::Resolved:
            ++summary.resolved;
            break;
        case WorldRegistryResolveStatus::NativeFault:
            ++summary.nativeFaults;
            break;
        default:
            ++summary.pending;
            break;
        }
    }

    // Bounded live control: extract raw identities from manager-owned objects,
    // then ask the *generic* retail resolver for the corresponding execution
    // handle. The returned handle is intentionally NOT re-identified with
    // FUN_008C3360: retail call sites treat it as an execution interface and it
    // may differ from the raw identity-bearing object.
    void* manager = nullptr;
    if (!ReadPointerSafely(managerRootAddress_, manager)) {
        ++summary.controlNativeFaults;
        return summary;
    }
    if (manager == nullptr) {
        return summary;
    }

    std::unordered_set<std::uint64_t> seenRuntimeKeys;
    seenRuntimeKeys.reserve(4096U);
    auto inspectCollection = [&](const std::size_t managerOffset,
                                 std::uint64_t& rawEntryCounter) noexcept {
        void* collection = nullptr;
        const auto* managerBytes = static_cast<const std::byte*>(manager);
        if (!ReadPointerSafely(managerBytes + managerOffset, collection)) {
            ++summary.controlNativeFaults;
            return;
        }
        if (collection == nullptr) {
            return;
        }

        std::uint32_t count = 0U;
        const auto* collectionBytes = static_cast<const std::byte*>(collection);
        if (!ReadUint32Safely(collectionBytes + kNativeCollectionCountOffset, count) ||
            count > kMaximumRuntimeEntities) {
            ++summary.controlNativeFaults;
            return;
        }
        rawEntryCounter += count;
        if (count == 0U) {
            return;
        }

        void* data = nullptr;
        if (!ReadPointerSafely(collectionBytes + kNativeCollectionDataOffset, data) ||
            data == nullptr) {
            ++summary.controlNativeFaults;
            return;
        }
        const auto* dataBytes = static_cast<const std::byte*>(data);
        for (std::uint32_t index = 0U; index < count; ++index) {
            void* shared = nullptr;
            if (!ReadPointerSafely(
                    dataBytes + static_cast<std::size_t>(index) * sizeof(void*),
                    shared)) {
                ++summary.controlNativeFaults;
                continue;
            }
            if (shared == nullptr) {
                continue;
            }

            void* rawEntity = nullptr;
            if (!ReadPointerSafely(
                    static_cast<const std::byte*>(shared) + kSharedObjectOffset,
                    rawEntity) || rawEntity == nullptr) {
                ++summary.controlNativeFaults;
                continue;
            }

            CathodeRuntimeKey key{};
            if (!ExtractRuntimeKeyNative(rawEntity, key) || key.entityA == 0U) {
                continue;
            }
            const auto packed = PackKey(key);
            if (!seenRuntimeKeys.insert(packed).second) {
                continue;
            }
            ++summary.runtimeUniqueKeys;
            if (FindWorldEntityId(key).has_value()) {
                ++summary.runtimeManifestExact;
            }

            if (summary.controlResolverChecks >= kControlResolverBudget) {
                continue;
            }
            ++summary.controlResolverChecks;
            bool nativeFault = false;
            auto lease = ResolveNative(key, nativeFault);
            if (lease) {
                ++summary.controlResolverResolved;
            } else if (nativeFault) {
                ++summary.controlNativeFaults;
            } else {
                ++summary.controlResolverPending;
            }
        }
    };

    inspectCollection(kManagerLoadedCollectionOffset, summary.runtimeLoadedEntries);
    inspectCollection(kManagerRootCollectionOffset, summary.runtimeRootEntries);
    return summary;
}

WorldOpaqueStateSnapshot WorldEntityRegistry::CaptureOpaqueRuntimeStateSnapshot(
    const std::uint32_t maximumEntries) const noexcept {
    WorldOpaqueStateSnapshot result{};
    const auto started = static_cast<std::uint64_t>(GetTickCount64());
    {
        std::lock_guard lock(mutex_);
        result.levelGeneration = manifestGeneration_;
    }

    const auto limit = std::max<std::uint32_t>(1U, maximumEntries);
    result.entries.reserve(std::min<std::uint32_t>(limit, 16'384U));

    auto captureRawEntity = [&](const WorldOpaqueStateSource source,
                                const std::uint32_t sourceIndex,
                                const std::uint32_t providerContextKey,
                                void* rawEntity) noexcept {
        ++result.inspectedEntries;
        if (rawEntity == nullptr) {
            ++result.objectReadFailures;
            return;
        }
        if (result.entries.size() >= limit) {
            ++result.capacityDrops;
            return;
        }

        WorldOpaqueStateEntry entry{};
        entry.source = source;
        entry.sourceIndex = sourceIndex;
        entry.providerContextKey = providerContextKey;

        CathodeRuntimeKey runtimeKey{};
        if (ExtractRuntimeKeyNative(rawEntity, runtimeKey) && runtimeKey.entityA != 0U) {
            entry.runtimeKey = runtimeKey;
            entry.runtimeKeyValid = true;
            ++result.runtimeKeyEntries;
            const auto worldId = FindRuntimeWorldEntityId(runtimeKey);
            if (worldId.has_value()) {
                entry.worldEntity = *worldId;
                entry.worldEntityValid = true;
                ++result.worldMappedEntries;
            }
        }

        const auto objectBytes = CopyOpaquePrefix(
            entry.objectBytes.data(),
            entry.objectBytes.size(),
            rawEntity);
        entry.objectReadableBytes = static_cast<std::uint16_t>(
            std::min<std::size_t>(objectBytes, std::numeric_limits<std::uint16_t>::max()));
        if (objectBytes == 0U) {
            ++result.objectReadFailures;
            return;
        }
        if (objectBytes < entry.objectBytes.size()) {
            ++result.objectPartialReads;
        }
        entry.objectHash = HashOpaqueBytes(entry.objectBytes.data(), objectBytes);

        const auto wordCount = std::min<std::size_t>(
            objectBytes / sizeof(std::uint32_t), 32U);
        for (std::size_t wordIndex = 0U; wordIndex < wordCount; ++wordIndex) {
            std::uint32_t word = 0U;
            std::memcpy(
                &word,
                entry.objectBytes.data() + wordIndex * sizeof(std::uint32_t),
                sizeof(word));
            if (!LooksLikePointerShapedWord(word)) {
                continue;
            }
            entry.objectPointerWordMask |= (1U << static_cast<std::uint32_t>(wordIndex));
            if (entry.pointerProbeCount >= entry.pointerProbes.size() ||
                !IsProbeableNonImagePointer(word)) {
                continue;
            }

            auto& probe = entry.pointerProbes[entry.pointerProbeCount];
            probe.ownerOffset = static_cast<std::uint16_t>(
                wordIndex * sizeof(std::uint32_t));
            const auto* pointee = reinterpret_cast<const void*>(
                static_cast<std::uintptr_t>(word));
            const auto pointeeBytes = CopyOpaquePrefix(
                probe.bytes.data(),
                probe.bytes.size(),
                pointee);
            probe.readableBytes = static_cast<std::uint16_t>(
                std::min<std::size_t>(
                    pointeeBytes,
                    std::numeric_limits<std::uint16_t>::max()));
            if (pointeeBytes == 0U) {
                ++result.pointerProbeReadFailures;
                continue;
            }
            if (pointeeBytes < probe.bytes.size()) {
                ++result.pointerProbePartialReads;
            }
            probe.hash = HashOpaqueBytes(probe.bytes.data(), pointeeBytes);
            const auto pointeeWordCount = std::min<std::size_t>(
                pointeeBytes / sizeof(std::uint32_t), 16U);
            for (std::size_t pointeeWord = 0U;
                 pointeeWord < pointeeWordCount;
                 ++pointeeWord) {
                std::uint32_t pointeeValue = 0U;
                std::memcpy(
                    &pointeeValue,
                    probe.bytes.data() + pointeeWord * sizeof(std::uint32_t),
                    sizeof(pointeeValue));
                if (LooksLikePointerShapedWord(pointeeValue)) {
                    probe.pointerWordMask |= static_cast<std::uint16_t>(
                        1U << static_cast<std::uint32_t>(pointeeWord));
                }
            }
            ++entry.pointerProbeCount;
        }

        ++result.capturedEntries;
        result.entries.push_back(std::move(entry));
    };

    // Provider registry: STATIC CONFIRMED by the existing v12.3 runtime probe
    // as an array of {shared provider, hierarchy key B}. Fix3.23 captures every
    // active entry rather than sampling the first N manifest entities.
    void* providerRegistry = nullptr;
    if (!ReadPointerSafely(providerRegistryRootAddress_, providerRegistry)) {
        ++result.objectReadFailures;
    } else if (providerRegistry != nullptr) {
        void* providerCollection = nullptr;
        const auto* registryBytes = static_cast<const std::byte*>(providerRegistry);
        if (!ReadPointerSafely(
                registryBytes + kProviderCollectionPointerOffset,
                providerCollection)) {
            ++result.objectReadFailures;
        } else if (providerCollection != nullptr) {
            std::uint32_t count = 0U;
            const auto* collectionBytes = static_cast<const std::byte*>(providerCollection);
            if (!ReadUint32Safely(
                    collectionBytes + kNativeCollectionCountOffset,
                    count) || count > kMaximumRuntimeEntities) {
                ++result.objectReadFailures;
            } else {
                result.providerEntries = count;
                void* data = nullptr;
                if (count != 0U &&
                    (!ReadPointerSafely(
                         collectionBytes + kNativeCollectionDataOffset,
                         data) || data == nullptr)) {
                    ++result.objectReadFailures;
                } else if (data != nullptr) {
                    const auto* dataBytes = static_cast<const std::byte*>(data);
                    for (std::uint32_t index = 0U; index < count; ++index) {
                        const auto* entryBytes =
                            dataBytes + static_cast<std::size_t>(index) * kProviderEntryStride;
                        void* shared = nullptr;
                        std::uint32_t contextKey = 0U;
                        if (!ReadPointerSafely(entryBytes, shared) ||
                            !ReadUint32Safely(
                                entryBytes + kProviderEntryKeyOffset,
                                contextKey)) {
                            ++result.objectReadFailures;
                            continue;
                        }
                        if (shared == nullptr) {
                            continue;
                        }
                        void* rawEntity = nullptr;
                        if (!ReadPointerSafely(
                                static_cast<const std::byte*>(shared) + kSharedObjectOffset,
                                rawEntity)) {
                            ++result.objectReadFailures;
                            continue;
                        }
                        captureRawEntity(
                            WorldOpaqueStateSource::ProviderRegistry,
                            index,
                            contextKey,
                            rawEntity);
                    }
                }
            }
        }
    }

    // Manager collections are included as a second broad surface because the
    // existing v12.3 probe proved that they contain live runtime identities
    // outside the provider registry's nested-composite path.
    void* manager = nullptr;
    if (ReadPointerSafely(managerRootAddress_, manager) && manager != nullptr) {
        auto captureManagerCollection = [&](const std::size_t managerOffset,
                                            const WorldOpaqueStateSource source,
                                            std::uint64_t& entryCounter) noexcept {
            void* collection = nullptr;
            if (!ReadPointerSafely(
                    static_cast<const std::byte*>(manager) + managerOffset,
                    collection) || collection == nullptr) {
                return;
            }
            std::uint32_t count = 0U;
            const auto* collectionBytes = static_cast<const std::byte*>(collection);
            if (!ReadUint32Safely(
                    collectionBytes + kNativeCollectionCountOffset,
                    count) || count > kMaximumRuntimeEntities) {
                ++result.objectReadFailures;
                return;
            }
            entryCounter = count;
            if (count == 0U) return;
            void* data = nullptr;
            if (!ReadPointerSafely(
                    collectionBytes + kNativeCollectionDataOffset,
                    data) || data == nullptr) {
                ++result.objectReadFailures;
                return;
            }
            const auto* dataBytes = static_cast<const std::byte*>(data);
            for (std::uint32_t index = 0U; index < count; ++index) {
                void* shared = nullptr;
                if (!ReadPointerSafely(
                        dataBytes + static_cast<std::size_t>(index) * sizeof(void*),
                        shared) || shared == nullptr) {
                    continue;
                }
                void* rawEntity = nullptr;
                if (!ReadPointerSafely(
                        static_cast<const std::byte*>(shared) + kSharedObjectOffset,
                        rawEntity)) {
                    ++result.objectReadFailures;
                    continue;
                }
                captureRawEntity(source, index, 0U, rawEntity);
            }
        };

        captureManagerCollection(
            kManagerLoadedCollectionOffset,
            WorldOpaqueStateSource::ManagerLoadedCollection,
            result.managerLoadedEntries);
        captureManagerCollection(
            kManagerRootCollectionOffset,
            WorldOpaqueStateSource::ManagerRootCollection,
            result.managerRootEntries);
    } else {
        ++result.objectReadFailures;
    }

    result.elapsedMs = static_cast<std::uint64_t>(GetTickCount64()) - started;
    return result;
}


void WorldEntityRegistry::BeginOpaqueRuntimeStateCapture(
    WorldOpaqueStateCaptureCursor& cursor,
    const std::uint32_t maximumEntries) const noexcept {
    cursor = {};
    {
        std::lock_guard lock(mutex_);
        cursor.snapshot.levelGeneration = manifestGeneration_;
    }
    cursor.maximumEntries = std::max<std::uint32_t>(1U, maximumEntries);
    cursor.startedTickMs = static_cast<std::uint64_t>(GetTickCount64());
    cursor.phase = WorldOpaqueStateCapturePhase::ProviderRegistry;
    cursor.initialized = true;
    cursor.complete = false;
    cursor.snapshot.entries.reserve(
        std::min<std::uint32_t>(cursor.maximumEntries, 16'384U));
}

bool WorldEntityRegistry::ContinueOpaqueRuntimeStateCapture(
    WorldOpaqueStateCaptureCursor& cursor,
    const std::uint32_t maximumSlots,
    const std::uint32_t maximumStepMs) const noexcept {
    if (!cursor.initialized) {
        BeginOpaqueRuntimeStateCapture(cursor, kWorldOpaqueStateMaximumEntries);
    }
    if (cursor.complete) {
        return true;
    }

    {
        std::lock_guard lock(mutex_);
        if (manifestGeneration_ != cursor.snapshot.levelGeneration) {
            cursor.phase = WorldOpaqueStateCapturePhase::GenerationInvalidated;
            cursor.complete = true;
            cursor.snapshot.elapsedMs =
                static_cast<std::uint64_t>(GetTickCount64()) - cursor.startedTickMs;
            return true;
        }
    }

    const auto slotBudget = std::max<std::uint32_t>(1U, maximumSlots);
    const auto startedStep = static_cast<std::uint64_t>(GetTickCount64());
    std::uint32_t processedSlots = 0U;

    auto stepBudgetReached = [&]() noexcept {
        if (processedSlots >= slotBudget) {
            return true;
        }
        if (processedSlots == 0U || maximumStepMs == 0U) {
            return false;
        }
        return static_cast<std::uint64_t>(GetTickCount64()) - startedStep >=
            static_cast<std::uint64_t>(maximumStepMs);
    };

    auto captureRawEntity = [&](const WorldOpaqueStateSource source,
                                const std::uint32_t sourceIndex,
                                const std::uint32_t providerContextKey,
                                void* rawEntity) noexcept {
        auto& result = cursor.snapshot;
        ++result.inspectedEntries;
        if (rawEntity == nullptr) {
            ++result.objectReadFailures;
            return;
        }
        if (result.entries.size() >= cursor.maximumEntries) {
            ++result.capacityDrops;
            return;
        }

        WorldOpaqueStateEntry entry{};
        entry.source = source;
        entry.sourceIndex = sourceIndex;
        entry.providerContextKey = providerContextKey;

        CathodeRuntimeKey runtimeKey{};
        if (ExtractRuntimeKeyNative(rawEntity, runtimeKey) && runtimeKey.entityA != 0U) {
            entry.runtimeKey = runtimeKey;
            entry.runtimeKeyValid = true;
            ++result.runtimeKeyEntries;
            const auto worldId = FindRuntimeWorldEntityId(runtimeKey);
            if (worldId.has_value()) {
                entry.worldEntity = *worldId;
                entry.worldEntityValid = true;
                ++result.worldMappedEntries;
            }
        }

        const auto objectBytes = CopyOpaquePrefix(
            entry.objectBytes.data(),
            entry.objectBytes.size(),
            rawEntity);
        entry.objectReadableBytes = static_cast<std::uint16_t>(
            std::min<std::size_t>(
                objectBytes,
                std::numeric_limits<std::uint16_t>::max()));
        if (objectBytes == 0U) {
            ++result.objectReadFailures;
            return;
        }
        if (objectBytes < entry.objectBytes.size()) {
            ++result.objectPartialReads;
        }
        entry.objectHash = HashOpaqueBytes(entry.objectBytes.data(), objectBytes);

        const auto wordCount = std::min<std::size_t>(
            objectBytes / sizeof(std::uint32_t), 32U);
        for (std::size_t wordIndex = 0U; wordIndex < wordCount; ++wordIndex) {
            std::uint32_t word = 0U;
            std::memcpy(
                &word,
                entry.objectBytes.data() + wordIndex * sizeof(std::uint32_t),
                sizeof(word));
            if (!LooksLikePointerShapedWord(word)) {
                continue;
            }
            entry.objectPointerWordMask |=
                (1U << static_cast<std::uint32_t>(wordIndex));
            if (entry.pointerProbeCount >= entry.pointerProbes.size() ||
                !IsProbeableNonImagePointer(word)) {
                continue;
            }

            auto& probe = entry.pointerProbes[entry.pointerProbeCount];
            probe.ownerOffset = static_cast<std::uint16_t>(
                wordIndex * sizeof(std::uint32_t));
            const auto* pointee = reinterpret_cast<const void*>(
                static_cast<std::uintptr_t>(word));
            const auto pointeeBytes = CopyOpaquePrefix(
                probe.bytes.data(),
                probe.bytes.size(),
                pointee);
            probe.readableBytes = static_cast<std::uint16_t>(
                std::min<std::size_t>(
                    pointeeBytes,
                    std::numeric_limits<std::uint16_t>::max()));
            if (pointeeBytes == 0U) {
                ++result.pointerProbeReadFailures;
                continue;
            }
            if (pointeeBytes < probe.bytes.size()) {
                ++result.pointerProbePartialReads;
            }
            probe.hash = HashOpaqueBytes(probe.bytes.data(), pointeeBytes);
            const auto pointeeWordCount = std::min<std::size_t>(
                pointeeBytes / sizeof(std::uint32_t), 16U);
            for (std::size_t pointeeWord = 0U;
                 pointeeWord < pointeeWordCount;
                 ++pointeeWord) {
                std::uint32_t pointeeValue = 0U;
                std::memcpy(
                    &pointeeValue,
                    probe.bytes.data() + pointeeWord * sizeof(std::uint32_t),
                    sizeof(pointeeValue));
                if (LooksLikePointerShapedWord(pointeeValue)) {
                    probe.pointerWordMask |= static_cast<std::uint16_t>(
                        1U << static_cast<std::uint32_t>(pointeeWord));
                }
            }
            ++entry.pointerProbeCount;
        }

        ++result.capturedEntries;
        result.entries.push_back(std::move(entry));
    };

    auto processProviderSlot = [&]() noexcept -> bool {
        void* providerRegistry = nullptr;
        if (!ReadPointerSafely(providerRegistryRootAddress_, providerRegistry) ||
            providerRegistry == nullptr) {
            ++cursor.snapshot.objectReadFailures;
            cursor.phase = WorldOpaqueStateCapturePhase::ManagerLoadedCollection;
            return false;
        }
        void* providerCollection = nullptr;
        const auto* registryBytes = static_cast<const std::byte*>(providerRegistry);
        if (!ReadPointerSafely(
                registryBytes + kProviderCollectionPointerOffset,
                providerCollection) ||
            providerCollection == nullptr) {
            ++cursor.snapshot.objectReadFailures;
            cursor.phase = WorldOpaqueStateCapturePhase::ManagerLoadedCollection;
            return false;
        }
        std::uint32_t count = 0U;
        const auto* collectionBytes = static_cast<const std::byte*>(providerCollection);
        if (!ReadUint32Safely(
                collectionBytes + kNativeCollectionCountOffset,
                count) ||
            count > kMaximumRuntimeEntities) {
            ++cursor.snapshot.objectReadFailures;
            cursor.phase = WorldOpaqueStateCapturePhase::ManagerLoadedCollection;
            return false;
        }
        cursor.snapshot.providerEntries = count;
        if (cursor.providerIndex >= count) {
            cursor.phase = WorldOpaqueStateCapturePhase::ManagerLoadedCollection;
            return false;
        }
        void* data = nullptr;
        if (!ReadPointerSafely(
                collectionBytes + kNativeCollectionDataOffset,
                data) ||
            (count != 0U && data == nullptr)) {
            ++cursor.snapshot.objectReadFailures;
            cursor.phase = WorldOpaqueStateCapturePhase::ManagerLoadedCollection;
            return false;
        }

        const auto index = cursor.providerIndex++;
        ++processedSlots;
        if (data == nullptr) {
            return true;
        }
        const auto* entryBytes = static_cast<const std::byte*>(data) +
            static_cast<std::size_t>(index) * kProviderEntryStride;
        void* shared = nullptr;
        std::uint32_t contextKey = 0U;
        if (!ReadPointerSafely(entryBytes, shared) ||
            !ReadUint32Safely(
                entryBytes + kProviderEntryKeyOffset,
                contextKey)) {
            ++cursor.snapshot.objectReadFailures;
            return true;
        }
        if (shared == nullptr) {
            return true;
        }
        void* rawEntity = nullptr;
        if (!ReadPointerSafely(
                static_cast<const std::byte*>(shared) + kSharedObjectOffset,
                rawEntity)) {
            ++cursor.snapshot.objectReadFailures;
            return true;
        }
        captureRawEntity(
            WorldOpaqueStateSource::ProviderRegistry,
            index,
            contextKey,
            rawEntity);
        return true;
    };

    auto processManagerSlot = [&](const std::size_t managerOffset,
                                  const WorldOpaqueStateSource source,
                                  std::uint32_t& currentIndex,
                                  std::uint64_t& entryCounter,
                                  const WorldOpaqueStateCapturePhase nextPhase) noexcept -> bool {
        void* manager = nullptr;
        if (!ReadPointerSafely(managerRootAddress_, manager) || manager == nullptr) {
            ++cursor.snapshot.objectReadFailures;
            cursor.phase = nextPhase;
            return false;
        }
        void* collection = nullptr;
        if (!ReadPointerSafely(
                static_cast<const std::byte*>(manager) + managerOffset,
                collection) ||
            collection == nullptr) {
            cursor.phase = nextPhase;
            return false;
        }
        std::uint32_t count = 0U;
        const auto* collectionBytes = static_cast<const std::byte*>(collection);
        if (!ReadUint32Safely(
                collectionBytes + kNativeCollectionCountOffset,
                count) ||
            count > kMaximumRuntimeEntities) {
            ++cursor.snapshot.objectReadFailures;
            cursor.phase = nextPhase;
            return false;
        }
        entryCounter = count;
        if (currentIndex >= count) {
            cursor.phase = nextPhase;
            return false;
        }
        void* data = nullptr;
        if (!ReadPointerSafely(
                collectionBytes + kNativeCollectionDataOffset,
                data) ||
            (count != 0U && data == nullptr)) {
            ++cursor.snapshot.objectReadFailures;
            cursor.phase = nextPhase;
            return false;
        }

        const auto index = currentIndex++;
        ++processedSlots;
        if (data == nullptr) {
            return true;
        }
        void* shared = nullptr;
        if (!ReadPointerSafely(
                static_cast<const std::byte*>(data) +
                    static_cast<std::size_t>(index) * sizeof(void*),
                shared) ||
            shared == nullptr) {
            return true;
        }
        void* rawEntity = nullptr;
        if (!ReadPointerSafely(
                static_cast<const std::byte*>(shared) + kSharedObjectOffset,
                rawEntity)) {
            ++cursor.snapshot.objectReadFailures;
            return true;
        }
        captureRawEntity(source, index, 0U, rawEntity);
        return true;
    };

    while (!cursor.complete && !stepBudgetReached()) {
        switch (cursor.phase) {
        case WorldOpaqueStateCapturePhase::ProviderRegistry:
            (void)processProviderSlot();
            break;
        case WorldOpaqueStateCapturePhase::ManagerLoadedCollection:
            (void)processManagerSlot(
                kManagerLoadedCollectionOffset,
                WorldOpaqueStateSource::ManagerLoadedCollection,
                cursor.managerLoadedIndex,
                cursor.snapshot.managerLoadedEntries,
                WorldOpaqueStateCapturePhase::ManagerRootCollection);
            break;
        case WorldOpaqueStateCapturePhase::ManagerRootCollection:
            (void)processManagerSlot(
                kManagerRootCollectionOffset,
                WorldOpaqueStateSource::ManagerRootCollection,
                cursor.managerRootIndex,
                cursor.snapshot.managerRootEntries,
                WorldOpaqueStateCapturePhase::Complete);
            break;
        case WorldOpaqueStateCapturePhase::Complete:
            cursor.complete = true;
            break;
        case WorldOpaqueStateCapturePhase::GenerationInvalidated:
            cursor.complete = true;
            break;
        }
    }

    if (cursor.phase == WorldOpaqueStateCapturePhase::Complete ||
        cursor.phase == WorldOpaqueStateCapturePhase::GenerationInvalidated) {
        cursor.complete = true;
        cursor.snapshot.elapsedMs =
            static_cast<std::uint64_t>(GetTickCount64()) - cursor.startedTickMs;
    }
    return cursor.complete;
}

WorldRegistryContext WorldEntityRegistry::ReplicationContext() const noexcept {
    std::lock_guard lock(mutex_);
    return {manifestLoaded_ && captureReady_.load(std::memory_order_acquire),
            manifestGeneration_, mapContentId_};
}

WorldRegistryDiagnostics WorldEntityRegistry::DiagnosticsSnapshot() const noexcept {
    std::lock_guard lock(mutex_);
    WorldRegistryDiagnostics result{};
    result.initialized = initialized_;
    result.manifestLoaded = manifestLoaded_;
    result.manifestEntities = entries_.size();
    result.resolveRequests = resolveRequests_;
    result.resolveHits = resolveHits_;
    result.pendingMisses = pendingMisses_;
    result.nativeFaults = nativeFaults_;
    result.invalidations = invalidations_;
    for (const auto& [zoneId, loaded] : loadedZones_) {
        (void)zoneId;
        if (loaded) {
            ++result.knownLoadedZones;
        } else {
            ++result.knownUnloadedZones;
        }
    }
    result.levelGeneration = manifestGeneration_;
    result.mapContentId = mapContentId_;
    return result;
}

void WorldEntityRegistry::NotifyZoneUnloaded(const std::uint32_t zoneId) noexcept {
    if (zoneId == 0U) {
        return;
    }
    std::lock_guard lock(mutex_);
    loadedZones_[zoneId] = false;
    ++invalidations_;
}

void WorldEntityRegistry::NotifyZoneLoaded(const std::uint32_t zoneId) noexcept {
    if (zoneId == 0U) {
        return;
    }
    std::lock_guard lock(mutex_);
    loadedZones_[zoneId] = true;
}

void WorldEntityRegistry::InvalidateAll() noexcept {
    captureReady_.store(false, std::memory_order_release);
    std::lock_guard lock(mutex_);
    loadedZones_.clear();
    // Fix3.24 public-interface mappings are generation-specific because their
    // WorldEntityIds include MapContentId + instance path. Never leave a stale
    // mapping eligible across an invalidation.
    compositeInputDirect_.clear();
    compositeInputReverse_.clear();
    compositeInputRuntimeReverse_.clear();
    compositeInputOwnerScopeRuntimeReverse_.clear();
    compositeInputCanonicalAnchors_.clear();
    compositeInputInterfaceTargets_.clear();
    compositeInputForward_.clear();
    compositePublicRootAmbiguous_.clear();
    compositePublicRoots_.Clear();
    compositeInputForwardBridges_.clear();
    compositeInputCausalProfiles_.clear();
    emittedOutputCausalProfiles_.clear();
    scheduledInputCausalProfiles_.clear();
    certifiedStateTransitionCarrierSemantics_.clear();
    compositePublicInputShortGuids_.clear();
    compositeInternalInputShortGuids_.clear();
    ++invalidations_;
}

WorldEntityLease WorldEntityRegistry::ResolveRuntimeReference(
    const CathodeRuntimeKey& key,
    bool& nativeFault) const noexcept {
    nativeFault = false;
    if (!RuntimeCaptureReady() || key.entityA == 0U) {
        return {};
    }
    return ResolveNative(key, nativeFault);
}

WorldEntityLease WorldEntityRegistry::ResolveNative(
    const CathodeRuntimeKey& key,
    bool& nativeFault) const noexcept {
    nativeFault = false;
    if (key.entityA == 0U) {
        return {};
    }

    void* manager = nullptr;
    if (!ReadPointerSafely(managerRootAddress_, manager)) {
        nativeFault = true;
        return {};
    }
    if (manager == nullptr) {
        return {};
    }

    const auto resolve = reinterpret_cast<ResolveEntityFn>(
        const_cast<void*>(resolveEntityAddress_));
    void* shared = nullptr;
    if (!InvokeResolveSafely(resolve, manager, &shared, &key)) {
        nativeFault = true;
        return {};
    }
    if (shared == nullptr) {
        return {};
    }

    void* executionObject = nullptr;
    if (!ReadPointerSafely(
            static_cast<const std::byte*>(shared) + kSharedObjectOffset,
            executionObject) || executionObject == nullptr) {
        const auto assign = reinterpret_cast<AssignEntityRefFn>(
            const_cast<void*>(assignAddress_));
        (void)InvokeAssignNullSafely(assign, &shared);
        nativeFault = true;
        return {};
    }

    return WorldEntityLease(shared, executionObject, assignAddress_);
}

bool WorldEntityRegistry::ExtractRuntimeKeyNative(
    const void* runtimeEntity,
    CathodeRuntimeKey& key) const noexcept {
    key = {};
    if (runtimeEntity == nullptr || extractRuntimeKeyAddress_ == nullptr) {
        return false;
    }
    const auto extract = reinterpret_cast<ExtractRuntimeKeyFn>(
        const_cast<void*>(extractRuntimeKeyAddress_));
    if (!InvokeExtractRuntimeKeySafely(extract, runtimeEntity, &key)) {
        key = {};
        return false;
    }
    return key.entityA != 0U;
}

} // namespace aimp
