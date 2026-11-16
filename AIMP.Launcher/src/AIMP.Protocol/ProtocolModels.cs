using System.Text.Json.Serialization;

namespace AIMP.Protocol;

public sealed record CompatibilityInfo(
    ushort ProtocolVersion,
    string LauncherVersion,
    string DllVersion,
    string AiSha256,
    string ProfileId,
    string CompatibilityHash);

public sealed record HelloPayload(
    ushort ProtocolVersion,
    string DllVersion,
    int ProcessId,
    bool ObservationOnly,
    string Nonce,
    bool AiSyncV1Available = false,
    bool UiHeapSynchronized = false);

public sealed record HelloAckPayload(
    ushort ProtocolVersion,
    string LauncherVersion,
    int ProcessId,
    string Nonce);

public sealed record BuildInfoPayload(
    string AiSha256,
    string ProfileId,
    bool ProfileValidated);

public sealed record NetZReadyPayload(
    string Mode,
    bool NativeReplicationReady,
    IReadOnlyList<string> BlockedGates);

public sealed record SessionConfigPayload(
    Guid SessionId,
    ulong PeerId,
    Guid LauncherPlayerId,
    string DisplayName,
    bool IsHost,
    string HostAddress,
    int HostPort,
    int LevelGeneration,
    bool NativeReplicationEnabled);

// Local Launcher <-> injected DLL lifecycle. This is deliberately distinct
// from SessionConfig: bootstrap IPC proves process compatibility, while this
// payload binds the real multiplayer authority and opens/closes the WS3 world
// bus only after the Launcher has a compatible live WorldContext.
public sealed record WorldSyncLifecyclePayload(
    Guid SessionId,
    ulong PeerId,
    bool IsHost,
    int LevelGeneration,
    bool AuthorityBound,
    bool WorldBusActive,
    string Reason);

public sealed record ErrorPayload(string Code, string Message);

public enum WorldTransactionRecoveryStatusCode : byte
{
    Complete = 0,
    Unavailable = 1,
    ContextRejected = 2
}

// Control-plane only. Gameplay mutations remain WorldNetworkMutation/ROOT v10.
public sealed record WorldTransactionRecoveryRequest(
    Guid SessionId,
    ulong PeerId,
    int WorldGeneration,
    ulong LastCommittedSequence);

public sealed record WorldTransactionRecoveryStatus(
    Guid SessionId,
    ulong PeerId,
    int WorldGeneration,
    WorldTransactionRecoveryStatusCode Status,
    ulong RequestedAfterSequence,
    ulong HostThroughSequence,
    string Reason);

public sealed record LobbyJoinPayload(
    Guid LauncherPlayerId,
    string DisplayName,
    CompatibilityInfo Compatibility,
    long ConnectionNonce);

public sealed record LobbyJoinAcceptedPayload(
    Guid SessionId,
    ulong PeerId,
    int Capacity,
    bool OpaqueRelayEnabled,
    long ConnectionGeneration);

public sealed record LobbyReadyPayload(bool Ready);

public sealed record GameplayReadyPayload(
    Guid SessionId,
    ulong PeerId,
    int LevelGeneration);

// Client -> Host: the sender's own THE_PLAYER reached its terminal death
// boundary during this launch generation.
public sealed record PlayerDeathPayload(
    Guid SessionId,
    ulong PeerId,
    int LevelGeneration);

// Client -> Host: the sender's own player reached a retail level exit
// (GoToFrontend/SwitchLevel) during this launch generation. It is finished,
// not dead: it waits for the remaining participants.
public sealed record PlayerMissionCompletePayload(
    Guid SessionId,
    ulong PeerId,
    int LevelGeneration);

// Host -> Clients: participants of the active generation that finished the
// mission or died. Used for the "waiting for players x/N" screen.
public sealed record RoundProgressPayload(
    Guid SessionId,
    int LevelGeneration,
    int Finished,
    int Dead,
    int Participants);

// Host -> Clients: every participant of the generation is dead or finished.
// Reason "MissionComplete" when at least one finished, else "AllPlayersDead".
// The Host is already back in Lobby; peers show the end message, then return
// to frontend.
public sealed record RoundEndedPayload(
    Guid SessionId,
    int LevelGeneration,
    string Reason,
    int ReturnDelayMilliseconds);

// Client -> Host: the sender's own player collected the world pickup EntityId
// (32-hex WorldEntityId from the level's WorldManifest).
public sealed record ItemPickupRequestPayload(
    Guid SessionId,
    ulong PeerId,
    int LevelGeneration,
    string EntityId);

// Host -> Clients: the first collector of EntityId in this generation. Every
// other participant removes the pickup from its own world.
public sealed record ItemCollectedPayload(
    Guid SessionId,
    int LevelGeneration,
    string EntityId,
    ulong CollectorPeerId);

// ItemSync spawn authority. Kind "owner": a GCIP_WorldPickup spawn result for
// OwnerKey (CATHODE runtime key, 16 hex), Spawn=false when nothing spawns.
// Kind "drop": a player-dropped pickup with a synthetic OwnerKey. Transform is
// the 3x4 row-major pickup transform (translation at [3], [7], [11]).
// Proposal (Client -> Host): PeerId is the sender. Decision (Host -> Clients):
// PeerId is the participant whose proposal won.
public sealed record ItemSpawnPayload(
    Guid SessionId,
    ulong PeerId,
    int LevelGeneration,
    string Kind,
    string OwnerKey,
    bool Spawn,
    string Path,
    int Quantity,
    float[] Transform);

public sealed record PlayerSpawnPositionPayload(
    ulong PeerId,
    float X,
    float Y,
    float Z,
    // Crew representation (PlayerCharacterCatalog) assigned by the Host, so
    // every peer shows this player as the same character. Null: E_RIPLEY.
    string? Character = null);

public sealed record SpawnPositionRequestPayload(
    Guid SessionId,
    Guid RequestId,
    ulong HostPeerId,
    int LevelGeneration);

public sealed record SpawnPositionResponsePayload(
    Guid SessionId,
    Guid RequestId,
    ulong PeerId,
    int LevelGeneration,
    float X,
    float Y,
    float Z);

public sealed record SpawnRemotePlayersPayload(
    Guid SessionId,
    Guid RequestId,
    ulong HostPeerId,
    int LevelGeneration,
    IReadOnlyList<PlayerSpawnPositionPayload> Players);

public enum PlayerMoveType
{
    SlowWalk = 0,
    Walk = 1,
    FastWalk = 2,
    Run = 3
}

public sealed record PlayerPostureSnapshot(
    [property: JsonRequired] bool Crouched,
    [property: JsonRequired] bool DeepCrouched,
    [property: JsonRequired] bool InVent,
    [property: JsonRequired] uint VentIdA,
    [property: JsonRequired] uint VentIdB,
    [property: JsonRequired] ulong TraversalRevision,
    [property: JsonRequired] ushort TraversalIndex,
    [property: JsonRequired] byte TraversalType,
    [property: JsonRequired] uint TraversalOwner,
    [property: JsonRequired] bool TraversalActive);

// Effective emitted beam state; retail battery/reload logic remains authoritative.
public sealed record PlayerTorchSnapshot(
    [property: JsonRequired] bool Enabled,
    float? AimX = null, float? AimY = null, float? AimZ = null);

// Owner-side locker state. The anchor is the hiding spot sampled by the owner;
// peers use it to hide the representation and mark that spot occupied.
public sealed record PlayerHidingSnapshot(
    [property: JsonRequired] bool Hidden,
    float? AnchorX = null, float? AnchorY = null, float? AnchorZ = null,
    PlayerHidingPresentationSnapshot? Presentation = null);

public enum PlayerHidingPhase { Entering = 1, Hidden = 2, Exiting = 3 }
public enum PlayerHidingKind { Tall = 1, Low = 2 }

// The key names the selected instance, never a process address. Elapsed time
// permits late snapshots to settle without replaying an old door/sound cue.
public sealed record PlayerHidingPresentationSnapshot(
    [property: JsonRequired] PlayerHidingPhase Phase,
    [property: JsonRequired] PlayerHidingKind Kind,
    [property: JsonRequired] uint EntityShortGuid,
    [property: JsonRequired] uint HierarchyHash,
    [property: JsonRequired] ulong Transition,
    [property: JsonRequired] uint ElapsedMilliseconds,
    [property: JsonRequired] float X,
    [property: JsonRequired] float Y,
    [property: JsonRequired] float Z,
    [property: JsonRequired] float Yaw);

public sealed record PlayerPositionSnapshotPayload(
    Guid SessionId,
    ulong PeerId,
    int LevelGeneration,
    ulong Sequence,
    long SentAtUnixMilliseconds,
    float X,
    float Y,
    float Z,
    float Yaw,
    PlayerMoveType MoveType,
    float NativeMoveSpeed,
    PlayerPostureSnapshot? Posture = null,
    PlayerTorchSnapshot? Torch = null,
    // Source-owner allocated, monotone within this session/peer. Zero means
    // unavailable. Never a runtime ID or permission to enter a native pair.
    uint CharacterIncarnation = 0,
    PlayerHidingSnapshot? Hiding = null,
    PlayerAnimationSnapshot? Animation = null);

// AnimationSync: the owner's current CATHODE scripted clip (CMD_PlayAnimation
// on THE_PLAYER: buttons, generators, terminals, vents...). Animation is the
// engine's FNV-1a alias; each receiver resolves it in its own representation's
// clip database, so no animation is special-cased. Transition 0 means no clip
// (PlayerAnimationCodec.None); null is an unavailable sample. Elapsed time lets
// a late snapshot be skipped instead of replaying a finished action.
public sealed record PlayerAnimationSnapshot(
    [property: JsonRequired] ulong Transition,
    [property: JsonRequired] uint Animation,
    [property: JsonRequired] uint ElapsedMilliseconds,
    [property: JsonRequired] int StartFrame,
    [property: JsonRequired] int EndFrame,
    [property: JsonRequired] int Loops,
    [property: JsonRequired] float Speed,
    [property: JsonRequired] float BlendIn,
    [property: JsonRequired] float X,
    [property: JsonRequired] float Y,
    [property: JsonRequired] float Z,
    [property: JsonRequired] float Yaw,
    [property: JsonRequired] float PhaseSeconds,
    [property: JsonRequired] float DurationSeconds,
    [property: JsonRequired] uint Attachment0 = 0,
    [property: JsonRequired] uint Attachment1 = 0,
    [property: JsonRequired] uint Attachment2 = 0,
    [property: JsonRequired] uint Attachment3 = 0,
    // The owner's real first-person body at PhaseSeconds (hips and head
    // displacement, left and right hand positions; 12 floats in its body
    // frame). Null when not sampled. Mirrors AnimationSync.h State::effectors.
    IReadOnlyList<float>? Effectors = null);

public enum PlayerActionKind
{
    Aim = 0,
    Primary = 1,
    Reload = 2
}

// Local IPC observation. Addresses and construction serials never leave the PC.
public sealed record NativePlayerHidingStatePayload(
    int SchemaVersion, ulong NativeGeneration, uint Character, ulong Serial, string Presentation);

public sealed record NativePlayerAnimationStatePayload(
    int SchemaVersion, ulong NativeGeneration, uint Character, ulong Serial, string Animation,
    [property: JsonRequired] ulong SampledAtTickMilliseconds);

public sealed record NativePlayerEquipmentStatePayload(
    int SchemaVersion,
    ulong Revision,
    bool Valid,
    int CurrentEquipmentIndex,
    int PreviousEquipmentIndex,
    bool EquipmentActive,
    int PresentationState,
    bool AimHeld,
    bool PrimaryHeld);

public sealed record NativePlayerLifecycleStatePayload(
    int SchemaVersion,
    ulong DeathSequence,
    bool LocalPlayerDead,
    ulong LevelExitSequence = 0,
    ulong SpectatePeer = 0,
    int SpectateCandidates = 0);

public sealed record NativeItemSyncPickupPayload(
    int SchemaVersion,
    ulong Sequence,
    string EntityId);

public sealed record NativeItemSyncSpawnPayload(
    int SchemaVersion,
    ulong Sequence,
    string Kind,
    string OwnerKey,
    bool Spawn,
    string Path,
    int Quantity,
    float[] Transform);

public sealed record NativePlayerActionEventPayload(
    int SchemaVersion,
    ulong EventSequence,
    PlayerActionKind Action,
    bool Active,
    int CurrentEquipmentIndex,
    int PresentationState);

public sealed record PlayerEquipmentSnapshotPayload(
    Guid SessionId,
    ulong PeerId,
    int LevelGeneration,
    ulong Sequence,
    long SentAtUnixMilliseconds,
    int CurrentEquipmentIndex,
    int PreviousEquipmentIndex,
    bool EquipmentActive,
    int PresentationState,
    bool AimHeld,
    bool PrimaryHeld);

public sealed record PlayerActionEventPayload(
    Guid SessionId,
    ulong PeerId,
    int LevelGeneration,
    ulong Sequence,
    long SentAtUnixMilliseconds,
    PlayerActionKind Action,
    bool Active,
    int CurrentEquipmentIndex,
    int PresentationState);

// Cutscene skip never carries a native pointer, input event, or client proposal.
// Only the Host can decide this one-shot gameplay transition.
public sealed record CinematicSkipPayload(Guid SessionId,int LevelGeneration,ulong Sequence);

public sealed record LobbyStartPayload(
    Guid SessionId,
    long StartAtUnixMilliseconds,
    string Level,
    int LevelGeneration,
    bool AlienEnabled = true,
    bool AlienVsSurvivors = false,
    ulong AlienPeerId = 0,
    bool CampaignContinuation = false,
    ulong PreviousMissionSequence = 0,
    CampaignLaunchDescriptor? Campaign = null);

public sealed record LobbyPlayer(
    ulong PeerId,
    Guid LauncherPlayerId,
    string DisplayName,
    bool IsHost,
    bool Ready,
    string? Character = null);

public sealed record LobbySnapshotPayload(
    Guid SessionId,
    string ServerName,
    string Level,
    string State,
    int Capacity,
    int LevelGeneration,
    IReadOnlyList<LobbyPlayer> Players,
    Guid PreparationToken = default,
    IReadOnlyList<LobbyPreparationPayload>? Preparations = null,
    bool WorldPreparationRequired = false,
    bool AlienEnabled = true,
    bool AlienVsSurvivors = false,
    ulong AlienPeerId = 0,
    CampaignLaunchDescriptor? Campaign = null);

public sealed record DiscoveryAnnouncement(
    ushort ProtocolVersion,
    Guid SessionId,
    string ServerName,
    string HostName,
    int PlayerCount,
    int Capacity,
    string Level,
    string State,
    int TcpPort,
    string CompatibilityHash);

public sealed record DiscoveredSession(
    DiscoveryAnnouncement Announcement,
    string Address,
    DateTimeOffset LastSeenUtc);


// A report proves only portable compiled content, never live native readiness.
public sealed record LobbyPreparationPayload(
    Guid SessionId,
    ulong PeerId,
    string Level,
    Guid PreparationToken,
    WorldManifestIdentity? Manifest,
    string ManifestSha256,
    bool Ready,
    string Error,
    string CampaignSaveSha256 = "");

// Sent only after world recovery, native binding and remote players are ready.
public sealed record SessionPreparedPayload(Guid SessionId, ulong PeerId, int LevelGeneration, string MissionSha256 = "");

// Host-only permission to expose gameplay for this exact start generation.
public sealed record SessionReleasePayload(Guid SessionId, int LevelGeneration);
