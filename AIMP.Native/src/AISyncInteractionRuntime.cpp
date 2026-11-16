#include "aimp/AISyncInteractionRuntime.h"
#include "aimp/AISync.h"

#include <limits>

namespace aimp::ai_sync::interactions {
namespace {
template<class T> bool Read(const Memory& memory, const std::uintptr_t address, T& value) noexcept {
    return memory.read != nullptr && address >= 0x10000U &&
        address <= 0xFFFFFFFFULL - sizeof(T) && memory.read(memory.context, address, &value, sizeof(T));
}
bool Pointer(const Memory& memory, const std::uintptr_t address, std::uintptr_t& value) noexcept {
    std::uint32_t word{};
    if (!Read(memory, address, word)) return false;
    value = word;
    return true;
}
Participant Source(const State& state) noexcept {
    return {0U, state.entityA, state.entityB, state.birth, state.characterClass};
}
bool SamePair(const Command& command, const State& state) noexcept {
    return command.source == Source(state) && command.pairRevision == state.interactionPairRevision;
}
Check PendingSource(const Memory& memory,const LocalParticipant& source,const PairState& admitted,
    const std::uintptr_t expected) noexcept {
    if(!expected||admitted.sourceCycle!=expected||admitted.source!=source.character||
        admitted.sourceRuntimeId!=source.runtimeId)return Check::Rejected;
    std::uintptr_t current{},owner{},type{};std::uint32_t runtime{};std::uint8_t serial{},kind{},flags{};
    if(!Read(memory,source.character+0x50U,runtime)||!Pointer(memory,source.character+0x36CU,current)||
        !Pointer(memory,expected,type)||!Pointer(memory,expected+4U,owner)||
        !Read(memory,expected+0x19U,serial)||!Read(memory,expected+0x1AU,kind)||
        !Read(memory,expected+8U,flags))return Check::Unknown;
    if(runtime!=source.runtimeId||current!=expected||owner!=source.character||
        type!=memory.imageBase+0xEAE5D0U||serial!=admitted.sourceSerial||kind!=0x4EU||(flags&6U)!=0U)
        return Check::Rejected;
    // The admitted native cycle owns its initialization too. E7 and a graph
    // may be published later; their absence cannot authorize scheduler Idle.
    if(!Pointer(memory,source.character+0x36CU,current)||!Read(memory,expected+0x19U,serial)||
        !Read(memory,expected+8U,flags))return Check::Unknown;
    return current==expected&&serial==admitted.sourceSerial&&(flags&6U)==0U?Check::Ready:Check::Rejected;
}
Check Capture(const State& state, Command& command) noexcept {
    const bool player=(state.interactionFlags&InteractionTargetIsParticipant)!=0U;
    // 101x: which field refused the carrier (516: an unexplained 101 left the
    // Client Alien standing in front of a player the Host had already killed).
    const auto refuse=[](std::uint32_t site) noexcept { RefusalSite()=site; return Check::Rejected; };
    if (state.version != State{}.version || state.birth == 0U || state.characterClass == 0U ||
        (state.entityA == 0U && state.entityB == 0U) || state.revision == 0U) return refuse(1011U);
    if ((state.stateFlags & InteractionValid) == 0U || state.interactionRevision == 0U) return refuse(1012U);
    if ((state.interactionFlags & (InteractionActorIsSource | InteractionPairValid)) !=
            (InteractionActorIsSource | InteractionPairValid)) return refuse(1013U);
    if (player) {
        if (state.interactionTargetPeer==0U) return refuse(1014U);
        if (state.interactionParticipantIncarnation==0U) return refuse(1015U);
        if (state.interactionTargetBirth!=0U||state.interactionTargetClass!=0U) return refuse(1016U);
    } else if (state.interactionTargetPeer!=0U||state.interactionParticipantIncarnation!=0U||state.interactionTargetBirth==0U||
             state.interactionTargetClass==0U||(state.interactionTargetA==0U&&state.interactionTargetB==0U)) return refuse(1017U);
    if (state.interactionPairRevision == 0U) return refuse(1018U);
    if (state.interactionTransition > 1U || (state.interactionTransitionFlags & ~0xFFFFU) != 0U) return refuse(1019U);
    if (state.interactionTransition == 0U || state.interactionTransitionRevision == 0U) return Check::Waiting;
    command.source = Source(state);
    command.victim = player?Participant{state.interactionTargetPeer,0U,0U,state.interactionParticipantIncarnation,0U}:
        Participant{0U,state.interactionTargetA,state.interactionTargetB,state.interactionTargetBirth,state.interactionTargetClass};
    command.pairRevision = state.interactionPairRevision;
    command.transitionRevision = state.interactionTransitionRevision;
    command.transition = static_cast<Transition>(state.interactionTransition);
    command.sourceDefinition = state.interactionTransitionVariant;
    command.firstFlag = state.interactionTransitionFlags & 0xFFU;
    command.secondFlag = (state.interactionTransitionFlags >> 8U) & 0xFFU;
    command.outcomeRequired = (state.interactionFlags & InteractionNativeOutcomeRequired) != 0U;
    const auto& outcome = state.outcome;
    command.outcome = {outcome.revision, outcome.flags, outcome.definition, outcome.kind,
        outcome.hpBefore, outcome.hpPrepared, outcome.hpReaction, outcome.healthReaction,
        outcome.sourceYaw, {outcome.targetOffset[0], outcome.targetOffset[1], outcome.targetOffset[2]}};
    if (command.outcomeRequired && (outcome.revision == 0U || (outcome.flags & OutcomePrepared) == 0U)) return Check::Waiting;
    return Check::Ready;
}
bool SameAdmission(Command current, Command original) noexcept {
    // Reaction results and end obligations advance separately. They never
    // mutate the preparation or become another native admission.
    if (current.outcome.revision < original.outcome.revision ||
        !SamePreparation(current.outcome, original.outcome)) return false;
    current.outcome = original.outcome;
    return current == original;
}
struct Busy final {
    bool& value;
    explicit Busy(bool& flag) noexcept : value(flag) { value = true; }
    ~Busy() { value = false; }
};
// Native calls may synchronously query the embedding pair's published receipt.
// Build a result privately; clearing the caller's receipt at entry temporarily
// revoked an already earned admission during Inspect/Observe callbacks.
struct ReceiptPublication final {
    Receipt& destination;
    Receipt value{};
    ~ReceiptPublication() { destination = value; }
};
}

static Check CheckSourceState(const Memory& memory, const LocalParticipant& source,
    const PairState& admitted, const std::uintptr_t expectedCycle,bool ended) noexcept {
    if (memory.imageBase < 0x10000U || memory.imageBase > 0xFFFFFFFFULL - 0x00EAE5D0U ||
        source.character == 0U || source.runtimeId == 0U || admitted.source != source.character ||
        admitted.sourceRuntimeId != source.runtimeId || expectedCycle == 0U ||
        admitted.sourceCycle != expectedCycle || admitted.sourceGraph == 0U ||
        !admitted.sourcePrepared) return Check::Rejected;
    std::uintptr_t cycle{}, owner{}, table{}, action{}, graph{};
    std::uint32_t runtime{};
    std::uint8_t kind{}, serial{}, flags{};
    if (!Read(memory, source.character + 0x50U, runtime)) return Check::Unknown;
    if (runtime != source.runtimeId) return Check::Rejected;
    if (!Pointer(memory, source.character + 0x36CU, cycle)) return Check::Unknown;
    if (cycle != expectedCycle) return Check::Rejected;
    if (!Pointer(memory, cycle, table) || !Pointer(memory, cycle + 4U, owner) ||
        !Read(memory, cycle + 0x1AU, kind) || !Read(memory, cycle + 0x19U, serial) ||
        !Read(memory, cycle + 8U, flags)) return Check::Unknown;
    if (table != memory.imageBase + 0x00EAE5D0U || owner != source.character || kind != 0x4EU ||
        serial != admitted.sourceSerial || (flags & 6U) != (ended?6U:0U)) return Check::Rejected;
    if (!Pointer(memory, cycle + 0x14U, action)) return Check::Unknown;
    if (action == 0U) return Check::Rejected;
    if (!Pointer(memory, action + 0x50U, graph)) return Check::Unknown;
    if (graph != admitted.sourceGraph) return Check::Rejected;
    // Native End_Action writes flags|6 at 00664AB2. E6 is instead set by
    // 00655A21 after 0077A180/virtual+3C accepts a finish request; stage 6 and
    // victim D8 are not graph disposal. An end request permits native cleanup;
    // it never discharges the independent ReleaseProof obligation.
    std::uintptr_t currentCycle{}, currentAction{}, currentGraph{};
    if (!Read(memory, source.character + 0x50U, runtime) ||
        !Pointer(memory, source.character + 0x36CU, currentCycle) ||
        !Pointer(memory, cycle + 0x14U, currentAction) ||
        !Pointer(memory, action + 0x50U, currentGraph) ||
        !Read(memory, cycle + 0x19U, serial) || !Read(memory, cycle + 8U, flags)) return Check::Unknown;
    return runtime == source.runtimeId && currentCycle == cycle && currentAction == action &&
        currentGraph == graph && serial == admitted.sourceSerial && (flags & 6U) == (ended?6U:0U) ? Check::Ready : Check::Rejected;
}
Check CheckSourceRetention(const Memory& memory,const LocalParticipant& source,const PairState& admitted,
    std::uintptr_t expectedCycle) noexcept {return CheckSourceState(memory,source,admitted,expectedCycle,false);}
Check CheckSourceExit(const Memory& memory,const LocalParticipant& source,const PairState& admitted,
    std::uintptr_t expectedCycle) noexcept {return CheckSourceState(memory,source,admitted,expectedCycle,true);}
bool RetainsNativeSource(const Memory& memory, const LocalParticipant& source,
    const PairState& admitted, const std::uintptr_t expectedCycle) noexcept {
    return CheckSourceRetention(memory, source, admitted, expectedCycle) != Check::Rejected;
}

bool Runtime::Initialize(const std::uintptr_t imageBase, const bool exactBuildValidated,
    const Entrypoints entries, const Permission permission, void* context) noexcept {
    if (base_ != 0U || !adapter_.Initialize(imageBase, exactBuildValidated, entries, permission, context)) return false;
    base_ = imageBase;
    return true;
}
Result Runtime::Configure(const transactions::Scope& scope, const std::uint64_t cookie,
    const std::uint32_t world) noexcept {
    if (busy_) return Result::Reentrant;
    if (cookie == 0U || world == 0U) return Result::Rejected;
    if (configured_ && scope == scope_ && (cookie != sessionCookie_ || world != worldGeneration_)) return Result::Conflict;
    std::uint64_t expectedCookie{};for(std::size_t i=0;i<8U;++i)expectedCookie|=static_cast<std::uint64_t>(scope.session[i])<<(i*8U);
    if(cookie!=(expectedCookie?expectedCookie:1U)||scope.worldEpoch!=world)return Result::Rejected;
    if(configured_&&scope.session==scope_.session&&scope.authorityEpoch==scope_.authorityEpoch&&
        scope.worldEpoch==scope_.worldEpoch&&scope.consumerEpoch!=scope_.consumerEpoch)return Result::WrongScope;
    const auto result = adapter_.Configure(scope);
    if (result != Result::Admitted) return result;
    scope_ = scope; sessionCookie_ = cookie; worldGeneration_ = world;
    entries_ = {}; count_ = 0U; configured_ = true;
    return result;
}
bool Runtime::ResetForWorld(const std::uint32_t current) noexcept {
    if (busy_ || !adapter_.ResetForWorld(current)) return false;
    entries_ = {}; count_ = 0U; scope_ = {};
    sessionCookie_ = 0U; worldGeneration_ = 0U; configured_ = false;
    return true;
}
bool Runtime::MatchesCarrier(const State& state) const noexcept {
    return configured_ && state.sessionCookie == sessionCookie_ && state.worldGeneration == worldGeneration_&&
        state.interactionAuthority==scope_.authorityEpoch&&state.interactionCommand!=0U&&
        std::equal(scope_.session.begin(),scope_.session.end(),std::begin(state.interactionSession));
}
Runtime::Entry* Runtime::FindEntry(const Key& key) noexcept {
    for (std::size_t i = 0U; i < count_; ++i) if (entries_[i].command.key == key) return &entries_[i];
    return nullptr;
}
void Runtime::CaptureSource(Entry& entry, const Receipt& receipt) noexcept {
    const auto& native = receipt.native;
    if (entry.ownedSource.sourceCycle == 0U && !entry.invalidated && !entry.sourceEnded &&
        receipt.transitionEntered && native.sourcePrepared && native.linked && native.victimAcquired &&
        native.source == entry.binding.source.character && native.sourceRuntimeId == entry.binding.source.runtimeId &&
        native.sourceGraph != 0U) entry.ownedSource = native;
}
Result Runtime::Submit(const State& state, const Binding& binding, Receipt& published) noexcept {
    ReceiptPublication publication{published};
    auto& output=publication.value;
    if (busy_) return Result::Reentrant;
    if (!MatchesCarrier(state)) return Result::WrongScope;
    Command command{};
    const auto captured = Capture(state, command);
    if (captured != Check::Ready) return captured == Check::Waiting ? Result::Waiting : Result::Rejected;
    command.key={scope_,state.interactionCommand};
    const bool player=command.victim.peer!=0U;
    if (binding.source.perspective != Perspective::Npc ||
        binding.source.entityA != command.source.entityA || binding.source.entityB != command.source.entityB ||
        (player?(binding.victim.perspective==Perspective::Npc||binding.victim.creationSerial==0U||
            binding.victim.participantIncarnation!=command.victim.birth):
            (binding.victim.perspective!=Perspective::Npc||binding.victim.entityA!=command.victim.entityA||
             binding.victim.entityB!=command.victim.entityB))){RefusalSite()=102U;return Result::Rejected;}
    Entry* entry{};
    for (std::size_t i = 0U; i < count_; ++i) if (SamePair(entries_[i].command, state)) { entry = &entries_[i]; break; }
    if (entry != nullptr) {
        if (!SameAdmission(command, entry->command) || binding != entry->binding) return Result::Conflict;
        if (entry->invalidated) { static_cast<void>(adapter_.Find(command.key, output)); return Result::Unknown; }
    } else {
        if ((state.interactionFlags & InteractionCycleActive) == 0U) { RefusalSite()=103U; return Result::Rejected; }
        if ((state.outcome.flags & OutcomeSourceEnded) != 0U) { RefusalSite()=104U; return Result::Rejected; }
        if(FindEntry(command.key)!=nullptr)return Result::Conflict;
        if (count_ == kCapacity) return Result::Capacity;
        entry = &entries_[count_++];
        *entry = {};
        entry->command = command; entry->binding = binding;
    }
    Busy guard(busy_);
    Receipt previous{};
    if (adapter_.Find(entry->command.key, previous)) {
        const auto progress = adapter_.AdvanceOutcome(entry->command.key, previous.serial, command.outcome);
        if (progress != Result::Admitted && progress != Result::Duplicate && entry->command.outcomeRequired) {
            static_cast<void>(adapter_.Find(entry->command.key, output)); return progress;
        }
    }
    const auto result = adapter_.Admit(entry->command, entry->binding, output);
    if (output.pairEntered && result != Result::Unknown && entry->command.outcomeRequired) {
        const auto progress = adapter_.AdvanceOutcome(entry->command.key, output.serial, command.outcome);
        static_cast<void>(adapter_.Find(entry->command.key, output));
        if (progress != Result::Admitted && progress != Result::Duplicate) return progress;
    }
    CaptureSource(*entry, output);
    return result;
}
bool Runtime::Find(const State& state, Receipt& output) const noexcept {
    output = {};
    if (!MatchesCarrier(state)) return false;
    for (std::size_t i = 0U; i < count_; ++i)
        if (SamePair(entries_[i].command, state)&&entries_[i].command.key.command==state.interactionCommand)
            return adapter_.Find(entries_[i].command.key, output);
    return false;
}
Result Runtime::Inspect(const Key& inputKey, Receipt& published) noexcept {
    const auto key = inputKey;
    ReceiptPublication publication{published};
    auto& output=publication.value;
    if (busy_) return Result::Reentrant;
    if (key.scope != scope_ || !configured_) return Result::WrongScope;
    Busy guard(busy_);
    const auto result = adapter_.Inspect(key, output);
    if (auto* entry = FindEntry(key); entry != nullptr) CaptureSource(*entry, output);
    return result;
}
Result Runtime::Observe(const Key& inputKey, const std::uint64_t serial, const LocalParticipant& actor,
    const std::uintptr_t cycle, const std::uint8_t cycleSerial, const FinishCause cause, Receipt& published) noexcept {
    const auto key = inputKey;
    ReceiptPublication publication{published};
    auto& output=publication.value;
    if (busy_) return Result::Reentrant;
    auto* entry = FindEntry(key);
    if (entry == nullptr) return key.scope == scope_ ? Result::Rejected : Result::WrongScope;
    Receipt previous{};
    if (!adapter_.Find(key, previous) || previous.serial != serial || previous.resourcesReleased) return Result::Rejected;
    const bool source = actor == entry->binding.source && cycle == previous.native.sourceCycle && cycleSerial == previous.native.sourceSerial;
    const bool victim = actor == entry->binding.victim && cycle == previous.native.victimCycle && cycleSerial == previous.native.victimSerial;
    if (!source && !victim) return Result::Rejected;
    Busy guard(busy_);
    const auto result = adapter_.Observe(key, serial, actor, cycle, cycleSerial, cause, output);
    if (cause == FinishCause::NativeDestruction || cause == FinishCause::NativeInterruption) entry->invalidated = true;
    if (source && cause == FinishCause::NativeDestruction) entry->sourceRevoked = true;
    // 00664A10 can receive End_Action yet decline termination while +44 is a
    // live interactive victim and E5 is clear. The callback's name alone is
    // not an end fact; require the exact inspected post-call native flags.
    if (source && cause == FinishCause::NativeGraphEnd && result != Result::Unknown && result != Result::Rejected &&
        (output.native.sourceFlags & 6U) == 6U) entry->sourceEnded = true;
    CaptureSource(*entry, output);
    return result;
}
Result Runtime::OutcomeApplied(const Key& key, const std::uint64_t serial,
    const Outcome& outcome, const std::uint32_t evidence) noexcept {
    if (busy_) return Result::Reentrant;
    if (key.scope != scope_ || !configured_) return Result::WrongScope;
    return adapter_.OutcomeApplied(key, serial, outcome, evidence);
}
Result Runtime::Release(const Key& inputKey, const ReleaseProof proof, void* context, Receipt& published) noexcept {
    const auto key = inputKey;
    ReceiptPublication publication{published};
    auto& output=publication.value;
    if (busy_) return Result::Reentrant;
    if (key.scope != scope_ || !configured_) return Result::WrongScope;
    Busy guard(busy_);
    return adapter_.Release(key, proof, context, output);
}
void Runtime::Invalidate(const LocalParticipant& actor) noexcept {
    adapter_.Invalidate(actor);
    for (std::size_t i = 0U; i < count_; ++i)
        if (entries_[i].binding.source == actor || entries_[i].binding.victim == actor) {
            entries_[i].invalidated = true;
            if (entries_[i].binding.source == actor) entries_[i].sourceRevoked = true;
        }
}
bool Runtime::RetainSource(const std::uintptr_t source, const std::uintptr_t cycle, const Memory& memory) noexcept {
    if (!configured_ || memory.imageBase != base_) return false;
    for (std::size_t i = 0U; i < count_; ++i) {
        auto& entry = entries_[i];
        Receipt receipt{};
        if (entry.binding.source.character != source || entry.sourceRevoked || entry.sourceEnded ||
            !adapter_.Find(entry.command.key, receipt) || receipt.resourcesReleased ||
            !receipt.pairEntered || !receipt.transitionEntered) continue;
        // Native initialization can run between engine pumps. Capture only
        // the exact admitted source cycle/serial and its initialized graph;
        // its victim may already be completing and must not own this outro.
        if(entry.ownedSource.sourceGraph==0U&&receipt.native.sourceCycle==cycle) {
            auto anchor=receipt.native;
            std::uintptr_t action{},graph{};std::uint8_t serial{},prepared{};
            if(Read(memory,cycle+0x19U,serial)&&serial==anchor.sourceSerial&&
                Read(memory,cycle+0xE7U,prepared)&&prepared!=0U&&Pointer(memory,cycle+0x14U,action)&&action!=0U&&
                Pointer(memory,action+0x50U,graph)&&graph!=0U) {
                anchor.sourcePrepared=true;anchor.sourceGraph=graph;
                if(CheckSourceRetention(memory,entry.binding.source,anchor,cycle)==Check::Ready)entry.ownedSource=anchor;
            }
            if(entry.ownedSource.sourceGraph==0U&&PendingSource(memory,entry.binding.source,receipt.native,cycle)!=Check::Rejected)
                return true;
        }
        if (RetainsNativeSource(memory, entry.binding.source, entry.ownedSource, cycle)) return true;
    }
    return false;
}
} // namespace aimp::ai_sync::interactions
