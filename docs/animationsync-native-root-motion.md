# AnimationSync native root-motion isolation (536)

Status: experimental; requires **two live game instances** to verify visual
alignment. This change is deliberately scoped to AnimationSync's own scripted
cycles. It does **not** alter AISync Alien movement, paired attacks or retail
NPC steering.

## Why

Retail full-body clips have a reference-motion trajectory. Previous playback
selected those original NPC clips, but also teleported the remote Character
every post-manager frame to chase the first-person player's network transform.
After the clip, an outstanding `initialPlacementPending` caused a second
immediate snap. Neither step proves the engine's native reference motion was
actually applied; both can conceal or magnify an anchoring error.

## Ownership

* **NativeRoot**: a distinct retail NPC clip is running. The game's scripted
  cycle owns translation/rotation; no network-driven Character teleport, yaw
  write, or one-time follower placement during the clip. On completion, the
  locomotion follower resumes from the actual result, with 1500 ms before
  emergency resynchronization becomes eligible again.
* **FirstPersonPath**: no distinct NPC clip exists. Preserve the old bounded
  Character network path and optional FP bone adaptation.
* **LockerVisual**: retain the existing body-only render offset and hiding path.
* **None**: the scripted cycle is not owned; ordinary PlayerSync/AISync rules
  continue. The policy tests cover owned/unowned/native/FP/hiding combinations.

NativeRoot initially places the actor using the **first received position and
heading of that transition**, not the latest position after streaming. This
is an approximation rather than a reconstruction of the true interaction
start: a first sample received late remains offset. There is no new protocol
field or peer compatibility change.

## Diagnostics to inspect

The existing `native_animation_sync_snapshot` now exposes:

* `clipSource=npc-native` or `fp-retargeted`
* `nativeRootSamples` — frames with a live native Character transform
* `nativeRootDistanceMeters` — accumulated Character translation excluding
  single-frame jumps over 0.35 m
* `nativeRootDiscontinuities` — excluded large steps (possible teleports)
* `nativeOwnerGapMeters` and `nativeOwnerMaxGapMeters` — live Character vs
  latest FP owner world-space gap
* `nativeAnchorDriftMeters` — owner distance travelled between first receipt
  and native admission

These measure **Character** displacement, not the Havok render skeleton. A
near-zero `nativeRootDistanceMeters` on an animation that should travel proves
the Character did not move, not necessarily that the rendered body did not
move. An actual model-space/root-bone trace is the next investigation if so.

## Minimum acceptance test

1. Two players, ChallengeMap16, no Alien. First confirm plain locomotion is
   unchanged and both peers are visible.
2. Test `enter_vent` once with the remote NPC nearly still at the vent. Record
   screen video and both diagnostic archives at the end.
3. Verify `clipSource=npc-native`, `applied`, `completed`, and nonzero
   `nativeRootSamples`. Compare `nativeRootDistanceMeters` with the onscreen
   displacement and `nativeOwnerMaxGapMeters`; check for a completion snap.
4. Repeat with a delayed stream load, then a non-locomoting button or
   terminal animation. Finally test a locker, which must retain its own path.
5. If native Character motion remains ~zero while the body/owner travels,
   restore positional convergence **only for clips verified to lack physical
   root motion**; do not revert to blanket per-frame FP teleporting.

The source-level policy test is registered as
`AnimationSyncMovementPolicy` in CTest, alongside
`AnimationSyncNativeTimeline`: eight native-clock projection cases adapted
from the previous standalone Claude analysis, using explicit failure checks
even when NDEBUG is defined. Both tests are header-only. The native build
requires Windows MSVC x86; this commit is not an in-game visual proof.


## 533: Spatial forensics of vent-entry mismatches

The November 11, 2026 532 Host/client diagnostics confirm `enter_vent`
used `clipSource=npc-native` and completed twice with no admission failures:
`nativeRootSamples=246`, `nativeRootDistanceMeters=5.55`,
`nativeRootDiscontinuities=5`, and `nativeOwnerMaxGapMeters=3.24`.
The other peer captured `enter_vent` three times without remote
replays. The receiving peer's locomotion flight trace also showed
**12 hard-resync applications** after normal follower reacquisition.
That is distinct from native clip admission and can produce visible snaps
after the interaction.

Offline examination of the original Havok reference frames found that the
native `FEMALE/VENT_ENTRY_UNARMED` clip traverses approximately +0.555 m
in model Y and +2.651 m in model Z over 2.333 seconds; the FP version
travels +0.510 m and +2.543 m over 2.267 seconds. These are clip-space
reference trajectories, not proof of their world-space direction or of the
rendered mesh's motion.

533 adds **no new gameplay correction**; it captures up to the last 64
engine-thread spatial samples in `native_animation_sync_snapshot`
under `nativeSpatialTrace` (tickMs, transition, clip path, graph time,
world Character XYZ, latest owner XYZ, and each heading). Samples are
recorded at approximately 125 ms intervals only while a full-body native
clip is owned. `nativeSpatialSampleCount` counts all samples collected.
The tickMs can be correlated with the existing
`remote-locomotion-flight.jsonl` hard-resync events.

For the next short two-player vent test, compare the initial and final
native Character displacement vector with the initial and final owner
displacement vector for the same transition, and compare both headings.
If world direction differs, fix the spatial basis/heading or interaction
anchor. If their directions agree but mesh enters the wall, instrument the
visual skeleton root relative to Character. If drift begins only on cycle
completion, repair the player follower reacquisition separately.

Do not assume an animation must be corrected by new teleporting while the
native world-space direction and the rendered skeleton path remain
unmeasured.

## 534 — Vent-only visual root compensation

The two November 11, 2026 diagnostics confirm that the native `enter_vent`
clip runs successfully. Its remote Character's native world movement agrees
with the owner's rising/forward motion until around phase 1.26 seconds, then
the Character abruptly drops approximately 0.47 metres at phase 1.41 and
stops moving into the vent. The owner continues forward from Z=-19.14 to
approximately Z=-21.28. The headings at +3.14/-3.14 are equivalent, not
a confirmed reversed yaw. Ten hard-resync applications preceded the vent
entry and therefore do not explain the in-clip collision/drop.

Only native `enter_vent` (hash 256590389) receives the new policy
`NativeVentVisual`. The retail native NPC animation and physical Character
are retained, but a render-only root translation is applied to the
high-resolution final pose using the latest owner network position and the
existing locker visual offset mechanism. Physics is not teleported every
frame. A network gap beyond 3m rejects the correction rather than creating
a massive visible jump.

When this specifically owned clip ends, one physical relocation to the last
visual world location is attempted, then the offset is released, and ordinary
PlayerSync resumes. Because the remote NPC is not necessarily inside the
owner's vent traversal state, physics may still reject this relocation in the
next retail update: this behavior is explicitly experimental.

Diagnostics event `native_animation_vent_visual` reports: `frames`,
`rejected`, `relocations`, `relocationFailures`, and
`maxOffsetMillimeters`. All other NPC native animations remain on the 533
NativeRoot policy, FP-replayed fallback clips retain FirstPersonPath, and
lockers retain LockerVisual.

Minimum test: reproduce two vent entries in ChallengeMap16, record a video,
and capture both 534 diagnostics. Verify whether the *rendered body* moves
through the vent despite the physical Character dropping near 1.41 seconds,
and whether the end-of-clip relocation visibly snaps or is undone by physics.
This is not proven correct without the game.

## 535 — Stable model-root anchor for native vent replay

The receiving machine's 534 diagnostics (November 11, 2026, remote PID
5884) show two complete native `enter_vent` clips with no failed starts:
218 `ShowVentVisualPath` successes, zero rejections, two successful
endpoint placements and a maximum requested render offset of 1.809m.
Those calls only prove the visual offset was **published**, not that the
final high-resolution pose consumed it. The physically simulated Character
still falls to Y≈27.25 at phases around 1.32–1.38 while the owner stays
near Y≈27.73–27.80. Its gap remains up to 1.81m.

The 534 final-pose hook added the owner-vs-Character offset to the **current
animated root translation**. If the native skeleton translates its own
root, this double-applies the clip's local root displacement. 535 changes
only `NativeVentVisual`: at the first actual final-pose application,
capture a stable root position from the previously captured idle pose.
If the idle final pose is unavailable, latch the first native rendered
root for this entry instead. Subsequent frames keep all animated body
joint rotations/limb animations intact, but the **root translation** is
formed from that stable reference plus the current replicated owner
correction. On clip completion, release the reference together with the
render-only offset. Other full-body NPC actions and locker hiding retain
the exact previous additive behavior.

The new `native_animation_vent_visual` diagnostic distinguishes:
`frames` (offset requests accepted by SetVisualOffset),
`renderFramesApplied` (high-res pose actually modified by the hook),
`stableRootFramesApplied` (pose writes using an anchored reference),
`stableRootIdleReferences` / `stableRootLiveReferences`
(which reference source was available), and
`maxSuppressedRootDeltaYMeters` (largest local native-root Y deviation
that would otherwise have been compounded). If renderFramesApplied is
zero, do **not** adjust root heights further: investigate the layer blend
render path instead. If the suppressed delta stays close to zero, the
root-motion-doubling hypothesis was not responsible for the visual drop.

A header-only portable math test `AnimationSyncVisualRoot` covers the
additive-vs-anchored vertical path, world yaw transform and unchanged
identity. The native MSVC x86 build and real visual behavior still require
validation; no global Y clamp or arbitrary 50cm lift was introduced.

## 536 — Native vent floor support from mapped high-resolution feet

The receiving 535 build (November 11, 2026, PID 1212) confirms both
native interactions completed with no admission failure. In the vent clip,
87 render offsets were requested **and 87 were actually applied**; the
stable root used one retail idle reference, and
`maxSuppressedRootDeltaYMeters=0.0`. This **disproves** the theory that
duplicate native ROOT Y translation caused the remaining sink. The physical
Character still drops from Y=27.63 at phase 1.25 to Y=27.32 at phase 1.37,
then Y=27.05 by phase 2.00 while the owner is approximately Y=27.79.
The native NPC has no replicated vent traversal/collision state.

536 leaves all physics, AI, actual Character transforms, and the native
full-body clip untouched. It uses the launcher's existing signed retarget
mapping from `FEMALEFP` to the remote crew low/high-res skeleton to find
hips, feet and toes **without hardcoded crew bone indices**. In the final
pose only, an explicit forward-kinematics model-space probe measures the
lowest animated foot and hip against the last idle pose. A *vent-only* visual
root lift is then calculated from measured foot penetration and the owner's
upward progress. Feet may trail on the original floor during the first part
of entry; support gradually transitions toward the raised vent floor.
The added lift is bounded to **0.45m total**, and changes are rate-limited
to 0.06m upward / 0.035m downward per rendered frame. An unavailable
skeleton map, missing idle pose, or invalid pose skips the correction.
All other animations and locker offsets continue to use their old policies.

`native_animation_vent_visual` now also exports
`ventMappedFootSamples`, `ventFootProbeSkipped`,
`ventFloorSupportFrames`, `maxVisualFloorLiftMeters`,
`maxDetectedFootDepthMeters`, `maxAnimatedHipDropMeters`,
`lastAnimatedFootY`, and `idleFootReferenceY`.
If foot probes are skipped or depth is zero, this correction will have no
vertical effect: do not raise the body by an arbitrary constant. If the
foot probe finds depth and corrects it yet the model still sinks, the
remaining problem is the visual/physics transform boundary or the placement
of the native clip relative to the intended vent target.

Registered standalone math regression: `AnimationSyncVentGround`.
It was exercised with an independent C++20 compiler. The complete
Windows MSVC x86 build and in-game rendering have not been verified.

Minimum test: first do normal walking and one button interaction to check
for regressions. Then enter the same vent twice, retaining both screenshots
or video and both 536 diagnostic ZIPs. Compare the new floor counters with
the movement at native clip phases 1.25–2.00, and check that neither entrance
nor handoff causes a sudden upward float or jump.
