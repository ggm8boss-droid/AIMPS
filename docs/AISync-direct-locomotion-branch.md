# AISync – Direct NPC locomotion (experimental)

Branch: `feature/aisync-direct-locomotion`

## Authority split

- **Host:** runs the NPC decision tree, pathfinding and physical simulation, publishes position, rotation, motion velocity, gait and flags at ~20 Hz.
- **Client:** does **not** replay native navigation kinds 1/2/3/4/6/7/8/9/11. The client feeds interpolated Host velocity, gait and pose error into the existing retail NPC locomotion/animation cycle; **native root motion and turning remain authoritative for ordinary frame-by-frame movement**. POST no longer teleports for centimetre-size drift: a physical correction is permitted only after **3.5 m** of divergence held for **750 ms**, with a **3 s** cooldown and strict ownership checks.
- **Native actions:** kind 5, reaction/state intents (including 1/2/7 as *cycle exits*, not goals), paired kills, scripts, passages, backstage/frontstage and lifecycle events remain authoritative ordered actions. No physical correction while the Host or native Client cycle owns an action.
- **Lifetime:** samples are scoped by AISync identity, birth and session/world. The timeline resets on incarnation, scope, invalid motion, and actor retirement. A 600 ms stale stream releases the motor rather than extrapolating indefinitely.
- **Other NPCs:** the controller is per eligible AI Character and not Alien-specific. The existing admitted class whitelist remains unchanged; the Facehugger (class 5, not yet certified for these native movement cycles) is deliberately not enabled blindly.

## Important remaining limitation

This first integration uses the already-shipped **v16, 912-byte causal state stream**. Motion is no longer *executed* from navigation commands, but pose delivery still shares the reliable ordered TCP lane with game actions. Under congestion, head-of-line blocking may temporarily freeze the puppet. A dedicated compact, latest-wins pose transport is a separate follow-up change, not claimed here. The 510 fix removes per-frame physics teleports, preserves initial segment velocity, extends bounded extrapolation to 150 ms, avoids per-frame hybrid follower reinitialization and leaves turning to retail during travel. Rare emergency teleports can still have visible consequences. No guarantee of zero visual deviation or 100% sync.

## In-game validation (no unit tests)

1. Two players, one Alien, straight corridor: compare physical location, turning, starting, stopping and gait on both PCs.
2. Reproduce sharp turns, sprint, stairs, elevator and large ping/jitter; verify correction is bounded and no old pose reappears.
3. Validate roar, reaction, scripted animation, paired kill of host and client, vent entry/traversal/exit; no direct pose commits while those actions own motion.
4. Try Android and other **admitted** NPC classes; check eligible native cycle and animation. Do not assume Facehugger support.
5. Trigger death, respawn, unloading/loading, world change, leave/join, host disconnect, PvP Alien possession. Ensure stale/previous-birth positions never apply.
6. Watch for collision clipping, excessive teleports, animation foot sliding, and TCP backlog.

The user builds and runs in-game validation; no automated tests were run or changed for this branch.

## 512 (from 511 diags 194257 Client / 174301 Host)

511 measurements: Client yaw stayed at ~0 while the Host turned (-0.27, 1.18, -0.94), so the Alien slid sideways ("possessed"). A Host velocity sample of 15 m/s for an 8 m/s sprint drove magnitude 20 and a 0.5 s stall. During slow Host walks the Client did not move (1.4 m lag). Host locomotion start/stop constructors (contracts 1/7) rebuilt the Client cycle (move_activate from 55/59).

- Host yaw is forced again on PRE/POST (the interpolated Timeline yaw).
- The direct AI motor uses velocity + bounded positional feedback in both axes. Magnitude is capped at 10.
- Timeline velocity is the interpolated segment's derivative (clamped to 12 m/s), not the Host finite difference. Interpolation delay is 100 ms. Past the extrapolation bound, velocity is zero.
- Host motionTick and velocity use a QPC clock on the GetTickCount64 epoch. Client sampling uses the same clock.
- Contracts 1/2/7 are acknowledged without a native call when the motor is armed and the cycle is ordinary locomotion (`HybridMayConsumeConstructor`). Counter: `directLocomotionConstructorsConsumed`.
- `tests/AISyncHybridMotionTests.cpp` still assumes the 50 ms delay and the 75 ms extrapolation; it was already out of date since 510.

## 513

- The Host replicates the effective gait it plays (M+0x198), not the requested one (M+0x194). In 511 the Client played gait 3 while the Host played gait 2 for 2 s. The Client hook writes +194 and +198 after the retail selection, so the gait is not braked twice.

## 514 – PlayerSync remote player replicas

513 diag (Host 182335 / Client 202332):
- Alien chase drift was 0.04-0.5 m.
- The Host replica of the Client player was 3 m away from the real player at the kill. On the Client, the pair alignment slid the Alien 6.2 m.
- The replica was hard-resynced (teleported) 7 times in 30 s.

Changes:
- Launcher: remote velocity is measured over an 80-250 ms window of source samples. Move direction magnitude is the measured speed (0.25-8 m/s) instead of the NPC gait-table speed (3.45 m/s FastWalk).
- Native: player replicas use the both-axis positional feedback follower (gain 2.5/s, max 4 m/s correction). They never move backwards (at least 25% of the input speed along travel). Cap is 8.

## 515

514 diag (Client 204013 / Host 184016), 167 s session:
- Visually OK; median Alien drift during locomotion was 0.37 m.
- The Host replica of the Client player was hard-resynced 14 times in 170 s (513: 7 times in 30 s).
- Remaining defect: the Client Alien was frozen on one exact point for 4-5 s several times (slow Host walks). Root motion advanced 3-5 cm/frame and `hybrid_stuck_diff` was logged, so the body was blocked by geometry while steering straight at the Host. Each episode ended with the 3.5 m emergency teleport.

Changes:
- Stuck recovery: if the body moved less than 0.15 m in 400 ms while at least 0.6 m from the interpolated Host pose, it is re-placed on that pose (1 s cooldown). Counters: `directPoseStuckCorrections`, `directPoseEmergencyCorrections`.

## 516

515 diag (Client 205545 / Host 185548):
- Median drift was 0.11 m, but the user saw non-fluid motion.
- Measured native speed was 0 in most slow/start phases even with a demand of 1-5 m/s. The body advanced only through 17 stuck snaps of 0.6 m.

Changes:
- The stuck snaps are replaced by a per-frame kinematic assist in the POST reconcile.
- Beyond 0.20 m of planar error, the body moves toward the interpolated Host pose. The step is at most (Host speed + 1.5 m/s)·dt and at most 0.15 m per frame. It stops 0.15 m short, leaving the native motor sole owner inside the dead zone.
- This is the same per-frame physical step AnimationSync uses for clips (`FollowAnimationClipPath`).
- The emergency at 3.5 m is unchanged. Counter: `directPoseAssistFrames`.

## 517

516 diag (Host 210701 / Client 190704):
- Locomotion drift was 0.02-0.36 m throughout, with 922 assist frames.
- The Host replica of the Client player was within 0.07 m of the real player.
- The kill on the Client player failed on the Client. The pair Submit was Rejected (gate 206, refusalSite 101 = Capture carrier validation) 65 times. The Client Alien stayed Idle 3.4 m from the player, and the Client player never died while the Host replica did.
- Site 101 is now split into 1011-1019 to identify the refused field: 1014 = no target peer, 1015 = no participant incarnation, 1016 = birth/class set for a player, 1019 = transition, and so on.

## 518 (full survival run 517: Host 193327 / Client 213331)

- **Vents and scripted clips.** The Client body did not move during vent entry/exit clips (cycle 0x12) or passages (0x4B-0x4D). The Host crossed 15-30 m of vent while the Client stayed at the mouth and played the exit clip there. The 3.5 m emergency then snapped it 15.8/31.7 m onto the player, followed by a kill ("teleported onto me").
  - New `FollowHostActionPose`: during non-paired 0x12/0x4B-0x4D cycles the Client body follows the Host pose. Visible clips move at most 0.15 m/frame; inside a passage at most 0.35 m/frame, with a direct jump beyond 3 m. Counter: `directPoseActionFollowFrames`.
- **Crash (both PCs, Crawl Space door, 7th level load in one process).** allocator[6] (animation heap) returned null during NPC graph construction (6A9360); retail does not check it.
  - `GraphLookupDetour` now probes 16 KB on that heap before construction. If the probe fails, it takes the native missing-resource path.
- **Client shots on the Alien.** Zero-HP hits (flame/revolver on the Alien) were dropped by `PublishClientImpact`, so a Client could not repel the Alien.
  - They are forwarded as channel 3 (zone, pellets). The Host replays the retail zero-damage branch of 007CDCF0: target notification 007C4EC0 and the repel meter at [brain+4238]+6C.
  - The Host Replica's own primary zero-damage copy is suppressed.
  - The C# and native validators accept channel 3.
  - Counters: `impactStimulusForwarded/Applied`, `impactDamagelessDropped`.
- **ChallengeMap11 lift climb-out.** Trigger-volume occupancy typed messages 0x01000000/0x02000000 targeted the player Character, whose id is the same on every peer, and were replayed on the receiver's own player. The Host's exit disabled the Client's climb Use. They are now receiver-local.

## 520 (518 diag: Host 054751 / Client 074748, ChallengeMap16)

- **Host AISync stream lost after the first kill, never reopened.** The Client Alien froze while the Host Alien killed the Client player.
  - Sequence: Host log "lost command coverage (Recovery, reason=4)", loss site 339, `aisync_host_movement_failure` stage 2 with kind 5.
  - Cause: the branch no longer captures Move (kind 1) leases, so every Move frame has `primary=0`, and `Find(0)` returned any lease without a companion. At the end of a Move, while the Alien's kind-5 action lease was live, `EndNativeMove` ran `FinalizeMovement` on that kind-5 lease. Its movement snapshot failed and closed the whole source.
  - Fixes:
    - `Find(0)` returns nothing.
    - `EndNativeMove` finalizes only kinds 1/2/3.
  - The failed lease stayed `unknown`. `RetireNative(true)` skipped it, so `NativeIdle()` stayed false and every same-world re-Offer was unqualified (`hostOffersUnqualified=1`). The Host source stayed silent for the rest of the round.
  - Fix: a lease now records `refsHeld` (every Ref came from a successful `Strong`) and `worldFenced` (set by `RetireNative(false)`). A failed decrement poisons `ref.control=0` and is never retried.
  - The top-level engine-tick `RetireNative(true)` retires same-world fences whose Refs are exactly held, like live leases. The destruction path is unchanged, which keeps it reentrancy-safe.
- **Native actions other than vents/scripted clips (e.g. state 0x57).** Each PC played its own root motion. The Host moved 1.5 m forward while the Client moved 1.3 m back, a peak drift of 1.85 m.
  - `FollowHostActionPose` now also covers non-paired, non-locomotion states.
  - Dead zone 0.25 m, at most 0.10 m/frame toward the interpolated Host pose.

## 522 (520 diag: Host 065403 / Client 085355)

- 520 confirmed the Host source fix: `hostCoverageLosses=0` for the whole round.
- **Stutter.** The 0.20 m dead-zone assist produced a sawtooth:
  - Native root motion stalled (measured speed 0 at a 0.5-1.8 m/s demand) until the error reached 0.20 m.
  - The assist then burst at Host speed + 1.5 m/s back to 0.15 m, and the cycle repeated (973 assist frames).
- **Change.** In locomotion cycles the POST reconcile now places the body on the interpolated Host pose every frame when the planar error is below 0.75 m. That pose is already smoothed by the 100 ms timeline.
  - Beyond 0.75 m: approach at no more than Host speed + 3 m/s, capped at 0.30 m/frame.
  - The native cycle still animates from the demand.
  - The 3.5 m emergency is unchanged.

## 523 (522 diag: Host 075147 / Client 095143)

- **No sync at all because the session dropped.** About 25 s into the round, Host→Client traffic stalled: one AISync send took 1.4 s and 16 packets were queued. The Client engine did not freeze (worst frame 0.8 s at load).
- **Cause.** `AISyncExecutionSender` cancels any write older than 2 s. A cancelled frame write disposes the whole lobby connection (`ProtocolConnection.SendExecutionAsync`), so the Host dropped the Client ("left the lobby").
  - The Client only ever received causal packet 1, so its Alien stayed at spawn.
- **Fix.** The execution sender and the shadow send/ready bounds are raised from 2 s to 10 s, matching `PeerTimeout`.
- The 522 kinematic tracking could not be evaluated in this run.
