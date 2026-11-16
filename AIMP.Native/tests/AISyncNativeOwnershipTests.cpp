#include "aimp/AISyncNativeOwnership.h"
#include "aimp/AISyncExecutionCodec.h"
#include <cstdlib>
#include <iostream>
#include <map>
#include <limits>

using namespace aimp::ai_sync::execution;
namespace {
void Check(bool value,int line) {if(!value){std::cerr<<"native ownership invariant at "<<line<<'\n';std::abort();}}
#define CHECK(value) Check((value),__LINE__)
struct Memory {
    std::map<std::uintptr_t,std::uint8_t> bytes;
    std::uintptr_t failAt{},mutateAt{};
    bool mutated{};
    unsigned reads{};
    ActorLifetime actor{0x100000U,15U,7U};
    static constexpr std::uint32_t cycle=0x200000U,movement=0x300000U;
    template<class T> void Put(std::uintptr_t at,T value) {
        const auto* source=reinterpret_cast<const std::uint8_t*>(&value);
        for(std::size_t i=0;i<sizeof(T);++i)bytes[at+i]=source[i];
    }
    Memory() {
        Put(actor.character+0x50U,actor.runtimeId);Put(actor.character+0x2CD0U,static_cast<std::uint32_t>(actor.character));
        Put(actor.character+0x36CU,cycle);Put(cycle+4U,static_cast<std::uint32_t>(actor.character));
        Cycle(0xEABF80U,1U);Put(cycle+0x19U,std::uint8_t{7});
        Put(actor.character+0x297U,std::uint8_t{});Put(actor.character+0x32CU,std::uint32_t{1});
        Put(actor.character+0x2CD8U,std::uint32_t{});Put(actor.character+0x2CE4U,std::uint32_t{});
        Put(actor.character+0x42A8U,movement);Put(actor.character+0x42ACU,0x310000U);
        for(const auto offset:{4U,0x14U,0x2CU,0x38U,0x40U,0x44U,0x5CU})Put(movement+0xC4U+offset,std::uint32_t{});
    }
    void Cycle(std::uint32_t type,std::uint8_t state) {Put(cycle,0x400000U+type);Put(cycle+0x1AU,state);}
    static bool Read(void* context,std::uintptr_t at,void* target,std::size_t size) noexcept {
        auto& m=*static_cast<Memory*>(context);++m.reads;
        auto* data=static_cast<std::uint8_t*>(target);
        for(std::size_t i=0;i<size;++i){if(at+i==m.failAt)return false;const auto it=m.bytes.find(at+i);if(it==m.bytes.end())return false;data[i]=it->second;}
        if(at==m.mutateAt&&!m.mutated){m.mutated=true;m.Put(m.actor.character+0x50U,m.actor.runtimeId+1U);}
        return true;
    }
    OwnershipCheck Inspect(bool quiet=true) {return InspectMovementBoundary({0x400000U,this,&Read},actor,quiet);}
};
void NativeIdleAndCycles() {
    Memory m;CHECK(m.Inspect()==OwnershipCheck::Ready);CHECK(m.Inspect(false)==OwnershipCheck::Ready);
    m.Cycle(0xEABF80U,0x38U);CHECK(m.Inspect()==OwnershipCheck::Waiting);CHECK(m.Inspect(false)==OwnershipCheck::Ready);
    m.Cycle(0xEABEC0U,0x3BU);CHECK(m.Inspect()==OwnershipCheck::Ready);
    for(const auto type:{0xEAE5D0U,0xEAE690U,0xEAE1B8U,0x123456U})for(const auto state:{1U,0x38U,0x3BU,0x42U,0x4EU,0x4FU}) {
        m.Cycle(type,static_cast<std::uint8_t>(state));CHECK(m.Inspect(false)==OwnershipCheck::Waiting);
    }
    m.Cycle(0xEABF80U,1U);m.Put(m.actor.character+0x297U,std::uint8_t{1});CHECK(m.Inspect(false)==OwnershipCheck::Waiting);
    m.Put(m.actor.character+0x297U,std::uint8_t{});m.Put(m.actor.character+0x32CU,std::uint32_t{3});
    CHECK(m.Inspect(false)==OwnershipCheck::Waiting);
}
void SchedulerAndNavigation() {
    Memory m;
    // Regression from the v262 Client trace: neutral state 1, not locomotion
    // Idle. It may acquire ownership only with the same full native proof.
    m.Cycle(0xEAD858U,1U);
    CHECK(m.Inspect(false)==OwnershipCheck::Ready&&m.Inspect()==OwnershipCheck::Ready);
    for(const auto offset:{0x2CD8U,0x2CE4U}) {
        m.Put(m.actor.character+offset,std::uint32_t{1});CHECK(m.Inspect()==OwnershipCheck::Waiting);
        m.Put(m.actor.character+offset,std::uint32_t{});CHECK(m.Inspect()==OwnershipCheck::Ready);
    }
    for(const auto offset:{4U,0x14U,0x2CU,0x38U,0x40U,0x44U,0x5CU}) {
        m.Put(Memory::movement+0xC4U+offset,std::uint32_t{1});CHECK(m.Inspect()==OwnershipCheck::Waiting);
        m.Put(Memory::movement+0xC4U+offset,std::uint32_t{});CHECK(m.Inspect()==OwnershipCheck::Ready);
    }
    m.Put(m.actor.character+0x42ACU,std::uint32_t{});CHECK(m.Inspect()==OwnershipCheck::Unknown);
}
void FaultsAndLifetimeChanges() {
    Memory baseline;
    for(const auto& [address,value]:baseline.bytes) {
        static_cast<void>(value);Memory m;m.failAt=address;
        CHECK(m.Inspect()==OwnershipCheck::Unknown);
    }
    for(const auto at:std::array<std::uintptr_t,2>{baseline.actor.character+0x32CU,Memory::movement+0xC4U+0x5CU}) {
        Memory m;m.mutateAt=at;CHECK(m.Inspect()==OwnershipCheck::Unknown&&m.mutated);
    }
    Memory stale;++stale.actor.runtimeId;CHECK(stale.Inspect()==OwnershipCheck::Unknown);
    Memory owner;owner.Put(Memory::cycle+4U,std::uint32_t{0x110000});CHECK(owner.Inspect()==OwnershipCheck::Unknown);
    Memory invalid;auto actor=invalid.actor;actor.nativeGeneration=0;
    CHECK(InspectMovementBoundary({0x400000U,&invalid,&Memory::Read},actor,true)==OwnershipCheck::Reject&&invalid.reads==0U);
}
void NativeInteractionReplacement() {
    Memory m;m.Cycle(0xEABE00U,0x37U);
    const std::uintptr_t table=0x400000U+0xEABE00U;
    m.Put(table,std::uint32_t{0x6577F0U});m.Put(table+0x60U,std::uint32_t{0x657870U});
    const auto inspect=[&] {return InspectInteractionBoundary({0x400000U,&m,&Memory::Read},m.actor);};
    CHECK(m.Inspect(false)==OwnershipCheck::Waiting); // Movement ownership remains stricter.
    CHECK(inspect()==OwnershipCheck::Ready); // The Host replaces this exact running cycle.
    const auto owned=[&] {return InspectAuthoritativeExecutionBoundary({0x400000U,&m,&Memory::Read},m.actor,true);};
    CHECK(owned()==OwnershipCheck::Ready);
    m.Put(Memory::movement+0xC4U+0x5CU,std::uint32_t{1});CHECK(owned()==OwnershipCheck::Waiting);
    m.Put(Memory::movement+0xC4U+0x5CU,std::uint32_t{});
    m.Put(m.actor.character+0x2CD8U,std::uint32_t{1});CHECK(owned()==OwnershipCheck::Waiting);
    m.Put(m.actor.character+0x2CD8U,std::uint32_t{});CHECK(owned()==OwnershipCheck::Ready);
    for(const auto state:{0x42U,0x4EU,0x4FU}) {
        m.Cycle(0xEABE00U,static_cast<std::uint8_t>(state));CHECK(inspect()==OwnershipCheck::Waiting);
    }
    m.Cycle(0xEABE00U,0x37U);m.Put(table,std::uint32_t{0xF0000000U});
    CHECK(inspect()==OwnershipCheck::Waiting);
    m.Put(table,std::uint32_t{0x6577F0U});m.failAt=table+0x60U;CHECK(inspect()==OwnershipCheck::Unknown);
    m.failAt=0;m.Put(m.actor.character+0x297U,std::uint8_t{1});CHECK(inspect()==OwnershipCheck::Waiting);
    m.Put(m.actor.character+0x297U,std::uint8_t{});m.Put(m.actor.character+0x32CU,std::uint32_t{3});
    CHECK(inspect()==OwnershipCheck::Waiting);
    m.Put(m.actor.character+0x32CU,std::uint32_t{1});m.mutateAt=table+0x60U;
    CHECK(inspect()==OwnershipCheck::Unknown);
}
void InitialBackstageHold() {
    Memory m;m.Cycle(0xDBB1B8U,0x4DU);m.Put(Memory::cycle+0x44U,-1.0F);
    const auto inspect=[&](bool quiet=true) {
        return InspectInitialBackstageBoundary({0x400000U,&m,&Memory::Read},m.actor,quiet);
    };
    CHECK(m.Inspect(false)==OwnershipCheck::Waiting);
    CHECK(inspect(false)==OwnershipCheck::Ready&&inspect()==OwnershipCheck::Ready);
    for(const auto offset:{0x2CD8U,0x2CE4U}) {
        m.Put(m.actor.character+offset,1U);CHECK(inspect()==OwnershipCheck::Waiting);
        m.Put(m.actor.character+offset,0U);
    }
    m.Put(Memory::movement+0xC4U+0x5CU,1U);CHECK(inspect()==OwnershipCheck::Waiting);
    m.Put(Memory::movement+0xC4U+0x5CU,0U);
    for(const auto state:{0x4BU,0x4CU,0x4EU,0x4FU}) {
        m.Cycle(0xDBB1B8U,static_cast<std::uint8_t>(state));CHECK(inspect(false)==OwnershipCheck::Waiting);
    }
    for(const auto type:{0xDBB2A0U,0xDBB360U,0xEAE5D0U}) {
        m.Cycle(type,0x4DU);CHECK(inspect(false)==OwnershipCheck::Waiting);
    }
    m.Cycle(0xDBB1B8U,0x4DU);
    m.Put(Memory::cycle+0x44U,std::numeric_limits<float>::quiet_NaN());CHECK(inspect()==OwnershipCheck::Waiting);
    m.Put(Memory::cycle+0x44U,-1.0F);m.failAt=Memory::cycle+0x44U;CHECK(inspect()==OwnershipCheck::Unknown);
    m.failAt=0;m.Put(m.actor.character+0x297U,std::uint8_t{1});CHECK(inspect()==OwnershipCheck::Waiting);
    m.Put(m.actor.character+0x297U,std::uint8_t{});m.Put(m.actor.character+0x32CU,3U);CHECK(inspect()==OwnershipCheck::Waiting);
    m.Put(m.actor.character+0x32CU,1U);m.mutateAt=Memory::cycle+0x44U;CHECK(inspect()==OwnershipCheck::Unknown);
}
void AlienPossessionDoesNotCancelSpawnPassage() {
    Memory m;
    m.Put(m.actor.character+0x2DAU,std::uint8_t{});
    m.Put(m.actor.character+0x2DBU,std::uint8_t{});
    const auto inspect=[&](bool quiet) {
        return InspectAlienPossessionBoundary({0x400000U,&m,&Memory::Read},m.actor,quiet);
    };
    // The observed Host cycle75 is still moving out of its map spawn.
    // The PRE-drain boundary must refuse it, even with an active scheduler.
    m.Cycle(0xDBB2A0U,75U);m.Put(m.actor.character+0x2CD8U,1U);
    CHECK(inspect(false)==OwnershipCheck::Waiting);
    m.Cycle(0xDBB360U,76U);CHECK(inspect(false)==OwnershipCheck::Waiting);
    // Retail completion produces a stationary backstage hold. This may now
    // retire the scheduler, but full possession still requires quiet resources.
    m.Cycle(0xDBB1B8U,77U);m.Put(Memory::cycle+0x44U,-1.0F);
    CHECK(inspect(false)==OwnershipCheck::Waiting); // Frontstage must not borrow this exception.
    m.Put(m.actor.character+0x2DAU,std::uint8_t{1});
    CHECK(inspect(false)==OwnershipCheck::Ready);
    CHECK(inspect(true)==OwnershipCheck::Waiting);
    m.Put(m.actor.character+0x2CD8U,0U);CHECK(inspect(true)==OwnershipCheck::Ready);
    m.Put(Memory::movement+0xC4U+0x44U,1U);CHECK(inspect(true)==OwnershipCheck::Waiting);
    m.Put(Memory::movement+0xC4U+0x44U,0U);
    m.Cycle(0xDBB2A0U,75U);CHECK(inspect(false)==OwnershipCheck::Waiting);
    m.Cycle(0xEABF80U,1U);m.Put(m.actor.character+0x2DAU,std::uint8_t{});
    CHECK(inspect(false)==OwnershipCheck::Ready && inspect(true)==OwnershipCheck::Ready);
    // v517 trace: the spawn Move is already in locomotion-start55 at the first
    // bind poll. Possession may drain/cancel it; generic movement may not.
    m.Cycle(0xEABE00U,0x37U);m.Put(m.actor.character+0x2CD8U,1U);
    CHECK(inspect(false)==OwnershipCheck::Ready && inspect(true)==OwnershipCheck::Waiting);
    CHECK(m.Inspect(false)==OwnershipCheck::Waiting);
    m.Put(m.actor.character+0x2CD8U,0U);CHECK(inspect(true)==OwnershipCheck::Ready);
    // The vent entry that follows remains native-owned, as does a backstage55.
    m.Cycle(0xEADB58U,18U);CHECK(inspect(false)==OwnershipCheck::Waiting);
    m.Cycle(0xEABE00U,0x37U);m.Put(m.actor.character+0x2DAU,std::uint8_t{1});
    CHECK(inspect(false)==OwnershipCheck::Waiting);
    m.Put(m.actor.character+0x2DAU,std::uint8_t{});
}
void ExactNativeExecutionProvenance() {
    Memory m;m.actor.creationSerial=11;
    aimp::ai_sync::transactions::Scope scope{};scope.session[0]=1;scope.authorityEpoch=scope.worldEpoch=scope.consumerEpoch=1;
    const aimp::ai_sync::transactions::OwnerKey owner{1,2,3,4,aimp::ai_sync::transactions::Capability::Movement,5};
    NativeExecutionAnchor anchor{scope,owner,m.actor,Memory::cycle,7};
    const auto matches=[&] {return MatchesNativeExecution({0x400000U,&m,&Memory::Read},anchor,scope,owner,m.actor);};
    CHECK(matches());
    ++scope.consumerEpoch;CHECK(!matches());--scope.consumerEpoch;
    ++anchor.owner.birth;CHECK(!matches());--anchor.owner.birth;
    ++anchor.actor.creationSerial;CHECK(!matches());--anchor.actor.creationSerial;
    ++anchor.actor.nativeGeneration;CHECK(!matches());--anchor.actor.nativeGeneration;
    m.Put(Memory::cycle+0x19U,std::uint8_t{8});CHECK(!matches());m.Put(Memory::cycle+0x19U,std::uint8_t{7});
    m.Put(m.actor.character+0x50U,m.actor.runtimeId+1U);CHECK(!matches());m.Put(m.actor.character+0x50U,m.actor.runtimeId);
    m.failAt=Memory::cycle+0x19U;CHECK(!matches());
}
void ExactPassageLifetime(std::uint32_t expectedType,std::uint8_t expectedState) {
    Memory m;m.actor.creationSerial=11;m.Cycle(expectedType,expectedState);
    m.Put(Memory::cycle+8U,std::uint8_t{0x20});
    aimp::ai_sync::transactions::Scope scope{};scope.session[0]=1;scope.authorityEpoch=scope.worldEpoch=scope.consumerEpoch=1;
    const aimp::ai_sync::transactions::OwnerKey owner{1,2,3,4,aimp::ai_sync::transactions::Capability::Movement,5};
    const NativeExecutionAnchor anchor{scope,owner,m.actor,Memory::cycle,7};
    const auto live=[&] {return IsLiveNativePassage({0x400000U,&m,&Memory::Read},anchor,scope,owner,m.actor);};
    CHECK(live());
    // The exact serial and class matter even when the engine reuses its cycle allocation.
    m.Cycle(0xEABF80U,expectedState);CHECK(!live());m.Cycle(expectedType,0x38U);CHECK(!live());
    m.Cycle(expectedType,static_cast<std::uint8_t>(expectedState==0x4BU?0x4CU:0x4BU));CHECK(!live());
    m.Cycle(expectedType,expectedState);m.Put(Memory::cycle+0x19U,std::uint8_t{8});CHECK(!live());
    m.Put(Memory::cycle+0x19U,std::uint8_t{7});
    for(const auto flags:{0x02U,0x04U,0x06U,0x26U}) {
        m.Put(Memory::cycle+8U,static_cast<std::uint8_t>(flags));CHECK(!live());
    }
    m.Put(Memory::cycle+8U,std::uint8_t{0x20});CHECK(live());
    m.Put(Memory::cycle+4U,std::uint32_t{0x110000});CHECK(!live());
    m.Put(Memory::cycle+4U,static_cast<std::uint32_t>(m.actor.character));
    m.Put(m.actor.character+0x36CU,std::uint32_t{0x210000});CHECK(!live());
    m.Put(m.actor.character+0x36CU,Memory::cycle);
    ++scope.consumerEpoch;CHECK(!live());--scope.consumerEpoch;
    ++m.actor.creationSerial;CHECK(!live());--m.actor.creationSerial;
    ++m.actor.nativeGeneration;CHECK(!live());--m.actor.nativeGeneration;
    m.Put(m.actor.character+0x32CU,std::uint32_t{3});CHECK(!live());
    m.Put(m.actor.character+0x32CU,std::uint32_t{1});
    m.Put(m.actor.character+0x297U,std::uint8_t{1});CHECK(!live());
    m.Put(m.actor.character+0x297U,std::uint8_t{});
    for(const auto at:std::array<std::uintptr_t,9>{m.actor.character+0x50U,m.actor.character+0x36CU,m.actor.character+0x32CU,
        m.actor.character+0x297U,std::uintptr_t{Memory::cycle},Memory::cycle+4U,Memory::cycle+8U,
        Memory::cycle+0x19U,Memory::cycle+0x1AU}) {
        m.failAt=at;CHECK(!live());
    }
    m.failAt=0;m.mutateAt=m.actor.character+0x32CU;CHECK(!live()&&m.mutated);
    for(std::uint32_t state=0;state<=0x5DU;++state)
        CHECK(OrdinaryPassageTransition(state)==(state==1U||state==2U||state==0x38U||state==0x3BU||state==0x4DU));
}
void PassageCausalReplacement() {
    using namespace aimp::ai_sync::transactions;
    NativeExecutionAnchor anchor{};anchor.scope.session[0]=1;anchor.scope.consumerEpoch=3;
    anchor.owner={1,2,3,4,Capability::Movement,5};
    Record record{};record.scope=anchor.scope;record.owner=anchor.owner;
    record.globalSeq=82;record.commandId=21;record.opcode=Op::Submit;record.nativeKind=1;
    const auto supersedes=[&](NativeOutcome outcome=NativeOutcome::Admitted) {
        return SupersedesNativePassage(anchor,81,20,record,outcome);
    };
    CHECK(supersedes());
    CHECK(!supersedes(NativeOutcome::Rejected)&&!supersedes(NativeOutcome::Unknown));
    // A continuing Move update and the direct call's immediate retirement do not end75.
    for(const auto op:{Op::Update,Op::Complete,Op::Release,Op::Cancel,Op::Stop}) {
        record.opcode=op;CHECK(!supersedes());
    }
    record.opcode=Op::Submit;record.globalSeq=81;CHECK(!supersedes());record.globalSeq=82;
    record.commandId=20;CHECK(!supersedes());record.commandId=21;
    record.admission=1;CHECK(!supersedes());record.admission=2;CHECK(!supersedes());record.admission=0;
    ++record.scope.consumerEpoch;CHECK(!supersedes());--record.scope.consumerEpoch;
    ++record.owner.birth;CHECK(!supersedes());--record.owner.birth;
    for(const auto kind:{1U,kRelocateKind,kForcedRelocateKind,kDirectRelocateKind,kDirectForcedRelocateKind,kFrontstageKind}) {
        record.nativeKind=kind;CHECK(supersedes());
    }
    for(const auto kind:{2U,3U}) {record.nativeKind=kind;CHECK(!supersedes());}
    record.nativeKind=kBackstageKind;record.mode=1;CHECK(!supersedes());record.mode=0;CHECK(supersedes());
    record.nativeKind=aimp::ai_sync::native_intent::kKind;record.commandId=20;record.intent.words[0]=21;
    record.opcode=Op::Release;CHECK(!supersedes());
}
}
int main() {
    NativeIdleAndCycles();SchedulerAndNavigation();FaultsAndLifetimeChanges();NativeInteractionReplacement();InitialBackstageHold();AlienPossessionDoesNotCancelSpawnPassage();ExactNativeExecutionProvenance();
    ExactPassageLifetime(0xDBB2A0U,0x4BU);ExactPassageLifetime(0xDBB360U,0x4CU);PassageCausalReplacement();
    std::cout<<"Native ownership: movement boundaries, exact passage lifetime and causal replacement PASS (modeled reads only).\n";
}
