using AIMP.Protocol;
using System.Globalization;
using System.Security.Cryptography;
using System.Text;

namespace AIMP.Launcher;

internal sealed record MultiplayerSessionView(
    string NetworkState,
    bool IsHost,
    bool Connected,
    bool LocalGameplayReady,
    bool RemoteGameplayReady,
    string RemotePlayerName)
{
    internal static MultiplayerSessionView Disconnected { get; } = new(
        "Disconnected", false, false, false, false, string.Empty);
}

internal sealed partial class MultiplayerSessionCoordinator : IDisposable
{
    private readonly NativeRuntimeController _controller;
    private readonly AISyncSession _aiSync;
    private readonly VoiceSyncSession _voiceSync;
    private readonly LauncherSettingsStore _settingsStore;
    private readonly ScaleformBridgeListener _bridge;
    private readonly LauncherDiagnostics _diagnostics;
    private readonly PerformanceIsolationState _performanceIsolation;
    private readonly SemaphoreSlim _commandGate = new(1, 1);
    private readonly SemaphoreSlim _spawnGate = new(1, 1);
    private readonly CancellationTokenSource _lifetime = new();
    private readonly object _stateGate = new();
    private readonly GameplaySpawnReadinessGate _gameplayReadiness = new();
    private readonly PlayerIncarnationAllocator _playerIncarnations = new();
    private LobbyHost? _host;
    private LobbyClient? _client;
    private LobbySnapshotPayload? _lobby;
    private LobbyStartPayload? _start;
    private LocalPlayerPosition _position;
    private bool _positionAvailable;
    private float _yaw;
    private PlayerMoveType _moveType = PlayerMoveType.Walk;
    private float _nativeMoveSpeed = RemoteMovementPlanner.LegacyWalkFallbackMagnitude;
    private LocalPlayerPosition _previousMovementPosition;
    private DateTimeOffset _previousMovementSampleAtUtc;
    private bool _hasPreviousMovementSample;
    private float _observedLocalPlanarSpeed;
    private bool _uiGameplayLoaded;
    private bool _localGameplayReady;
    private bool _localReadySending;
    private bool _remoteGameplayReady;
    private readonly Dictionary<ulong, uint> _remoteCharacters = new();
    private readonly Dictionary<ulong, RemotePositionTrack> _remotePositions = new();
    private readonly HashSet<ulong> _remoteGameplayReadyPeers = new();
    private readonly HashSet<Guid> _processedSpawnRequests = new();
    private SpawnPositionRequestPayload? _pendingSpawnPositionRequest;
    private bool _spawnPositionSending;
    private bool _spawnCommandCollecting;
    private SpawnRemotePlayersPayload? _pendingSpawnRequest;
    // Host-assigned crew character per peer, from the last accepted roster: an
    // independent respawn must show the same character as the first spawn.
    private readonly Dictionary<ulong, string> _assignedCharacters = new();
    private bool _spawnRequestRunning;
    private bool _remoteSpawnConvergenceRunning;
    private long _remoteSpawnEpoch;
    private readonly record struct RemoteSpawnContext(
        Guid SessionId, int WorldGeneration, int NativeGeneration, int ProcessId, long Epoch,
        ulong Peer,uint Incarnation,Guid ConnectionIdentity);
    private sealed class RemoteSpawnContextChangedException : OperationCanceledException
    {
        internal RemoteSpawnContextChangedException()
            : base("The Character spawn belongs to an ended session or native generation.") { }
    }
    // A Character spawn the engine already accepted keeps loading even when
    // the Launcher stops observing it. Its key is kept per remote peer so a
    // later convergence resumes waiting for it instead of submitting another
    // spawn (each extra submission became an orphan Character in the world).
    private readonly record struct InFlightRemoteSpawn(
        uint PreviousKey, uint? SpawnKey, Guid SessionId, int StartGeneration, int ProcessId,
        DateTimeOffset SubmittedUtc,int NativeGeneration,uint Incarnation,Guid ConnectionIdentity);
    private static readonly TimeSpan InFlightRemoteSpawnLifetime = TimeSpan.FromSeconds(90);
    private readonly Dictionary<ulong, InFlightRemoteSpawn> _inFlightRemoteSpawns = new();
    private readonly Dictionary<ulong, int> _remoteSpawnRetryAttempts = new();
    private readonly Dictionary<ulong, DateTimeOffset> _remoteSpawnRetryNotBeforeUtc = new();
    private PlayerPositionSnapshotPayload? _pendingLocalPosition;
    private readonly Queue<PlayerPositionSnapshotPayload> _pendingLocalPostureEdges = new();
    private PlayerPostureSnapshot? _posture;
    private PlayerPostureSnapshot? _lastQueuedPosture;
    private PlayerTorchSnapshot? _torch;
    private PlayerTorchSnapshot? _lastQueuedTorch;
    private PlayerHidingSnapshot? _hiding;
    private PlayerHidingSnapshot? _lastQueuedHiding;
    private bool _positionSendRunning;
    private ulong _localPositionSequence;
    private NativePlayerEquipmentStatePayload? _localEquipmentState;
    private NativePlayerEquipmentStatePayload? _lastQueuedEquipmentState;
    private DateTimeOffset _nextEquipmentRefreshAtUtc = DateTimeOffset.MinValue;
    private PlayerEquipmentSnapshotPayload? _pendingLocalEquipment;
    private bool _playerSyncSendRunning;
    // Equipment snapshots and action edges share one sequence domain so a
    // delayed durable snapshot can never overwrite a newer Aim/Primary edge.
    private ulong _localPlayerSyncSequence;
    private readonly Queue<PlayerActionEventPayload> _pendingLocalPlayerActions = new();
    private readonly Dictionary<ulong, PlayerEquipmentSnapshotPayload> _remoteEquipment = new();
    private readonly Queue<PlayerActionEventPayload> _pendingRemotePlayerActions = new();
    private readonly Dictionary<ulong, ulong> _remoteLastPlayerSyncSequence = new();
    private bool _remotePlayerActionApplyRunning;
    private readonly Dictionary<ulong, int> _remoteProvisionedEquipment = new();
    private readonly Dictionary<ulong, bool> _remoteAppliedAim = new();
    private readonly Dictionary<ulong, bool> _remoteAppliedPrimary = new();
    private readonly SemaphoreSlim _remoteEquipmentProvisionGate = new(1, 1);
    private readonly SemaphoreSlim _remotePlayerSyncGate = new(1, 1);
    private bool _movementCommandRunning;
    private string _lastMovementFailure = string.Empty;
    private DateTimeOffset _lastMovementFailureLoggedAt;
    private WorldNetworkContext? _localWorldContext;
    private int _worldSessionGeneration;
    private readonly SessionWorldGenerationCounter _worldGenerations = new();
    private int _localWorldRuntimeGeneration;
    private Task<PreparedWorldManifest?>? _automaticWorldManifestPreparation;
    private Task<RemoteEquipmentCatalogSet?>? _remoteEquipmentCatalogPreparation;
    private bool _worldActivationRunning;
    private bool _worldActivationAttempted;
    // A failed activation (manifest still preparing, peer context not ready)
    // is retried during the loading hold instead of leaving WorldSync parked.
    private DateTimeOffset _nextWorldActivationRetryUtc;
    private bool _worldSyncEnabledForSession = true;
    private TaskCompletionSource<WorldMutationResult>? _pendingWorldProbeResult;
    private TaskCompletionSource<IReadOnlyList<WorldNetworkMutationAck>>? _pendingWorldProbeAckResult;
    private HashSet<ulong>? _pendingWorldProbeExpectedPeers;
    private readonly Dictionary<ulong, WorldNetworkMutationAck> _pendingWorldProbeAcks = new();
    private ulong _pendingWorldProbeSequence;
    private DateTimeOffset _nextWorldMutationFaultSummaryAtUtc = DateTimeOffset.MinValue;
    private int _suppressedWorldMutationFaults;
    private DateTimeOffset _nextWorldIntentRejectSummaryAtUtc = DateTimeOffset.MinValue;
    private int _suppressedWorldIntentRejects;
    private string _lastWorldIntentRejectReason = string.Empty;
    private DateTimeOffset _nextWorldIntentAbandonSummaryAtUtc = DateTimeOffset.MinValue;
    private int _suppressedWorldIntentAbandons;
    private string _lastWorldIntentAbandonReason = string.Empty;
    private readonly SemaphoreSlim _worldIntentGate = new(1, 1);
    private readonly SemaphoreSlim _worldMutationApplyGate = new(1, 1);
    private int _queuedWorldIntents;
    private enum WorldBusLifecycleState : byte
    {
        Inactive,
        AwaitingCompatibleWorldContext,
        Active,
        RecoveryRequired
    }
    private Guid _nativeAuthoritySessionId;
    private ulong _nativeAuthorityPeerId;
    private bool _nativeAuthorityIsHost;
    private int _nativeAuthorityGeneration;
    private bool _nativeAuthorityBound;
    private bool _nativeWorldBusActive;
    private readonly record struct AwaitAuthorityCause(ulong SourcePeer, ulong ObservationId);
    private sealed class ClientWorldTransaction(
        ulong startSequence,
        int rootCount,
        WorldNetworkContext worldContext,
        WorldAuthorityContextIdentity? hostAuthorityContext)
    {
        internal ulong StartSequence { get; } = startSequence;
        internal int RootCount { get; } = rootCount;
        internal WorldNetworkContext WorldContext { get; } = worldContext;
        internal WorldAuthorityContextIdentity? HostAuthorityContext { get; } = hostAuthorityContext;
        internal int ReceivedCount { get; set; }
        internal int ResultCount { get; set; }
        internal bool Failed { get; set; }
        internal WorldMutationResultCode FailureCode { get; set; }
        internal ulong FailureSequence { get; set; }
        internal HashSet<AwaitAuthorityCause> LocalAwaitCauses { get; } = new();
        internal TaskCompletionSource<bool> Completion { get; } = new(
            TaskCreationOptions.RunContinuationsAsynchronously);
        internal ulong EndSequence => StartSequence + checked((ulong)RootCount) - 1U;
    }
    private sealed record NativeWorldDispatch(
        ulong NativeDispatchId,
        ulong AuthoritySequence,
        ClientWorldTransaction Transaction);
    private ClientWorldTransaction? _activeWorldTransaction;
    private readonly WorldMutationDispatchWindow _worldDispatchWindow = new(128);
    // Step 5: correlation is exact and local to Launcher <-> DLL. Authority
    // sequence remains bookkeeping only and is never used to guess which native
    // dispatch completed.
    private readonly Dictionary<ulong, NativeWorldDispatch> _worldResultDispatches = new();
    private WorldAuthorityContextIdentity? _currentHostAuthorityContext;
    private WorldAuthorityContextIdentity? _worldRecoveryAuthorityContext;
    private readonly HashSet<AwaitAuthorityCause> _pendingAwaitAuthorityCauses = new();
    private static bool SameWorldContext(WorldNetworkContext left, WorldNetworkContext right) =>
        left.SessionId == right.SessionId
        && left.WorldGeneration == right.WorldGeneration
        && left.Manifest == right.Manifest
        && string.Equals(left.ManifestSha256, right.ManifestSha256,
            StringComparison.OrdinalIgnoreCase);

    private WorldBusLifecycleState GetWorldBusLifecycleStateLocked()
    {
        if (!_worldSyncEnabledForSession || !_nativeAuthorityBound
            || (_host is null && _client is null) || _start is null)
            return WorldBusLifecycleState.Inactive;
        if (_worldRecoveryRequired) return WorldBusLifecycleState.RecoveryRequired;
        if (_worldRecoveryRequestRunning || _worldRecoveryTargetSequence != 0)
            return WorldBusLifecycleState.AwaitingCompatibleWorldContext;
        if (!_nativeWorldBusActive || _localWorldContext is not { Ready: true })
            return WorldBusLifecycleState.AwaitingCompatibleWorldContext;
        if (_host is not null && !_host.AreWorldPeersCompatible())
            return WorldBusLifecycleState.AwaitingCompatibleWorldContext;
        return WorldBusLifecycleState.Active;
    }

    private async Task<bool> SetNativeWorldSyncLifecycleAsync(
        Guid sessionId,
        ulong peerId,
        bool isHost,
        int generation,
        bool authorityBound,
        bool worldBusActive,
        string reason)
    {
        if (sessionId == Guid.Empty || peerId == 0) return false;
        var payload = new WorldSyncLifecyclePayload(
            sessionId,
            peerId,
            isHost,
            Math.Max(0, generation),
            authorityBound,
            authorityBound && worldBusActive,
            reason);
        try
        {
            await _controller.SendWorldSyncLifecycleAsync(payload, _lifetime.Token)
                .ConfigureAwait(false);
            int aiWorldGeneration;
            bool alienEnabled;
            lock (_stateGate)
            {
                aiWorldGeneration = _localWorldContext?.WorldGeneration ?? 0;
                alienEnabled = _start?.AlienEnabled ?? _lobby?.AlienEnabled ?? true;
            }
            await _aiSync.ConfigureAsync(sessionId, isHost, _controller.Snapshot.LevelGeneration,
                aiWorldGeneration, authorityBound, payload.WorldBusActive, peerId, _lifetime.Token, alienEnabled)
                .ConfigureAwait(false);
            await ConfigureThrowableSyncAsync(sessionId, peerId, authorityBound && payload.WorldBusActive)
                .ConfigureAwait(false);
            lock (_stateGate)
            {
                if (authorityBound)
                {
                    _nativeAuthoritySessionId = sessionId;
                    _nativeAuthorityPeerId = peerId;
                    _nativeAuthorityIsHost = isHost;
                    _nativeAuthorityGeneration = payload.LevelGeneration;
                    _nativeAuthorityBound = true;
                    _nativeWorldBusActive = payload.WorldBusActive;
                }
                else
                {
                    _nativeAuthoritySessionId = Guid.Empty;
                    _nativeAuthorityPeerId = 0;
                    _nativeAuthorityIsHost = false;
                    _nativeAuthorityGeneration = 0;
                    _nativeAuthorityBound = false;
                    _nativeWorldBusActive = false;
                }
            }
            _diagnostics.Log("WorldSync",
                authorityBound
                    ? $"WS3 native authority bound: Session={sessionId:N} PeerId={peerId} " +
                      $"Role={(isHost ? "Host" : "Client")} Generation={payload.LevelGeneration} " +
                      $"WorldBus={(payload.WorldBusActive ? "Active" : "Closed")} Reason={reason}."
                    : $"WS3 native authority unbound: Session={sessionId:N} PeerId={peerId} " +
                      $"Role={(isHost ? "Host" : "Client")} Generation={payload.LevelGeneration} " +
                      $"Reason={reason}.");
            return true;
        }
        catch (Exception exception) when (exception is ProtocolException or InvalidOperationException
            or IOException or OperationCanceledException)
        {
            if (!_lifetime.IsCancellationRequested)
                _diagnostics.Log("WorldSync",
                    $"WS3 native lifecycle update failed ({reason}): {exception.Message}");
            return false;
        }
    }

    private async Task CloseNativeWorldBusAsync(string reason)
    {
        Guid sessionId;
        ulong peerId;
        bool isHost;
        int generation;
        lock (_stateGate)
        {
            if (!_nativeAuthorityBound) return;
            sessionId = _nativeAuthoritySessionId;
            peerId = _nativeAuthorityPeerId;
            isHost = _nativeAuthorityIsHost;
            generation = _nativeAuthorityGeneration;
        }
        await SetNativeWorldSyncLifecycleAsync(
            sessionId, peerId, isHost, generation, true, false, reason)
            .ConfigureAwait(false);
    }

    private async Task UnbindNativeWorldSyncAuthorityAsync(string reason)
    {
        Guid sessionId;
        ulong peerId;
        bool isHost;
        int generation;
        lock (_stateGate)
        {
            if (!_nativeAuthorityBound) return;
            sessionId = _nativeAuthoritySessionId;
            peerId = _nativeAuthorityPeerId;
            isHost = _nativeAuthorityIsHost;
            generation = _nativeAuthorityGeneration;
        }
        await SetNativeWorldSyncLifecycleAsync(
            sessionId, peerId, isHost, generation, false, false, reason)
            .ConfigureAwait(false);
    }

    private async Task TryOpenClientWorldBusAfterRecoveryAsync(string reason)
    {
        LobbyClient? client;
        WorldNetworkContext? context;
        lock (_stateGate)
        {
            client = _client;
            context = _localWorldContext;
            if (client is null || context is not { Ready: true }
                || _worldRecoveryRequired || _worldRecoveryRequestRunning
                || _worldRecoveryTargetSequence != 0 || _nativeWorldBusActive)
                return;
        }
        await SetNativeWorldSyncLifecycleAsync(
            context.SessionId, client.PeerId, false, context.WorldGeneration,
            true, true, reason).ConfigureAwait(false);
    }

    private bool AdoptHostAuthorityContextLocked(WorldAuthorityContextIdentity authorityContext)
    {
        if (_currentHostAuthorityContext is { } current && current.Matches(authorityContext))
            return false;

        if (_currentHostAuthorityContext is not null)
        {
            // Logical invalidation only. Native callbacks from the previous
            // authority incarnation may still arrive; NativeDispatchId makes them
            // exact stale results rather than sequence-order tombstones.
            _activeWorldTransaction?.Completion.TrySetResult(false);
            _activeWorldTransaction = null;
            _worldDispatchWindow.Clear();
            _lastCommittedWorldSequence = 0;
            _worldRecoveryTargetSequence = 0;
            _worldRecoveryAwaitSnapshot.Clear();
            _worldRecoveryRequired = false;
            _worldRecoveryAuthorityContext = null;
            _pendingAwaitAuthorityCauses.Clear();
            _worldResumeCheckpoint = null;
        }
        _currentHostAuthorityContext = authorityContext;
        return true;
    }

    private void TrackWorldResultLocked(
        ulong nativeDispatchId,
        ulong authoritySequence,
        ClientWorldTransaction transaction)
    {
        if (nativeDispatchId == 0)
            throw new ProtocolException("NativeDispatchId zero is reserved.");
        if (!_worldResultDispatches.TryAdd(nativeDispatchId,
                new NativeWorldDispatch(nativeDispatchId, authoritySequence, transaction)))
            throw new ProtocolException($"NativeDispatchId {nativeDispatchId} is already pending.");
    }

    private bool TryTakeWorldResultLocked(
        ulong nativeDispatchId,
        out NativeWorldDispatch dispatch)
    {
        if (nativeDispatchId == 0
            || !_worldResultDispatches.Remove(nativeDispatchId, out var found))
        {
            dispatch = null!;
            return false;
        }
        dispatch = found;
        return true;
    }

    private void RemoveWorldResultDispatchLocked(ulong nativeDispatchId)
    {
        if (nativeDispatchId != 0) _worldResultDispatches.Remove(nativeDispatchId);
    }

    private void RemoveWorldResultDispatchesLocked(ClientWorldTransaction transaction)
    {
        if (_worldResultDispatches.Count == 0) return;
        var stale = _worldResultDispatches
            .Where(pair => ReferenceEquals(pair.Value.Transaction, transaction))
            .Select(pair => pair.Key)
            .ToArray();
        foreach (var nativeDispatchId in stale)
            _worldResultDispatches.Remove(nativeDispatchId);
    }

    private sealed record WorldResumeCheckpoint(
        Guid SessionId,
        int WorldGeneration,
        WorldManifestIdentity Manifest,
        ulong LastCommittedSequence,
        bool RecoveryRequired,
        AwaitAuthorityCause[] PendingAwaitAuthorityCauses);
    private WorldResumeCheckpoint? _worldResumeCheckpoint;
    private ulong _lastCommittedWorldSequence;
    private long _worldCommitLogTick;
    private long _worldCommittedTransactionCount;
    private ulong _worldRecoveryTargetSequence;
    private readonly HashSet<AwaitAuthorityCause> _worldRecoveryAwaitSnapshot = new();
    private bool _worldRecoveryRequired;
    private bool _worldRecoveryRequestRunning;
    private int _disposed;

    internal MultiplayerSessionCoordinator(
        NativeRuntimeController controller,
        LauncherSettingsStore settingsStore,
        ScaleformBridgeListener bridge,
        PerformanceIsolationState performanceIsolation,
        GamePreparationService? gamePreparation = null,
        SteamRelay? steamRelay = null)
    {
        _gamePreparation = gamePreparation;
        _steamRelay = steamRelay;
        _controller = controller ?? throw new ArgumentNullException(nameof(controller));
        _settingsStore = settingsStore ?? throw new ArgumentNullException(nameof(settingsStore));
        _bridge = bridge ?? throw new ArgumentNullException(nameof(bridge));
        _bridge.NativeUiReady = () => _controller.Snapshot.IpcConnected;
        _performanceIsolation = performanceIsolation ?? throw new ArgumentNullException(nameof(performanceIsolation));
        _diagnostics = controller.Diagnostics;
        _aiSync = new AISyncSession(controller, _performanceIsolation);
        _voiceSync = new VoiceSyncSession(controller, settingsStore);
        _bridge.MessageReceived += OnBridgeMessage;
        _controller.StateChanged += OnGameRuntimeStateChanged;
        _bridge.ConnectionClosed += OnRoundBridgeConnectionClosed;
        _controller.WorldIntentProduced += OnNativeWorldIntentProduced;
        _controller.WorldMutationCompleted += OnNativeWorldMutationCompleted;
        _controller.PlayerSyncStateProduced += OnNativePlayerSyncStateProduced;
        _controller.PlayerSyncActionProduced += OnNativePlayerSyncActionProduced;
        _controller.PlayerLifecycleStateChanged += OnNativePlayerLifecycleStateChanged;
        InitializeMissionSync();
        _controller.MissionSignalProduced += OnNativeMissionSignalProduced;
        _controller.ItemSyncPickupProduced += OnNativeItemSyncPickupProduced;
        _controller.ItemSyncSpawnProduced += OnNativeItemSyncSpawnProduced;
        _controller.ThrowableProduced += OnNativeThrowableProduced;
        InitializeSteamRelay();
    }

    internal async Task ApplyPerformanceIsolationModuleAsync(
        PerformanceIsolationModule module, bool enabled)
    {
        switch (module)
        {
            case PerformanceIsolationModule.AISync:
                await _aiSync.SetEnabledAsync(enabled, _lifetime.Token).ConfigureAwait(false);
                return;

            case PerformanceIsolationModule.PositionNetworkTransmit:
                _performanceIsolation.SetEnabled(module, enabled);
                if (!enabled)
                {
                    lock (_stateGate) ClearPendingLocalPositions();
                }
                return;

            case PerformanceIsolationModule.RemoteMovement:
                if (!enabled)
                {
                    _performanceIsolation.SetEnabled(module, false);
                    uint[] characters;
                    lock (_stateGate) characters = _remoteCharacters.Values.Distinct().ToArray();
                    await StopRemoteCharactersBestEffortAsync(characters).ConfigureAwait(false);
                    await _controller.SetPerformanceIsolationModuleAsync(
                        PerformanceIsolationNativeModules.RemoteMovementRuntime, false, _lifetime.Token)
                        .ConfigureAwait(false);
                }
                else
                {
                    await _controller.SetPerformanceIsolationModuleAsync(
                        PerformanceIsolationNativeModules.RemoteMovementRuntime, true, _lifetime.Token)
                        .ConfigureAwait(false);
                    _performanceIsolation.SetEnabled(module, true);
                }
                return;

            case PerformanceIsolationModule.EquipmentSync:
            case PerformanceIsolationModule.ActionSync:
                _performanceIsolation.SetEnabled(module, enabled);
                if (module == PerformanceIsolationModule.ActionSync)
                {
                    lock (_stateGate) _lastQueuedEquipmentState = null;
                }
                if (!enabled && module == PerformanceIsolationModule.ActionSync)
                {
                    KeyValuePair<ulong, uint>[] characters;
                    lock (_stateGate)
                    {
                        _pendingLocalPlayerActions.Clear();
                        _pendingRemotePlayerActions.Clear();
                        characters = _remoteCharacters.ToArray();
                    }
                    foreach (var pair in characters)
                        await StopRemotePlayerActionsAsync(pair.Key, pair.Value).ConfigureAwait(false);
                }
                if (!enabled && module == PerformanceIsolationModule.EquipmentSync)
                {
                    lock (_stateGate)
                    {
                        _pendingLocalEquipment = null;
                        _lastQueuedEquipmentState = null;
                    }
                }
                var observerEnabled =
                    _performanceIsolation.IsEnabled(PerformanceIsolationModule.EquipmentSync) ||
                    _performanceIsolation.IsEnabled(PerformanceIsolationModule.ActionSync);
                await _controller.SetPerformanceIsolationModuleAsync(
                    PerformanceIsolationNativeModules.PlayerSyncObserver, observerEnabled, _lifetime.Token)
                    .ConfigureAwait(false);
                return;

            case PerformanceIsolationModule.WorldSync:
                if (!enabled)
                {
                    _performanceIsolation.SetEnabled(module, false);
                    lock (_stateGate) _worldSyncEnabledForSession = false;
                    await CloseNativeWorldBusAsync("performance-isolation").ConfigureAwait(false);
                    await _controller.SetPerformanceIsolationModuleAsync(
                        PerformanceIsolationNativeModules.WorldSyncHooks, false, _lifetime.Token)
                        .ConfigureAwait(false);
                }
                else
                {
                    await _controller.SetPerformanceIsolationModuleAsync(
                        PerformanceIsolationNativeModules.WorldSyncHooks, true, _lifetime.Token)
                        .ConfigureAwait(false);
                    _performanceIsolation.SetEnabled(module, true);
                    lock (_stateGate)
                    {
                        _worldSyncEnabledForSession = true;
                        _worldActivationAttempted = false;
                    }
                    await TryActivateAutomaticWorldSyncAsync(_controller.Snapshot).ConfigureAwait(false);
                }
                return;

            case PerformanceIsolationModule.EngineThreadEvidence:
                await _controller.SetPerformanceIsolationModuleAsync(
                    PerformanceIsolationNativeModules.EngineEvidence, enabled, _lifetime.Token)
                    .ConfigureAwait(false);
                _performanceIsolation.SetEnabled(module, enabled);
                return;

            case PerformanceIsolationModule.CharacterLifecycleCapture:
                await _controller.SetPerformanceIsolationModuleAsync(
                    PerformanceIsolationNativeModules.CharacterLifecycle, enabled, _lifetime.Token)
                    .ConfigureAwait(false);
                _performanceIsolation.SetEnabled(module, enabled);
                return;

            case PerformanceIsolationModule.GameThreadPumps:
                if (!enabled)
                {
                    KeyValuePair<ulong, uint>[] characters;
                    lock (_stateGate) characters = _remoteCharacters.ToArray();
                    await StopRemoteCharactersBestEffortAsync(
                        characters.Select(pair => pair.Value)).ConfigureAwait(false);
                    foreach (var pair in characters)
                        await StopRemotePlayerActionsAsync(pair.Key, pair.Value).ConfigureAwait(false);
                }
                await _controller.SetPerformanceIsolationModuleAsync(
                    PerformanceIsolationNativeModules.GameThreadPumps, enabled, _lifetime.Token)
                    .ConfigureAwait(false);
                _performanceIsolation.SetEnabled(module, enabled);
                return;

            default:
                _performanceIsolation.SetEnabled(module, enabled);
                return;
        }
    }

    internal event EventHandler<MultiplayerSessionView>? StateChanged;

    internal MultiplayerSessionView State
    {
        get { lock (_stateGate) return CreateView(); }
    }

    internal Task<GameCommandResultPayload> ExecuteLauncherCommandAsync(
        string command,
        IReadOnlyDictionary<string, string> arguments)
    {
        if (string.Equals(command, LauncherGameCommandNames.SpawnConnectedPlayers,
                StringComparison.Ordinal))
            return RequestRemotePlayerSpawnsAsync();
        if (string.Equals(command, LauncherGameCommandNames.WorldNetProbe,
                StringComparison.Ordinal))
            return RequestWorldNetProbeAsync(arguments);
        if (string.Equals(command, LauncherGameCommandNames.ProvisionRemoteEquipment,
                StringComparison.Ordinal))
            return ProvisionRemoteEquipmentManualAsync(arguments);
        throw new InvalidOperationException("The launcher command is not supported.");
    }

    internal void UpdateLocalRuntime(
        NativeRuntimeSnapshot runtime,
        bool positionAvailable,
        LocalPlayerPosition position,
        bool yawAvailable,
        float yaw,
        bool movementProfileAvailable,
        LocalPlayerMovementProfile movementProfile,
        string positionError = "")
    {
        var now = DateTimeOffset.UtcNow;
        InvalidateWorldContextForRuntimeGeneration(runtime.LevelGeneration);
        UpdateObservedTransform(
            now, positionAvailable, position, yawAvailable, yaw,
            movementProfileAvailable, movementProfile);
        ReportSessionPreparationProgress(runtime, now, positionError);
        if (TryQueueLocalPosition(now)) _ = PumpLocalPositionsAsync();
        if (TryQueueLocalEquipment(now)) _ = PumpLocalPlayerSyncAsync();
        _ = TryAdvanceGameplayAsync(runtime);
        _ = TryConvergeRemoteCharactersAsync(runtime, now);
        _ = TryAdvanceRemoteMovementAsync(runtime, now);
        UpdateVoiceSession(runtime);
    }

    private void UpdateObservedTransform(
        DateTimeOffset now,
        bool positionAvailable,
        LocalPlayerPosition position,
        bool yawAvailable,
        float yaw,
        bool movementProfileAvailable,
        LocalPlayerMovementProfile movementProfile)
    {
        lock (_stateGate)
        {
            _positionAvailable = positionAvailable;
            _posture = positionAvailable ? movementProfile.Posture : null;
            _torch = positionAvailable ? movementProfile.Torch : null;
            _hiding = positionAvailable ? movementProfile.Hiding : null;
            if (positionAvailable)
            {
                UpdateLocalMovementSpeed(now, position);
                _position = position;
            }
            else
            {
                _hasPreviousMovementSample = false;
                _observedLocalPlanarSpeed = 0F;
            }
            if (yawAvailable) _yaw = yaw;
            if (movementProfileAvailable)
            {
                _moveType = movementProfile.ResolveEffectiveMoveType(
                    _observedLocalPlanarSpeed);
                // Use transform speed only to identify the effective gait. The
                // locomotion magnitude itself comes from the game's retail
                // move_type table so timer jitter cannot modulate the remote
                // Character every 16 ms. Position feedback in the v10.6
                // follower remains responsible for the small catch-up/brake.
                _nativeMoveSpeed = movementProfile.SpeedFor(_moveType);
            }
            _gameplayReadiness.Observe(now, positionAvailable, position);
        }
    }

    private void UpdateLocalMovementSpeed(
        DateTimeOffset now,
        LocalPlayerPosition position)
    {
        if (!_hasPreviousMovementSample)
        {
            _previousMovementPosition = position;
            _previousMovementSampleAtUtc = now;
            _hasPreviousMovementSample = true;
            _observedLocalPlanarSpeed = 0F;
            return;
        }

        var seconds = (float)(now - _previousMovementSampleAtUtc).TotalSeconds;
        var dx = position.X - _previousMovementPosition.X;
        var dz = position.Z - _previousMovementPosition.Z;
        var distance = MathF.Sqrt(dx * dx + dz * dz);
        _previousMovementPosition = position;
        _previousMovementSampleAtUtc = now;

        if (!float.IsFinite(seconds) || seconds is < 0.005F or > 0.25F
            || !float.IsFinite(distance) || distance >= 2.0F)
        {
            _observedLocalPlanarSpeed = 0F;
            return;
        }

        var instantaneous = distance / seconds;
        if (!float.IsFinite(instantaneous) || instantaneous > 8.0F)
        {
            _observedLocalPlanarSpeed = 0F;
            return;
        }

        // Fast attack so RUN is published in one or two samples, gentler
        // release so timer jitter cannot make the gait flap WALK/RUN every tick.
        var alpha = instantaneous >= _observedLocalPlanarSpeed ? 0.80F : 0.45F;
        _observedLocalPlanarSpeed +=
            (instantaneous - _observedLocalPlanarSpeed) * alpha;
        if (_observedLocalPlanarSpeed < 0.02F) _observedLocalPlanarSpeed = 0F;
    }

    public void Dispose()
    {
        if (Interlocked.Exchange(ref _disposed, 1) != 0) return;
        _lifetime.Cancel();
        _aiSync.Dispose();
        _voiceSync.Dispose();
        _bridge.MessageReceived -= OnBridgeMessage;
        _controller.StateChanged -= OnGameRuntimeStateChanged;
        _bridge.ConnectionClosed -= OnRoundBridgeConnectionClosed;
        _controller.WorldIntentProduced -= OnNativeWorldIntentProduced;
        _controller.WorldMutationCompleted -= OnNativeWorldMutationCompleted;
        _controller.PlayerSyncStateProduced -= OnNativePlayerSyncStateProduced;
        _controller.PlayerSyncActionProduced -= OnNativePlayerSyncActionProduced;
        _controller.PlayerLifecycleStateChanged -= OnNativePlayerLifecycleStateChanged;
        _controller.MissionSignalProduced -= OnNativeMissionSignalProduced;
        _controller.ItemSyncPickupProduced -= OnNativeItemSyncPickupProduced;
        _controller.ItemSyncSpawnProduced -= OnNativeItemSyncSpawnProduced;
        _controller.ThrowableProduced -= OnNativeThrowableProduced;
        StopNetworkAsync("Launcher closed").GetAwaiter().GetResult();
        Task.WhenAll(_missionReportTask, _missionApplyTask).GetAwaiter().GetResult();
        _commandGate.Dispose();
        _spawnGate.Dispose();
        _remoteEquipmentProvisionGate.Dispose();
        _remotePlayerSyncGate.Dispose();
        _lifetime.Dispose();
    }

    private void OnBridgeMessage(object? sender, ScaleformBridgeEvent value)
    {
        if (Volatile.Read(ref _disposed) != 0) return;
        // Process on the reader before its EOF notification; never queue this
        // behind the command gate while a round return is waiting for the HUD.
        if ((value.Message.Type.Equals("HELLO", StringComparison.OrdinalIgnoreCase)
                || value.Message.Type.Equals("PICKUP_OVERLAY_LOADED", StringComparison.OrdinalIgnoreCase))
            && value.Channel == "command" && value.ConnectionId != Guid.Empty
            && value.Message.Get("source") == "pickup_overlay" && value.Message.Get("phase") == "gameplay")
        {
            lock (_stateGate) { _loadingOverlayConnection = value.ConnectionId; _loadingOverlayAcknowledged = false; }
        }
        if (value.Message.Type.Equals("SESSION_LOADING_ACK", StringComparison.OrdinalIgnoreCase))
        { AcceptSessionLoadingAck(value); return; }
        if (value.Message.Type.Equals("ROUND_RETURN_READY", StringComparison.OrdinalIgnoreCase))
        { AcceptRoundReturnReady(value); return; }
        if (TryAcceptMissionResultsOpened(value)) return;
        if (TryHandleImmediateMenuMessage(value)) return;
        _ = HandleBridgeMessageAsync(value.Message);
    }

    private async Task HandleBridgeMessageAsync(ScaleformBridgeMessage message)
    {
        await _commandGate.WaitAsync().ConfigureAwait(false);
        try { await DispatchBridgeMessageAsync(message).ConfigureAwait(false); }
        catch (Exception exception) { await ReportBridgeFailureAsync(exception).ConfigureAwait(false); }
        finally { _commandGate.Release(); }
    }

    private Task DispatchBridgeMessageAsync(ScaleformBridgeMessage message) =>
        message.Type.Trim().ToUpperInvariant() switch
        {
            "HELLO" => HandleHelloAsync(message),
            "VOICE_GET_SETTINGS" or "VOICE_SAVE_SETTINGS" => HandleVoiceBridgeAsync(message),
            "DEVELOPER_MODE" => SetDeveloperModeAsync(message),
            "PICKUP_OVERLAY_LOADED" => MarkGameplayUiLoadedAsync(message),
            "CREATE_LOBBY" => CreateLobbyAsync(message),
            "JOIN_DIRECT_IP" => JoinDirectAsync(),
            "READY" => SetReadyAsync(message.Get("ready", "0") == "1"),
            "START_GAME" => StartGameAsync(message),
            "START_GAME_ACK" => LogOnlyAsync("Start-game UI acknowledgement received."),
            "CHARACTER_SELECTED" => LogCharacterSelectionAsync(message),
            "REFRESH_SERVERS" or "JOIN_SERVER" => HandleBrowserCommandAsync(message),
            "LEAVE" => StopNetworkAsync("Lobby left from the game UI"),
            _ => LogOnlyAsync($"No multiplayer action is attached to {message.Type}.")
        };

    private Task HandleBrowserCommandAsync(ScaleformBridgeMessage message) =>
        string.Equals(message.Type, "REFRESH_SERVERS", StringComparison.OrdinalIgnoreCase)
            ? SendEmptyServerListAsync()
            : SendErrorAsync("Server discovery is disabled; use Direct Connection.");

    private async Task HandleHelloAsync(ScaleformBridgeMessage message)
    {
        // The frontend retries HELLO until native heap synchronization is ready.
        // Never hold the command gate for injection or queue user actions here.
        if (!await AwaitMenuHelloReadyAsync(message).ConfigureAwait(false)) return;
        await SendConfigAsync().ConfigureAwait(false);
        await SendCurrentLobbyAsync().ConfigureAwait(false);
        var source = message.Get("source", "frontend");
        var phase = message.Get("phase", "unknown");
        if (IsGameplaySource(source, phase)) await MarkGameplayUiLoadedAsync(message).ConfigureAwait(false);
        else await SendFrontendReadyAsync().ConfigureAwait(false);
    }

    private static bool IsGameplaySource(string source, string phase) =>
        string.Equals(source, "pickup_overlay", StringComparison.OrdinalIgnoreCase)
        || string.Equals(phase, "gameplay", StringComparison.OrdinalIgnoreCase);

    private async Task MarkGameplayUiLoadedAsync(ScaleformBridgeMessage message)
    {
        MarkGameplayUiLoaded(DateTimeOffset.UtcNow);
        LobbyStartPayload? preparing;
        lock (_stateGate) { preparing = _loadingHoldStart; _loadingOverlayAcknowledged = false; }
        if (preparing is not null) await SendSessionLoadingAsync(preparing, true, "Preparing world and waiting for players...").ConfigureAwait(false);
        _diagnostics.Log("Multiplayer",
            "Gameplay UI loaded; waiting for a stable local player position.");
        await SendStatusAsync("Waiting for local player spawn").ConfigureAwait(false);
        await TryAdvanceGameplayAsync(_controller.Snapshot).ConfigureAwait(false);
    }

    private void MarkGameplayUiLoaded(DateTimeOffset now)
    {
        lock (_stateGate)
        {
            if (!_uiGameplayLoaded) _gameplayReadiness.MarkGameplayUiLoaded(now);
            _uiGameplayLoaded = true;
        }
    }

    private async Task CreateLobbyAsync(ScaleformBridgeMessage message)
    {
        EnsureMenuConnectionAvailable();
        await StopNetworkAsync("Creating a new lobby").ConfigureAwait(false);
        var settings = _settingsStore.Load();
        ValidateDeveloperLobby(settings, message);
        var options = CreateHostOptions(settings, message);
        var host = await StartMenuHostAsync(options).ConfigureAwait(false);
        _diagnostics.Log("Multiplayer", $"Lobby hosted on port {host.BoundPort}. Alien enabled={options.AlienEnabled}.");
        await SendMenuLobbyOpenedAsync("host").ConfigureAwait(false);
        await SendStatusAsync("Lobby created").ConfigureAwait(false);
        await StartSteamHostingAsync(host).ConfigureAwait(false);
    }

    private async Task<LobbyHost> StartHostAsync(LobbyHostOptions options)
    {
        var host = new LobbyHost(options);
        AttachHost(host);
        lock (_stateGate) _host = host;
        try
        {
            host.Start();
            await SetNativeWorldSyncLifecycleAsync(
                host.SessionId, host.HostPeerId, true, 0, true, false, "lobby-host-created")
                .ConfigureAwait(false);
            return host;
        }
        catch
        {
            DetachHost(host);
            lock (_stateGate) if (ReferenceEquals(_host, host)) _host = null;
            await host.DisposeAsync().ConfigureAwait(false);
            throw;
        }
    }

    private static LobbyHostOptions CreateHostOptions(
        LauncherSettings settings,
        ScaleformBridgeMessage message) => new(
        ResolveServerName(settings, message),
        settings.PlayerName,
        settings.PlayerId,
        settings.Port,
        ResolveCapacity(message),
        ResolveLevel(message),
        LobbyCompatibility.Create(settings),
        RequireWorldPreparation: true,
        AlienEnabled: ReadLobbyAlienEnabled(message));

    private static string ResolveServerName(
        LauncherSettings settings,
        ScaleformBridgeMessage message)
    {
        var requested = message.Get("serverName").Trim();
        var fallback = $"{settings.PlayerName}'s Lobby";
        return requested.Length is >= 1 and <= ProtocolConstants.MaximumServerNameLength
            ? requested : fallback;
    }

    private static int ResolveCapacity(ScaleformBridgeMessage message) =>
        // The menu stepper defaults to "2 Players", which capped lobbies at two ("Lobby is full").
        // The menu selection is ignored for now: every lobby uses the default capacity (4).
        // Remove this line to honor the menu selection again.
        ProtocolConstants.DefaultLobbyCapacity;

    // Original menu-driven capacity resolution (currently unused, see ResolveCapacity).
    private static int ResolveRequestedCapacity(ScaleformBridgeMessage message) =>
        int.TryParse(message.Get("maxPlayers"), NumberStyles.Integer,
            CultureInfo.InvariantCulture, out var requested)
            ? Math.Clamp(requested, ProtocolConstants.MinimumLobbyCapacity,
                ProtocolConstants.MaximumLobbyCapacity)
            : ProtocolConstants.DefaultLobbyCapacity;

    private static string ResolveLevel(ScaleformBridgeMessage message)
    {
        var mapId = message.Get("mapId").Trim();
        if (mapId.Length is >= 1 and <= 128) return mapId;
        var mapName = message.Get("mapName").Trim();
        return mapName.Length is >= 1 and <= 128 ? mapName : "ChallengeMap16";
    }

    private async Task JoinDirectAsync()
    {
        EnsureMenuConnectionAvailable();
        await StopNetworkAsync("Joining a direct lobby").ConfigureAwait(false);
        var settings = _settingsStore.Load();
        var endpoint = await ResolveJoinEndpointAsync(settings).ConfigureAwait(false);
        var options = new LobbyClientOptions(endpoint.Host, endpoint.Port,
            settings.PlayerId, settings.PlayerName, LobbyCompatibility.Create(settings));
        LobbyClient client;
        try { client = await LobbyClient.ConnectAsync(options).ConfigureAwait(false); }
        catch { CloseSteamClientTunnels("join failed"); throw; }
        await CompleteMenuJoinAsync(client).ConfigureAwait(false);
        await SendMenuLobbyOpenedAsync("client").ConfigureAwait(false);
        await SendStatusAsync($"Connected to {endpoint.Label}")
            .ConfigureAwait(false);
    }

    private async Task SetReadyAsync(bool ready)
    {
        LobbyHost? host;
        LobbyClient? client;
        lock (_stateGate) { host = _host; client = _client; }
        if (host is not null)
        {
            _diagnostics.Log("Multiplayer", "Host readiness is implicit; READY was ignored.");
            return;
        }
        if (client is null) throw new InvalidOperationException(
            "Join a lobby before changing ready state.");
        await client.SetReadyAsync(ready).ConfigureAwait(false);
        _diagnostics.Log("Multiplayer", $"Client lobby ready state: {ready}.");
    }

    private async Task StartGameAsync(ScaleformBridgeMessage message)
    {
        LobbyHost? host;
        lock (_stateGate) host = _host;
        if (host is null) throw new InvalidOperationException("Only the lobby host can start the game.");
        host.SetLevel(ResolveLevel(message));
        host.RefreshWorldPreparation();
        await AwaitLobbyPreparationAsync(host).ConfigureAwait(false);
        if (!host.CanStart) throw new InvalidOperationException(
            "Every connected client must be Ready before the game can start.");
        await host.StartGameAsync(DateTimeOffset.UtcNow.AddMilliseconds(500))
            .ConfigureAwait(false);
    }

    private async Task<GameCommandResultPayload> RequestRemotePlayerSpawnsAsync()
    {
        var target = GetSpawnCommandTarget();
        try
        {
            var hostPosition = GetLocalSpawnPosition(target.Host.HostPeerId);
            var payload = await target.Host.RequestRemotePlayerSpawnsAsync(
                target.Generation, hostPosition, _lifetime.Token).ConfigureAwait(false);
            _diagnostics.Log("Multiplayer",
                $"Host broadcast remote-player spawn request {payload.RequestId:N}.");
            return CreateSpawnBroadcastResult(payload);
        }
        finally { lock (_stateGate) _spawnCommandCollecting = false; }
    }

    private async Task<GameCommandResultPayload> RequestWorldNetProbeAsync(
        IReadOnlyDictionary<string, string> arguments)
    {
        if (!arguments.TryGetValue("manifestPath", out var manifestPath)
            || string.IsNullOrWhiteSpace(manifestPath))
            throw new InvalidOperationException("Select the AIMPWM manifest to probe.");

        LobbyHost host;
        LobbyStartPayload start;
        HashSet<ulong> expectedPeers;
        lock (_stateGate)
        {
            host = _host ?? throw new InvalidOperationException(
                "Only the lobby host can execute WORLD_NET_PROBE.");
            start = _start ?? throw new InvalidOperationException(
                "Start the multiplayer session before WORLD_NET_PROBE.");
            if ((_lobby?.Players.Count ?? 0) < 2)
                throw new InvalidOperationException("At least one remote player must be connected.");
            if (!_localGameplayReady || !_remoteGameplayReady)
                throw new InvalidOperationException(
                    "Wait until every connected player is ready in gameplay before WORLD_NET_PROBE.");
            expectedPeers = _lobby!.Players
                .Where(player => player.PeerId != host.HostPeerId)
                .Select(player => player.PeerId)
                .ToHashSet();
        }
        var runtime = _controller.Snapshot;
        if (!runtime.IpcConnected || !runtime.EngineThreadReady || runtime.LevelGeneration <= 0)
            throw new InvalidOperationException(
                "Wait until the host DLL and engine thread are ready before WORLD_NET_PROBE.");

        var prepared = WorldManifestSessionLoader.Load(manifestPath);
        var load = await _controller.ExecuteCommandAsync(
            GameCommandNames.WorldManifestLoad,
            new Dictionary<string, string>(StringComparer.Ordinal)
            {
                ["manifestPath"] = prepared.Path
            },
            _lifetime.Token).ConfigureAwait(false);
        if (!load.Executed)
            throw new InvalidOperationException(
                $"WORLD_MANIFEST_LOAD failed: {load.StatusCode} - {load.Message}");

        WorldNetworkContext context;
        lock (_stateGate)
        {
            var sameWorld = _localWorldContext is { Ready: true } current
                && _localWorldRuntimeGeneration == runtime.LevelGeneration
                && current.Manifest == prepared.Identity
                && string.Equals(current.ManifestSha256, prepared.Sha256,
                    StringComparison.OrdinalIgnoreCase);
            if (!sameWorld)
                _worldSessionGeneration = _worldGenerations.Next(host.SessionId);
            context = new WorldNetworkContext(
                host.SessionId,
                host.HostPeerId,
                _worldSessionGeneration,
                prepared.Identity,
                prepared.Sha256,
                true);
            _localWorldContext = context;
            _localWorldRuntimeGeneration = _controller.Snapshot.LevelGeneration;
            _pendingWorldProbeResult = new TaskCompletionSource<WorldMutationResult>(
                TaskCreationOptions.RunContinuationsAsynchronously);
            _pendingWorldProbeAckResult =
                new TaskCompletionSource<IReadOnlyList<WorldNetworkMutationAck>>(
                    TaskCreationOptions.RunContinuationsAsynchronously);
            _pendingWorldProbeExpectedPeers = expectedPeers;
            _pendingWorldProbeAcks.Clear();
            _pendingWorldProbeSequence = 0;
        }
        try
        {
            await host.SetHostWorldContextAsync(context, _lifetime.Token).ConfigureAwait(false);
            await host.WaitForWorldCompatibilityAsync(TimeSpan.FromSeconds(10), _lifetime.Token)
                .ConfigureAwait(false);

            var probe = await _controller.ExecuteCommandAsync(
                GameCommandNames.WorldNetProbe,
                new Dictionary<string, string>(StringComparer.Ordinal)
                {
                    ["entityId"] = prepared.ProbeEntity.ToString()
                },
                _lifetime.Token).ConfigureAwait(false);
            if (!probe.Executed)
                throw new InvalidOperationException(
                    $"Native WORLD_NET_PROBE failed: {probe.StatusCode} - {probe.Message}");

            Task<WorldMutationResult> pending;
            Task<IReadOnlyList<WorldNetworkMutationAck>> pendingAcks;
            lock (_stateGate)
            {
                pending = (_pendingWorldProbeResult ?? throw new InvalidOperationException(
                    "WORLD_NET_PROBE local completion was lost.")).Task;
                pendingAcks = (_pendingWorldProbeAckResult ?? throw new InvalidOperationException(
                    "WORLD_NET_PROBE remote acknowledgement tracking was lost.")).Task;
            }
            var result = await pending.WaitAsync(TimeSpan.FromSeconds(10), _lifetime.Token)
                .ConfigureAwait(false);
            var acks = await pendingAcks.WaitAsync(TimeSpan.FromSeconds(10), _lifetime.Token)
                .ConfigureAwait(false);
            var localAccepted = result.Code == WorldMutationResultCode.Accepted
                && result.EntityResolved && result.Dispatched;
            var remoteAccepted = acks.Count(ack =>
                ack.Result.Code == WorldMutationResultCode.Accepted
                && ack.Result.EntityResolved && ack.Result.Dispatched);
            var success = localAccepted && remoteAccepted == expectedPeers.Count;
            var status = success ? "WORLD_NET_PROBE_COMPLETE" : "WORLD_NET_PROBE_REJECTED";
            var message =
                $"sent=1 localAccepted={(localAccepted ? 1 : 0)} sequence={result.Sequence} " +
                $"localResolved={(result.EntityResolved ? 1 : 0)} localResult={result.Code} " +
                $"remoteExpected={expectedPeers.Count} remoteReceived={acks.Count} " +
                $"remoteAccepted={remoteAccepted} remoteResolved={acks.Count(ack => ack.Result.EntityResolved)}. " +
                "No environment gameplay was executed.";
            return new GameCommandResultPayload(
                ProtocolConstants.ExperimentalSchemaVersion,
                Guid.NewGuid(),
                _controller.Snapshot.LevelGeneration,
                Guid.Empty,
                LauncherGameCommandNames.WorldNetProbe,
                status,
                message,
                success);
        }
        finally
        {
            ClearPendingWorldProbeState();
        }
    }

    private void OnNativeWorldIntentProduced(object? sender, WorldIntent intent) =>
        _ = ForwardNativeWorldIntentAsync(intent);

    private async Task ForwardNativeWorldIntentAsync(WorldIntent intent)
    {
        ulong awaitAuthorityObservationId = 0;
        if (intent.Operation == WorldReplicationConstants.CompositePublicInputOperation
            && WorldPublicActionCodec.TryDecode(intent.Payload, out var outboundRoot)
            && outboundRoot.RetailRoot
            && (outboundRoot.Flags & WorldPublicActionCodec.AwaitAuthority) != 0)
        {
            awaitAuthorityObservationId = outboundRoot.ObservationId;
        }

        // A native ROOT emitted while the authoritative World bus is closed was
        // never admitted into a multiplayer transaction. Retire its local
        // observation/reservation without turning that lifecycle condition into
        // a Host rejection or recovery-required event.
        WorldBusLifecycleState readinessState;
        lock (_stateGate)
        {
            if (!_worldSyncEnabledForSession) return;
            readinessState = GetWorldBusLifecycleStateLocked();
        }
        if (readinessState != WorldBusLifecycleState.Active)
        {
            await AbandonUnengagedPublicRootAsync(
                intent, $"world-bus-{readinessState}").ConfigureAwait(false);
            return;
        }

        var handedToTransport = false;
        var retiredWithoutTransport = false;
        var gateHeld = false;
        var receivedAt = Environment.TickCount64;
        var deadline = receivedAt + 10000;
        var pendingCount = Interlocked.Increment(ref _queuedWorldIntents);
        try
        {
            if (pendingCount > 256)
                throw new ProtocolException("WS3 authority queue full; resynchronization required.");
            gateHeld = await _worldIntentGate.WaitAsync(10000, _lifetime.Token).ConfigureAwait(false);
            if (!gateHeld || Environment.TickCount64 >= deadline)
                throw new ProtocolException("WS3 authority wait expired; resynchronization required.");
            LobbyHost? host;
            LobbyClient? client;
            LobbyStartPayload? start;
            WorldNetworkContext? context;
            while (true)
            {
                lock (_stateGate)
                {
                    host = _host;
                    client = _client;
                    start = _start;
                    context = _localWorldContext;
                    if (!_worldSyncEnabledForSession)
                        throw new WorldBusClosedException(
                            "WorldSync was disabled while a ROOT was awaiting transport.");
                    if (GetWorldBusLifecycleStateLocked() != WorldBusLifecycleState.Active)
                        throw new WorldBusClosedException(
                            $"World bus left Active while a ROOT was awaiting transport ({GetWorldBusLifecycleStateLocked()}).");
                }
                if (intent.LevelGeneration != _controller.Snapshot.LevelGeneration)
                    throw new WorldBusClosedException(
                        "Native WorldIntent generation became stale before transport handoff.");
                if (start is not null && context is { Ready: true }
                    && (host is null || host.AreWorldPeersCompatible())) break;
                if (Environment.TickCount64 >= deadline)
                    throw new ProtocolException("WS3 authority/context unavailable at deadline; resynchronization required.");
                await Task.Delay(50, _lifetime.Token).ConfigureAwait(false);
            }
            if (context is null)
                throw new WorldBusClosedException("WorldContext disappeared before transport handoff.");
            var peerId = host?.HostPeerId ?? client?.PeerId ?? 0;
            if (peerId == 0 || intent.Manifest != context.Manifest)
                throw new WorldBusClosedException(
                    "Native WorldIntent context became stale or incompatible before transport handoff.");
            AwaitAuthorityCause? awaitAuthorityCause = null;
            if (awaitAuthorityObservationId != 0)
            {
                awaitAuthorityCause = new AwaitAuthorityCause(peerId, awaitAuthorityObservationId);
                lock (_stateGate) _pendingAwaitAuthorityCauses.Add(awaitAuthorityCause.Value);
            }
            if (intent.Operation == WorldReplicationConstants.CompositePublicInputOperation
                && WorldPublicActionCodec.TryDecode(intent.Payload, out var action) && action.RetailRoot)
            {
                var payload = intent.Payload.ToArray();
                var delay = Math.Max(0.0, action.RelativeDelaySeconds -
                    (Environment.TickCount64 - receivedAt) / 1000.0);
                System.Buffers.Binary.BinaryPrimitives.WriteDoubleLittleEndian(payload.AsSpan(32, 8), delay);
                intent = intent with { Payload = payload };
            }
            var authenticated = intent with { SourcePeer = peerId };
            var envelope = new WorldNetworkIntent(
                context.SessionId,
                context.WorldGeneration,
                authenticated);
            if (host is not null)
            {
                handedToTransport = true;
                await host.SubmitWorldIntentAsync(envelope, _lifetime.Token).ConfigureAwait(false);
            }
            else if (client is not null)
            {
                handedToTransport = true;
                await client.SendWorldIntentAsync(envelope, _lifetime.Token).ConfigureAwait(false);
                if (awaitAuthorityCause is { } pendingCause)
                    _ = WatchAwaitAuthorityAsync(client, context, pendingCause);
            }
            else
                throw new ProtocolException("No active multiplayer transport exists for WorldIntent.");
        }
        catch (WorldBusClosedException exception)
        {
            if (!_lifetime.IsCancellationRequested)
                await AbandonUnengagedPublicRootAsync(intent, exception.Message).ConfigureAwait(false);
            // The item was never handed to the multiplayer transport, so the
            // explicit abandonment above is its complete disposition.
            retiredWithoutTransport = true;
        }
        catch (Exception exception) when (exception is ProtocolException or InvalidOperationException
            or IOException or OperationCanceledException)
        {
            if (!_lifetime.IsCancellationRequested)
            {
                LogWorldIntentReject(exception.Message);
                // A client write failure after handoff is ambiguous: the Host may
                // already have accepted the intent. Never "release" an
                // AwaitAuthority takeover based on that ambiguity; ask the Host
                // journal from the durable checkpoint instead.
                LobbyClient? recoveryClient;
                WorldNetworkContext? recoveryContext;
                lock (_stateGate)
                {
                    recoveryClient = handedToTransport && awaitAuthorityObservationId != 0
                        ? _client : null;
                    recoveryContext = recoveryClient is not null ? _localWorldContext : null;
                }
                if (recoveryClient is not null && recoveryContext is { Ready: true })
                    _ = RequestWorldRecoveryAsync(recoveryClient, recoveryContext, "ambiguous-intent-send");
            }
        }
        finally
        {
            // Native reserves before fan-out. Every local early return must
            // retire that reservation, including a closed compatibility barrier.
            // Once handed to transport, only an explicit authority rejection is
            // conclusive: an I/O failure may follow acceptance and an echo in flight.
            try
            {
                if (!handedToTransport && !retiredWithoutTransport)
                    await ReleaseRejectedPublicRootAsync(intent).ConfigureAwait(false);
            }
            finally
            {
                if (gateHeld) _worldIntentGate.Release();
                Interlocked.Decrement(ref _queuedWorldIntents);
            }
        }
    }

    private sealed class WorldBusClosedException : InvalidOperationException
    {
        internal WorldBusClosedException(string message) : base(message) { }
    }

    private void LogWorldIntentAbandon(string reason)
    {
        var now = DateTimeOffset.UtcNow;
        int count;
        string lastReason;
        bool shouldLog;
        lock (_stateGate)
        {
            _suppressedWorldIntentAbandons++;
            _lastWorldIntentAbandonReason = reason;
            shouldLog = now >= _nextWorldIntentAbandonSummaryAtUtc;
            count = shouldLog ? _suppressedWorldIntentAbandons : 0;
            lastReason = _lastWorldIntentAbandonReason;
            if (shouldLog)
            {
                _suppressedWorldIntentAbandons = 0;
                _nextWorldIntentAbandonSummaryAtUtc = now.AddSeconds(2);
            }
        }
        if (shouldLog)
            _diagnostics.Log("WorldSync",
                $"WS3 pre-bus ROOTs abandoned without authority rejection: count={count} last={lastReason}");
    }

    private async Task AbandonUnengagedPublicRootAsync(WorldIntent intent, string reason)
    {
        if (intent.Operation != WorldReplicationConstants.CompositePublicInputOperation
            || intent.RequestedKind != WorldMutationKind.Event
            || !WorldPublicActionCodec.TryDecode(intent.Payload, out _, out _, out var retailRoot)
            || !retailRoot) return;
        LogWorldIntentAbandon(reason);
        try
        {
            await _controller.SendWorldIntentAbandonedAsync(intent, _lifetime.Token)
                .ConfigureAwait(false);
        }
        catch (Exception exception) when (exception is ProtocolException or InvalidOperationException
            or IOException or OperationCanceledException)
        {
            if (!_lifetime.IsCancellationRequested)
                _diagnostics.Log("WorldSync",
                    $"Pre-bus ROOT observation could not be retired: {exception.Message}");
        }
    }

    private async Task WatchAwaitAuthorityAsync(
        LobbyClient client,
        WorldNetworkContext context,
        AwaitAuthorityCause cause)
    {
        try
        {
            await Task.Delay(TimeSpan.FromSeconds(5), _lifetime.Token).ConfigureAwait(false);
            lock (_stateGate)
            {
                if (_worldRecoveryRequired
                    || !_pendingAwaitAuthorityCauses.Contains(cause)
                    || _localWorldContext is not { Ready: true } current
                    || current.SessionId != context.SessionId
                    || current.WorldGeneration != context.WorldGeneration)
                    return;
            }
            await RequestWorldRecoveryAsync(client, context, "AwaitAuthority-timeout")
                .ConfigureAwait(false);
        }
        catch (OperationCanceledException) when (_lifetime.IsCancellationRequested)
        {
        }
    }

    private void OnWorldIntentRejected(object? sender, WorldNetworkIntent intent) =>
        _ = ReleaseRejectedPublicRootAsync(intent.Intent);

    private void LogWorldIntentReject(string reason)
    {
        var now = DateTimeOffset.UtcNow;
        int count;
        string lastReason;
        bool shouldLog;
        lock (_stateGate)
        {
            _suppressedWorldIntentRejects++;
            _lastWorldIntentRejectReason = reason;
            shouldLog = now >= _nextWorldIntentRejectSummaryAtUtc;
            count = shouldLog ? _suppressedWorldIntentRejects : 0;
            lastReason = _lastWorldIntentRejectReason;
            if (shouldLog)
            {
                _suppressedWorldIntentRejects = 0;
                _nextWorldIntentRejectSummaryAtUtc = now.AddSeconds(2);
            }
        }
        if (shouldLog)
        {
            _diagnostics.Log("WorldSync",
                $"WorldIntent transport rejects summarized: count={count} last={lastReason}");
        }
    }

    private async Task ReleaseRejectedPublicRootAsync(WorldIntent intent)
    {
        if (intent.Operation != WorldReplicationConstants.CompositePublicInputOperation
            || intent.RequestedKind != WorldMutationKind.Event
            || !WorldPublicActionCodec.TryDecode(intent.Payload, out _, out _, out var retailRoot)
            || !retailRoot) return;
        try
        {
            if (WorldPublicActionCodec.TryDecode(intent.Payload, out var action))
            {
                var awaitAuthority = (action.Flags & WorldPublicActionCodec.AwaitAuthority) != 0;
                lock (_stateGate)
                {
                    if (awaitAuthority)
                    {
                        var sourcePeer = intent.SourcePeer != 0
                            ? intent.SourcePeer
                            : _client?.PeerId ?? _host?.HostPeerId ?? 0;
                        if (sourcePeer != 0)
                            _pendingAwaitAuthorityCauses.Remove(
                                new AwaitAuthorityCause(sourcePeer, action.ObservationId));
                    }
                }
                _diagnostics.Log("WorldSync",
                    $"WS3 cause {action.ObservationId:X16} rejected by Host and skipped; " +
                    $"awaitAuthority={awaitAuthority}. This element may be unsynchronized; WorldSync continues without replay.");
            }
            await _controller.SendWorldIntentRejectedAsync(intent, _lifetime.Token)
                .ConfigureAwait(false);
        }
        catch (Exception exception) when (exception is ProtocolException or InvalidOperationException
            or IOException or OperationCanceledException)
        {
            if (!_lifetime.IsCancellationRequested)
                _diagnostics.Log("WorldSync", $"Rejected ROOT observation could not be released: {exception.Message}");
        }
    }

    private void OnWorldMutationAccepted(object? sender, WorldHostAcceptedMutation accepted)
    {
        bool changed;
        lock (_stateGate) changed = AdoptHostAuthorityContextLocked(accepted.AuthorityContext);
        if (changed)
            _diagnostics.Log("WorldSync",
                $"WS3 Host authority context adopted: generation={accepted.AuthorityContext.WorldGeneration} " +
                $"epoch={accepted.AuthorityContext.Epoch} sha256={accepted.AuthorityContext.ManifestSha256}.");
        _ = ApplyNetworkWorldMutationAsync(accepted.Mutation, accepted.AuthorityContext);
    }

    private void OnWorldMutationReceived(object? sender, WorldNetworkMutation mutation) =>
        _ = ApplyNetworkWorldMutationAsync(mutation, null);

    private static bool TryDescribeAuthorityTransaction(
        WorldNetworkMutation networkMutation,
        out ulong startSequence,
        out int rootCount,
        out int rootOrdinal)
    {
        startSequence = networkMutation.Mutation.Sequence;
        rootCount = 1;
        rootOrdinal = 0;
        if (networkMutation.Mutation.Operation != WorldReplicationConstants.CompositePublicInputOperation
            || networkMutation.Mutation.Kind != WorldMutationKind.Event
            || !WorldPublicActionCodec.TryDecode(networkMutation.Mutation.Payload, out var root)
            || !root.RetailRoot)
            return true;
        if (root.AuthorityTransaction == 0
            || networkMutation.Mutation.Sequence != root.AuthorityTransaction + root.RootOrdinal)
            return false;
        startSequence = root.AuthorityTransaction;
        rootCount = root.RootCount;
        rootOrdinal = root.RootOrdinal;
        return true;
    }

    private async Task ApplyNetworkWorldMutationAsync(
        WorldNetworkMutation networkMutation,
        WorldAuthorityContextIdentity? hostAuthorityContext)
    {
        await _worldMutationApplyGate.WaitAsync(_lifetime.Token).ConfigureAwait(false);
        ClientWorldTransaction? transaction = null;
        var lastMember = false;
        try
        {
            lock (_stateGate)
            {
                if (!_worldSyncEnabledForSession) return;
            }
            WorldNetworkContext? context;
            LobbyStartPayload? start;
            LobbyClient? client;
            LobbyHost? host;
            lock (_stateGate)
            {
                context = _localWorldContext;
                start = _start;
                client = _client;
                host = _host;
                if (_pendingWorldProbeResult is not null
                    && networkMutation.Mutation.Operation == WorldReplicationConstants.NetworkProbeOperation)
                    _pendingWorldProbeSequence = networkMutation.Mutation.Sequence;
            }
            if (context is null || !context.Ready || start is null
                || networkMutation.SessionId != context.SessionId
                || networkMutation.WorldGeneration != context.WorldGeneration
                || networkMutation.Mutation.Manifest != context.Manifest)
                throw new ProtocolException("Authoritative WorldMutation context is incompatible.");
            if (hostAuthorityContext is { } expectedHostContext
                && (host is null
                    || !expectedHostContext.Matches(context)
                    || !host.IsCurrentWorldAuthorityContext(expectedHostContext)))
            {
                _diagnostics.Log("WorldSync",
                    $"WS3 stale Host mutation dispatch ignored before native enqueue: " +
                    $"sequence={networkMutation.Mutation.Sequence} generation={expectedHostContext.WorldGeneration} " +
                    $"epoch={expectedHostContext.Epoch}.");
                return;
            }
            var runtimeGeneration = _controller.Snapshot.LevelGeneration;
            if (runtimeGeneration <= 0 || runtimeGeneration != _localWorldRuntimeGeneration)
                throw new ProtocolException("Local LevelGeneration changed before WorldMutation dispatch.");
            if (!TryDescribeAuthorityTransaction(networkMutation,
                    out var transactionStart, out var transactionRoots, out var rootOrdinal))
                throw new ProtocolException("Authoritative transaction metadata is invalid.");

            // Leave headroom in the native 256-ROOT FIFO. Wait only when the
            // window is full, rather than paying one IPC round trip per ROOT.
            if (rootOrdinal == 0)
            {
                while (true)
                {
                    Task<bool>? capacityWait;
                    lock (_stateGate)
                        capacityWait = _worldDispatchWindow.CapacityWait(transactionRoots);
                    if (capacityWait is null) break;
                    await capacityWait.WaitAsync(TimeSpan.FromSeconds(10), _lifetime.Token)
                        .ConfigureAwait(false);
                }
            }

            var requestRecovery = false;
            var windowHighWaterChanged = false;
            var windowPendingRoots = 0;
            lock (_stateGate)
            {
                if (networkMutation.Mutation.Sequence <= _lastCommittedWorldSequence)
                    return; // clean committed retransmission
                if (_worldRecoveryRequired)
                {
                    if (hostAuthorityContext is { } currentHostAuthority
                        && _worldRecoveryAuthorityContext is { } failedHostAuthority
                        && !failedHostAuthority.Matches(currentHostAuthority))
                    {
                        _worldRecoveryRequired = false;
                        _worldRecoveryAuthorityContext = null;
                    }
                    else
                    {
                        return; // fail-stop until a new authority context is established
                    }
                }

                transaction = _activeWorldTransaction;
                if (transaction is not null
                    && hostAuthorityContext is { } incomingHostContext
                    && transaction.HostAuthorityContext is { } activeHostContext
                    && !activeHostContext.Matches(incomingHostContext))
                {
                    transaction.Completion.TrySetResult(false);
                    _activeWorldTransaction = null;
                    transaction = null;
                }
                if (transaction is not null && transaction.ReceivedCount < transaction.RootCount)
                {
                    var exactNext = transaction.StartSequence == transactionStart
                        && transaction.RootCount == transactionRoots
                        && rootOrdinal == transaction.ReceivedCount
                        && networkMutation.Mutation.Sequence == transaction.StartSequence + checked((ulong)rootOrdinal);
                    var freshPrefix = transaction.StartSequence == transactionStart
                        && transaction.RootCount == transactionRoots
                        && rootOrdinal == 0
                        && networkMutation.Mutation.Sequence == transactionStart;
                    if (freshPrefix && transaction.ReceivedCount != 0)
                    {
                        // The journal may retransmit ordinal-0 while an older
                        // prefix is still buffered. No gameplay can have run from
                        // an incomplete prefix, so replace the assembly in place.
                        // Do not erase native-dispatch correlation here. The
                        // replaced prefix may still emit a late result, and its
                        // sequence can be reused by the fresh prefix/context.
                        transaction.Completion.TrySetResult(false);
                        transaction = new ClientWorldTransaction(
                            transactionStart, transactionRoots, context, hostAuthorityContext);
                        _activeWorldTransaction = transaction;
                    }
                    else if (!exactNext)
                    {
                        requestRecovery = true;
                    }
                }
                if (transaction is null)
                {
                    if (transactionStart != _worldDispatchWindow.NextSequence(_lastCommittedWorldSequence) || rootOrdinal != 0)
                    {
                        // Never start from a suffix. A retransmitted ordinal-0
                        // from the Host journal is the only safe way to rebuild an
                        // uncommitted transaction.
                        requestRecovery = true;
                    }
                    else
                    {
                        transaction = new ClientWorldTransaction(
                            transactionStart, transactionRoots, context, hostAuthorityContext);
                        _activeWorldTransaction = transaction;
                    }
                }
                if (!requestRecovery && transaction is not null)
                {
                    if (transaction.StartSequence != transactionStart
                        || transaction.RootCount != transactionRoots
                        || rootOrdinal != transaction.ReceivedCount
                        || networkMutation.Mutation.Sequence != transaction.StartSequence + checked((ulong)rootOrdinal))
                        throw new ProtocolException("Authoritative transaction ordering is not contiguous.");
                    if (networkMutation.Mutation.Operation == WorldReplicationConstants.CompositePublicInputOperation
                        && WorldPublicActionCodec.TryDecode(networkMutation.Mutation.Payload, out var observedRoot)
                        && observedRoot.RetailRoot
                        && (observedRoot.Flags & WorldPublicActionCodec.AwaitAuthority) != 0)
                    {
                        var cause = new AwaitAuthorityCause(
                            networkMutation.Mutation.SourcePeer, observedRoot.ObservationId);
                        if (_pendingAwaitAuthorityCauses.Contains(cause))
                            transaction.LocalAwaitCauses.Add(cause);
                    }
                    transaction.ReceivedCount++;
                    lastMember = transaction.ReceivedCount == transaction.RootCount;
                    if (lastMember)
                    {
                        var previousHighWater = _worldDispatchWindow.HighWater;
                        _worldDispatchWindow.Add(transaction.StartSequence, transaction.RootCount,
                            transaction.Completion);
                        windowPendingRoots = _worldDispatchWindow.PendingRoots;
                        windowHighWaterChanged = previousHighWater < _worldDispatchWindow.HighWater
                            && _worldDispatchWindow.HighWater % 32 == 0;
                        _activeWorldTransaction = null;
                    }
                }
            }
            if (requestRecovery)
            {
                if (client is not null)
                    _ = RequestWorldRecoveryAsync(client, context, "sequence-gap");
                else
                    MarkWorldRecoveryRequired(
                        "Host local authority sequence gap.", transactionStart,
                        hostAuthorityContext);
                return;
            }

            if (transaction is null)
                throw new ProtocolException("Authoritative transaction tracker disappeared before native dispatch.");

            if (windowHighWaterChanged)
                _diagnostics.Log("WorldSync",
                    $"WS3 dispatch pipeline: pendingRoots={windowPendingRoots} capacity=128 " +
                    $"admittedThrough={transaction.EndSequence}; commits remain native-confirmed.");

            // Re-check immediately before native enqueue. A context reset can
            // happen while this async task waited on the apply gate.
            if (hostAuthorityContext is { } dispatchHostContext
                && (host is null || !host.IsCurrentWorldAuthorityContext(dispatchHostContext)))
            {
                lock (_stateGate)
                {
                    transaction.Completion.TrySetResult(false);
                    if (ReferenceEquals(_activeWorldTransaction, transaction))
                        _activeWorldTransaction = null;
                }
                _diagnostics.Log("WorldSync",
                    $"WS3 stale Host mutation dispatch ignored at native enqueue: " +
                    $"sequence={networkMutation.Mutation.Sequence} generation={dispatchHostContext.WorldGeneration} " +
                    $"epoch={dispatchHostContext.Epoch}.");
                return;
            }

            var localMutation = networkMutation.Mutation with
            {
                LevelGeneration = checked((uint)runtimeGeneration)
            };
            var nativeDispatchId = _controller.ReserveWorldMutationDispatchId();
            lock (_stateGate)
                TrackWorldResultLocked(
                    nativeDispatchId, networkMutation.Mutation.Sequence, transaction);
            try
            {
                await _controller.SendWorldMutationAsync(
                        localMutation, nativeDispatchId, _lifetime.Token)
                    .ConfigureAwait(false);
            }
            catch
            {
                lock (_stateGate)
                    RemoveWorldResultDispatchLocked(nativeDispatchId);
                throw;
            }

            if (lastMember)
                _ = WatchWorldTransactionCommitAsync(transaction);
        }
        catch (Exception exception) when (exception is ProtocolException or InvalidOperationException
            or IOException or OperationCanceledException or OverflowException or TimeoutException)
        {
            if (!_lifetime.IsCancellationRequested)
            {
                MarkWorldRecoveryRequired(
                    $"WorldMutation dispatch failed: {exception.Message}",
                    transaction?.StartSequence ?? networkMutation.Mutation.Sequence,
                    transaction?.HostAuthorityContext ?? hostAuthorityContext);
                _diagnostics.Log("WorldSync", $"WorldMutation rejected before DLL dispatch: {exception.Message}");
            }
        }
        finally
        {
            _worldMutationApplyGate.Release();
        }
    }

    private async Task WatchWorldTransactionCommitAsync(ClientWorldTransaction transaction)
    {
        try
        {
            await transaction.Completion.Task.WaitAsync(TimeSpan.FromSeconds(10), _lifetime.Token)
                .ConfigureAwait(false);
        }
        catch (TimeoutException)
        {
            lock (_stateGate)
            {
                if (_localWorldContext is not { } current
                    || !SameWorldContext(current, transaction.WorldContext)
                    || !_worldDispatchWindow.Contains(transaction.Completion.Task)) return;
            }
            MarkWorldRecoveryRequired(
                $"Transaction {transaction.StartSequence} did not reach native commit before timeout.",
                transaction.StartSequence, transaction.HostAuthorityContext);
            transaction.Completion.TrySetResult(false);
        }
        catch (OperationCanceledException) { }
    }

    private void MarkWorldRecoveryRequired(
        string reason,
        ulong failedAuthorityTransaction = 0,
        WorldAuthorityContextIdentity? hostAuthorityContext = null)
    {
        LobbyHost? host;
        lock (_stateGate) host = _host;

        // Step 4.3: a Host failure may mutate authority/local fail-stop state only
        // if the transaction's immutable authority epoch is still current.
        if (host is not null)
        {
            if (hostAuthorityContext is not { } expected
                || !host.MarkWorldAuthorityUnhealthy(
                    expected, failedAuthorityTransaction, reason))
            {
                _diagnostics.Log("WorldSync",
                    $"WS3 stale Host fail-stop ignored: transaction={failedAuthorityTransaction} " +
                    $"reason={reason}");
                return;
            }

            lock (_stateGate)
            {
                if (_host != host || !host.IsCurrentWorldAuthorityContext(expected))
                {
                    _diagnostics.Log("WorldSync",
                        $"WS3 Host fail-stop became stale before Coordinator state update: " +
                        $"transaction={failedAuthorityTransaction} epoch={expected.Epoch}.");
                    return;
                }
                _worldRecoveryRequired = true;
                _worldRecoveryAuthorityContext = expected;
            }
        }
        else
        {
            lock (_stateGate)
            {
                _worldRecoveryRequired = true;
                _worldRecoveryAuthorityContext = null;
            }
        }

        _diagnostics.Log("WorldSync",
            $"WS3 transaction recovery stopped: {reason} Rejoin/resynchronization is required; no partially applied transaction is replayed.");
        _ = CloseNativeWorldBusAsync("recovery-required");
    }

    private async Task RequestWorldRecoveryAsync(
        LobbyClient client,
        WorldNetworkContext context,
        string reason)
    {
        ulong checkpoint;
        lock (_stateGate)
        {
            if (_worldRecoveryRequired || _worldRecoveryRequestRunning
                || _localWorldContext is not { Ready: true } current
                || current.SessionId != context.SessionId
                || current.WorldGeneration != context.WorldGeneration)
                return;
            _worldRecoveryRequestRunning = true;
            checkpoint = _lastCommittedWorldSequence;
            _worldRecoveryAwaitSnapshot.Clear();
            foreach (var cause in _pendingAwaitAuthorityCauses)
                _worldRecoveryAwaitSnapshot.Add(cause);
        }
        try
        {
            await CloseNativeWorldBusAsync($"recovery-request:{reason}")
                .ConfigureAwait(false);
            var request = new WorldTransactionRecoveryRequest(
                client.SessionId, client.PeerId, context.WorldGeneration, checkpoint);
            await client.SendWorldTransactionRecoveryRequestAsync(request, _lifetime.Token)
                .ConfigureAwait(false);
            _diagnostics.Log("WorldSync",
                $"Requested authoritative transaction recovery after committed sequence {checkpoint} ({reason}).");
        }
        catch (Exception exception) when (exception is IOException or ProtocolException
            or InvalidOperationException or OperationCanceledException)
        {
            lock (_stateGate) _worldRecoveryRequestRunning = false;
            if (!_lifetime.IsCancellationRequested)
                _diagnostics.Log("WorldSync", $"World recovery request failed: {exception.Message}");
        }
    }

    private void OnWorldRecoveryStatusReceived(object? sender, WorldTransactionRecoveryStatus status)
    {
        ulong committed;
        var missingAwaitAfterRecovery = false;
        lock (_stateGate)
        {
            _worldRecoveryRequestRunning = false;
            committed = _lastCommittedWorldSequence;
            if (_localWorldContext is not { Ready: true } context
                || status.SessionId != context.SessionId
                || status.WorldGeneration != context.WorldGeneration)
                return;
            if (status.Status != WorldTransactionRecoveryStatusCode.Complete)
            {
                _worldRecoveryRequired = true;
                // An unavailable/context-rejected recovery is terminal for this
                // generation. AwaitAuthority causes must not remain pending or
                // trigger delayed gameplay retry; the next safe action is rejoin
                // or a genuinely new WorldContext generation.
                _pendingAwaitAuthorityCauses.Clear();
                _worldRecoveryAwaitSnapshot.Clear();
                _worldRecoveryTargetSequence = 0;
            }
            else
            {
                _worldRecoveryTargetSequence = status.HostThroughSequence;
                if (_lastCommittedWorldSequence >= _worldRecoveryTargetSequence)
                {
                    missingAwaitAfterRecovery =
                        _worldRecoveryAwaitSnapshot.Any(_pendingAwaitAuthorityCauses.Contains);
                    if (missingAwaitAfterRecovery) _worldRecoveryRequired = true;
                    _worldRecoveryTargetSequence = 0;
                    _worldRecoveryAwaitSnapshot.Clear();
                }
            }
        }
        if (status.Status == WorldTransactionRecoveryStatusCode.Complete)
        {
            _diagnostics.Log("WorldSync",
                missingAwaitAfterRecovery
                    ? "Host journal reached the requested checkpoint but at least one pre-disconnect AwaitAuthority cause was never authority-assigned; rejoin/resynchronization is required."
                    : $"Host recovery suffix accepted: checkpoint={committed} hostThrough={status.HostThroughSequence}.");
            if (!missingAwaitAfterRecovery)
                _ = TryOpenClientWorldBusAfterRecoveryAsync("recovery-complete");
        }
        else
        {
            _diagnostics.Log("WorldSync",
                $"Host recovery unavailable ({status.Status}): {status.Reason} Rejoin/resynchronization is required.");
            _ = CloseNativeWorldBusAsync("recovery-unavailable");
        }
    }

    private bool TryCommitCoordinatorTransactionLocked(ClientWorldTransaction transaction)
    {
        if (_worldRecoveryRequired
            || transaction.StartSequence != _lastCommittedWorldSequence + 1U
            || _localWorldContext is not { Ready: true } current
            || !SameWorldContext(current, transaction.WorldContext))
            return false;

        _lastCommittedWorldSequence = transaction.EndSequence;
        foreach (var cause in transaction.LocalAwaitCauses)
            _pendingAwaitAuthorityCauses.Remove(cause);
        if (ReferenceEquals(_activeWorldTransaction, transaction))
            _activeWorldTransaction = null;
        _worldResumeCheckpoint = new WorldResumeCheckpoint(
            current.SessionId,
            current.WorldGeneration,
            current.Manifest,
            _lastCommittedWorldSequence,
            false,
            _pendingAwaitAuthorityCauses.ToArray());
        if (_worldRecoveryTargetSequence != 0
            && _lastCommittedWorldSequence >= _worldRecoveryTargetSequence)
        {
            if (_worldRecoveryAwaitSnapshot.Any(_pendingAwaitAuthorityCauses.Contains))
                _worldRecoveryRequired = true;
            _worldRecoveryTargetSequence = 0;
            _worldRecoveryAwaitSnapshot.Clear();
        }
        return !_worldRecoveryRequired;
    }

    private void OnNativeWorldMutationCompleted(object? sender, WorldMutationResult result)
    {
        LobbyHost? host;
        LobbyClient? client;
        WorldNetworkContext? context;
        ClientWorldTransaction? tracked = null;
        NativeWorldDispatch? nativeDispatch = null;
        lock (_stateGate)
        {
            host = _host;
            client = _client;
            context = _localWorldContext;
            if (TryTakeWorldResultLocked(result.NativeDispatchId, out var dequeued))
            {
                nativeDispatch = dequeued;
                tracked = dequeued.Transaction;
            }
        }

        if (tracked is not null)
        {
            // Authority/world context must be proven current before *any* result
            // semantic (including the secondary authority-sequence cross-check) may
            // mutate recovery or authority state. A stale callback is inert first.
            var staleWorldContext = context is null
                || !SameWorldContext(context, tracked.WorldContext);
            var staleHostAuthority = tracked.HostAuthorityContext is { } expectedHostContext
                && (host is null || !host.IsCurrentWorldAuthorityContext(expectedHostContext));
            if (staleWorldContext || staleHostAuthority)
            {
                lock (_stateGate)
                {
                    tracked.ResultCount++;
                    if (tracked.ResultCount >= tracked.ReceivedCount)
                        tracked.Completion.TrySetResult(false);
                    if (ReferenceEquals(_activeWorldTransaction, tracked))
                        _activeWorldTransaction = null;
                }
                var epoch = tracked.HostAuthorityContext?.Epoch ?? 0U;
                _diagnostics.Log("WorldSync",
                    $"WS3 stale native WorldMutation result ignored: dispatch={result.NativeDispatchId} " +
                    $"sequence={result.Sequence} generation={tracked.WorldContext.WorldGeneration} " +
                    $"epoch={epoch} code={result.Code} dispatched={result.Dispatched}.");
                return;
            }
        }

        if (nativeDispatch is not null && nativeDispatch.AuthoritySequence != result.Sequence)
        {
            _diagnostics.Log("WorldSync",
                $"WS3 NativeDispatchId correlation rejected sequence mismatch: " +
                $"dispatch={result.NativeDispatchId} expectedSequence={nativeDispatch.AuthoritySequence} " +
                $"resultSequence={result.Sequence}.");
            MarkWorldRecoveryRequired(
                $"Native dispatch {result.NativeDispatchId} returned sequence {result.Sequence}, expected {nativeDispatch.AuthoritySequence}.",
                tracked?.StartSequence ?? nativeDispatch.AuthoritySequence,
                tracked?.HostAuthorityContext);
            tracked?.Completion.TrySetResult(false);
            return;
        }

        if (tracked is null)
        {
            // Every Launcher->DLL WorldMutation now has an explicit local dispatch
            // identity. An unknown result can only be stale, already cleaned, or
            // produced by an incompatible native build; never bind it by Sequence.
            _diagnostics.Log("WorldSync",
                $"WS3 untracked native result ignored as stale/uncertified: " +
                $"dispatch={result.NativeDispatchId} sequence={result.Sequence} " +
                $"code={result.Code} dispatched={result.Dispatched} host={host is not null}.");
            return;
        }

        TaskCompletionSource<WorldMutationResult>? completion = null;
        ClientWorldTransaction? finishedTransaction = null;
        var transactionSucceeded = false;
        var transactionFailed = false;
        var retryIncompleteTransaction = false;

        lock (_stateGate)
        {
            if (_pendingWorldProbeResult is not null
                && _pendingWorldProbeSequence != 0
                && _pendingWorldProbeSequence == result.Sequence)
                completion = _pendingWorldProbeResult;

            if (tracked is not null)
            {
                tracked.ResultCount++;
                var resultFailed = result.Code != WorldMutationResultCode.Accepted || !result.Dispatched;
                if (resultFailed)
                {
                    if (!tracked.Failed)
                    {
                        tracked.FailureCode = result.Code;
                        tracked.FailureSequence = result.Sequence;
                    }
                    tracked.Failed = true;
                }

                // If the authoritative transaction was never fully received, the
                // native segment cannot have crossed gameplay: the native pump
                // requires rootCount contiguous members before preflight/execution.
                if (resultFailed && tracked.ReceivedCount < tracked.RootCount)
                {
                    retryIncompleteTransaction = result.Code == WorldMutationResultCode.QueueRejected;
                    transactionFailed = !retryIncompleteTransaction;
                    if (transactionFailed && tracked.HostAuthorityContext is null)
                        _worldRecoveryRequired = true;
                    finishedTransaction = tracked;
                    if (ReferenceEquals(_activeWorldTransaction, tracked))
                        _activeWorldTransaction = null;
                    // Keep remaining dispatch correlations. Their callbacks may
                    // still arrive and must be drained under this exact context.
                }
                else if (tracked.ResultCount == tracked.RootCount)
                {
                    finishedTransaction = tracked;
                    // All members have a final disposition. A native failure
                    // consumes this transaction as skipped, preserving its raw
                    // result for diagnostics/ACKs and never replaying effects.
                    transactionSucceeded = tracked.StartSequence == _lastCommittedWorldSequence + 1U
                        && (tracked.HostAuthorityContext is not null || !_worldRecoveryRequired);
                    if (transactionSucceeded && tracked.HostAuthorityContext is null)
                    {
                        transactionSucceeded = TryCommitCoordinatorTransactionLocked(tracked);
                        if (!transactionSucceeded)
                        {
                            _worldRecoveryRequired = true;
                            transactionFailed = true;
                        }
                    }
                    else if (!transactionSucceeded)
                    {
                        if (tracked.HostAuthorityContext is null)
                            _worldRecoveryRequired = true;
                        transactionFailed = true;
                    }
                }
            }
        }

        if (finishedTransaction is not null)
        {
            lock (_stateGate)
                RemoveWorldResultDispatchesLocked(finishedTransaction);

            if (transactionSucceeded && host is not null
                && finishedTransaction.HostAuthorityContext is { } hostTransactionContext)
            {
                if (!host.ReportWorldGameplayCommitted(
                        hostTransactionContext,
                        finishedTransaction.StartSequence,
                        finishedTransaction.EndSequence,
                        out var commitRejection))
                {
                    transactionSucceeded = false;
                    if (string.Equals(commitRejection,
                            "HOST_GAMEPLAY_COMMIT_CONTEXT_STALE", StringComparison.Ordinal))
                    {
                        transactionFailed = false;
                        _diagnostics.Log("WorldSync",
                            $"WS3 stale HostGameplayCommitted result ignored: " +
                            $"transaction={finishedTransaction.StartSequence} " +
                            $"epoch={hostTransactionContext.Epoch}.");
                    }
                    else
                    {
                        transactionFailed = true;
                        MarkWorldRecoveryRequired(
                            $"HostGameplayCommitted bookkeeping failed for transaction {finishedTransaction.StartSequence}: {commitRejection}",
                            finishedTransaction.StartSequence,
                            hostTransactionContext);
                    }
                }
                else
                {
                    lock (_stateGate)
                    {
                        if (host.IsCurrentWorldAuthorityContext(hostTransactionContext))
                            transactionSucceeded = TryCommitCoordinatorTransactionLocked(finishedTransaction);
                        else
                            transactionSucceeded = false;
                    }
                    if (!transactionSucceeded)
                    {
                        _diagnostics.Log("WorldSync",
                            $"WS3 HostGameplayCommitted callback became stale before Coordinator commit: " +
                            $"transaction={finishedTransaction.StartSequence} epoch={hostTransactionContext.Epoch}.");
                    }
                }
            }

            if (transactionFailed && !retryIncompleteTransaction)
            {
                MarkWorldRecoveryRequired(
                    $"Transaction {finishedTransaction.StartSequence} was not committed by local CATHODE.",
                    finishedTransaction.StartSequence,
                    finishedTransaction.HostAuthorityContext);
            }

            finishedTransaction.Completion.TrySetResult(transactionSucceeded);
            if (transactionSucceeded)
            {
                if (finishedTransaction.Failed)
                    _diagnostics.Log("WorldSync",
                        $"WS3 transaction {finishedTransaction.StartSequence}-{finishedTransaction.EndSequence} skipped: " +
                        $"sequence={finishedTransaction.FailureSequence} code={finishedTransaction.FailureCode}; " +
                        "this World effect may be unsynchronized. WorldSync and AISync remain active; no replay.");
                var committedCount = Interlocked.Increment(ref _worldCommittedTransactionCount);
                var now = Environment.TickCount64;
                var lastLog = Interlocked.Read(ref _worldCommitLogTick);
                if (now - lastLog >= 1_000 &&
                    Interlocked.CompareExchange(ref _worldCommitLogTick, now, lastLog) == lastLog)
                    _diagnostics.Log("WorldSync",
                        $"WS3 transactions settled={committedCount}; settledThrough={_lastCommittedWorldSequence}.");
                if (client is not null)
                    _ = TryOpenClientWorldBusAfterRecoveryAsync("recovery-suffix-committed");
            }
            else if (retryIncompleteTransaction)
            {
                _diagnostics.Log("WorldSync",
                    $"WS3 transaction {finishedTransaction.StartSequence} expired before complete assembly; requesting the authoritative suffix again from the last committed checkpoint.");
                if (client is not null && context is { Ready: true })
                    _ = RequestWorldRecoveryAsync(client, context, "incomplete-transaction");
                else
                    MarkWorldRecoveryRequired(
                        "Host-local transaction assembly became incomplete.",
                        finishedTransaction.StartSequence,
                        finishedTransaction.HostAuthorityContext);
            }
            else if (transactionFailed)
                _diagnostics.Log("WorldSync",
                    $"WS3 transaction {finishedTransaction.StartSequence} was not committed; rejoin/resynchronization is required and it will not be auto-replayed.");
        }

        if (result.Code is WorldMutationResultCode.NativeFault or WorldMutationResultCode.QueueRejected
            or WorldMutationResultCode.ManifestRejected or WorldMutationResultCode.GenerationRejected
            or WorldMutationResultCode.EntityRejected)
        {
            var now = DateTimeOffset.UtcNow;
            int suppressed;
            bool shouldLog;
            lock (_stateGate)
            {
                _suppressedWorldMutationFaults++;
                shouldLog = now >= _nextWorldMutationFaultSummaryAtUtc;
                suppressed = shouldLog ? _suppressedWorldMutationFaults : 0;
                if (shouldLog)
                {
                    _suppressedWorldMutationFaults = 0;
                    _nextWorldMutationFaultSummaryAtUtc = now.AddSeconds(5);
                }
            }
            if (shouldLog)
            {
                _diagnostics.Log("WorldSync",
                    $"WorldMutation faults summarized: count={suppressed} lastCode={result.Code} " +
                    $"resolved={result.EntityResolved} dispatched={result.Dispatched}. " +
                    "Failed transactions are logged and skipped; WorldSync continues without replaying partial effects.");
            }
        }
        completion?.TrySetResult(result);
        if (client is not null && context is not null && context.Ready
            && (tracked is null || SameWorldContext(context, tracked.WorldContext)))
        {
            var ack = new WorldNetworkMutationAck(
                client.SessionId,
                client.PeerId,
                context.WorldGeneration,
                result);
            _ = SendWorldAckBestEffortAsync(client, ack);
        }
    }

    private async Task SendWorldAckBestEffortAsync(
        LobbyClient client,
        WorldNetworkMutationAck ack)
    {
        try
        {
            await client.SendWorldMutationAckAsync(ack, _lifetime.Token).ConfigureAwait(false);
        }
        catch (Exception exception) when (exception is IOException or ProtocolException
            or InvalidOperationException or OperationCanceledException)
        {
            if (!_lifetime.IsCancellationRequested)
                _diagnostics.Log("WorldSync", $"WorldMutation ACK failed: {exception.Message}");
        }
    }

    private void OnWorldMutationAckReceived(object? sender, WorldNetworkMutationAck ack)
    {
        TaskCompletionSource<IReadOnlyList<WorldNetworkMutationAck>>? completion = null;
        IReadOnlyList<WorldNetworkMutationAck>? completedAcks = null;
        lock (_stateGate)
        {
            if (_pendingWorldProbeAckResult is not null
                && _pendingWorldProbeExpectedPeers?.Contains(ack.PeerId) == true
                && _pendingWorldProbeSequence != 0
                && ack.Result.Sequence == _pendingWorldProbeSequence)
            {
                _pendingWorldProbeAcks[ack.PeerId] = ack;
                if (_pendingWorldProbeAcks.Count == _pendingWorldProbeExpectedPeers.Count)
                {
                    completion = _pendingWorldProbeAckResult;
                    completedAcks = _pendingWorldProbeAcks.Values
                        .OrderBy(value => value.PeerId)
                        .ToArray();
                }
            }
        }
        if (completion is not null && completedAcks is not null)
            completion.TrySetResult(completedAcks);
    }

    private void OnWorldContextReceived(object? sender, WorldNetworkContext context)
    {
        if (sender is LobbyClient)
            _ = PrepareClientWorldContextAsync(context);
        else
            _diagnostics.Log("WorldSync",
                $"WorldContext observed: peer={context.PeerId} ready={context.Ready} map={context.Manifest.MapContentId}.");
    }

    private async Task PrepareClientWorldContextAsync(WorldNetworkContext hostContext)
    {
        try
        {
            lock (_stateGate)
            {
                if (!_worldSyncEnabledForSession)
                {
                    _diagnostics.Log("WorldSync",
                        "Host WorldContext ignored because WorldSync is disabled locally for this game session.");
                    return;
                }
            }
            LobbyClient client;
            LobbyStartPayload loadingStart;
            lock (_stateGate)
            {
                client = _client ?? throw new InvalidOperationException("Client session is unavailable.");
                loadingStart = _start ?? throw new InvalidOperationException("Client session has not started.");
            }
            if (!hostContext.Ready)
            {
                await CloseNativeWorldBusAsync("host-world-context-invalidated")
                    .ConfigureAwait(false);
                lock (_stateGate) ResetWorldReplicationState(resetWorldGeneration: false);
                _diagnostics.Log("WorldSync", "Host WorldContext was invalidated.");
                return;
            }
            if (hostContext.SessionId != client.SessionId)
                throw new ProtocolException("Host WorldContext session is stale.");
            var replacingWorldContext = false;
            lock (_stateGate)
            {
                if (_worldSessionGeneration != 0
                    && hostContext.WorldGeneration < _worldSessionGeneration)
                    throw new ProtocolException("Host WorldContext generation is stale.");
                replacingWorldContext = _localWorldContext is { Ready: true } existing
                    && !SameWorldContext(existing, hostContext);
            }
            if (replacingWorldContext)
                await CloseNativeWorldBusAsync("host-world-context-changing")
                    .ConfigureAwait(false);
            Task<PreparedWorldManifest?>? automaticPreparation;
            lock (_stateGate) automaticPreparation = _automaticWorldManifestPreparation;
            var automaticPrepared = automaticPreparation is null
                ? null
                : await automaticPreparation.ConfigureAwait(false);
            var automaticMatchesHost = automaticPrepared is not null
                && automaticPrepared.Identity == hostContext.Manifest
                && string.Equals(automaticPrepared.Sha256, hostContext.ManifestSha256,
                    StringComparison.OrdinalIgnoreCase);
            if (automaticPrepared is not null && !automaticMatchesHost)
            {
                _diagnostics.Log("WorldSync",
                    $"Automatic manifest mismatch: localMap={automaticPrepared.Identity.MapContentId} " +
                    $"hostMap={hostContext.Manifest.MapContentId} " +
                    $"localCompatSha256={automaticPrepared.Sha256} " +
                    $"hostCompatSha256={hostContext.ManifestSha256} " +
                    $"localBinarySha256={automaticPrepared.BinarySha256} " +
                    $"localPakSha256={automaticPrepared.Manifest.Source.CommandsPakSha256} " +
                    $"localBinSha256={automaticPrepared.Manifest.Source.CommandsBinSha256}.");
            }
            // Manifest hash match with the host is no longer required: use the locally prepared
            // manifest when available, and only fall back to the host-identified cache entry.
            var prepared = automaticPrepared
                ?? WorldManifestSessionLoader.LoadFromCache(hostContext);
            var nativeGeneration = _controller.Snapshot.LevelGeneration;
            await EnsureNativeManifestLoadedAsync(prepared, loadingStart, nativeGeneration).ConfigureAwait(false);
            EnsureManifestLoadContext(loadingStart, nativeGeneration);
            var local = hostContext with { PeerId = client.PeerId, Ready = true };
            lock (_stateGate)
            {
                EnsureManifestLoadContext(loadingStart, nativeGeneration);
                _localWorldContext = local;
                _worldSessionGeneration = local.WorldGeneration;
                _localWorldRuntimeGeneration = _controller.Snapshot.LevelGeneration;
                if (_worldResumeCheckpoint is { } resume
                    && resume.SessionId == local.SessionId
                    && resume.WorldGeneration == local.WorldGeneration
                    && resume.Manifest == local.Manifest)
                {
                    _lastCommittedWorldSequence = resume.LastCommittedSequence;
                    _worldRecoveryRequired = resume.RecoveryRequired;
                    foreach (var cause in resume.PendingAwaitAuthorityCauses)
                        _pendingAwaitAuthorityCauses.Add(cause);
                }
                else
                {
                    _worldResumeCheckpoint = null;
                    _lastCommittedWorldSequence = 0;
                    _worldRecoveryRequired = false;
                    _pendingAwaitAuthorityCauses.Clear();
                }
            }
            await client.SendWorldContextAsync(local, _lifetime.Token).ConfigureAwait(false);
            _diagnostics.Log("WorldSync",
                $"WorldContext compatible: map={local.Manifest.MapContentId} compatSha256={local.ManifestSha256}.");
            await RequestWorldRecoveryAsync(client, local, "context-ready/reconnect")
                .ConfigureAwait(false);
        }
        catch (Exception exception) when (exception is IOException or ProtocolException
            or InvalidOperationException or OperationCanceledException)
        {
            if (!_lifetime.IsCancellationRequested && exception is not OperationCanceledException)
            {
                _diagnostics.Log("WorldSync", $"WorldContext compatibility failed: {exception.Message}");
                await ShowPreparationFailureAsync(exception.Message).ConfigureAwait(false);
            }
        }
    }

    private void InvalidateWorldContextForRuntimeGeneration(int generation)
    {
        WorldNetworkContext? old = null;
        LobbyHost? host = null;
        LobbyClient? client = null;
        lock (_stateGate)
        {
            if (_localWorldContext is null || _localWorldRuntimeGeneration == generation) return;
            old = _localWorldContext;
            host = _host;
            client = _client;
            ResetWorldReplicationState(resetWorldGeneration: false);
        }
        _diagnostics.Log("WorldSync",
            $"Local WorldContext invalidated by LevelGeneration change to {generation}.");
        _ = CloseNativeWorldBusAsync("runtime-generation-changed");
        if (old is null) return;
        var invalid = old with { PeerId = host?.HostPeerId ?? client?.PeerId ?? old.PeerId, Ready = false };
        if (host is not null) _ = SetHostWorldContextBestEffortAsync(host, invalid);
        else if (client is not null) _ = SendClientWorldContextBestEffortAsync(client, invalid);
    }

    private async Task SetHostWorldContextBestEffortAsync(LobbyHost host, WorldNetworkContext context)
    {
        try { await host.SetHostWorldContextAsync(context, _lifetime.Token).ConfigureAwait(false); }
        catch (Exception exception) when (exception is ProtocolException or IOException or OperationCanceledException)
        { if (!_lifetime.IsCancellationRequested) _diagnostics.Log("WorldSync", $"WorldContext invalidation send failed: {exception.Message}"); }
    }

    private async Task SendClientWorldContextBestEffortAsync(LobbyClient client, WorldNetworkContext context)
    {
        try { await client.SendWorldContextAsync(context, _lifetime.Token).ConfigureAwait(false); }
        catch (Exception exception) when (exception is ProtocolException or IOException or OperationCanceledException)
        { if (!_lifetime.IsCancellationRequested) _diagnostics.Log("WorldSync", $"WorldContext invalidation send failed: {exception.Message}"); }
    }

    private void ResetWorldReplicationState(
        bool resetWorldGeneration = true,
        bool preserveCommittedCheckpoint = false)
    {
        if (preserveCommittedCheckpoint && _localWorldContext is { Ready: true } current)
        {
            _worldResumeCheckpoint = new WorldResumeCheckpoint(
                current.SessionId,
                current.WorldGeneration,
                current.Manifest,
                _lastCommittedWorldSequence,
                _worldRecoveryRequired,
                _pendingAwaitAuthorityCauses.ToArray());
        }
        else if (!preserveCommittedCheckpoint)
        {
            _worldResumeCheckpoint = null;
        }
        _activeWorldTransaction?.Completion.TrySetCanceled();
        _activeWorldTransaction = null;
        _worldDispatchWindow.Clear();
        // Exact NativeDispatchId correlation makes old sequence tombstones
        // unnecessary. Late callbacks from cleared dispatches are unknown/stale
        // and cannot be rebound to any future transaction.
        _worldResultDispatches.Clear();
        _currentHostAuthorityContext = null;
        _worldRecoveryAuthorityContext = null;
        _pendingAwaitAuthorityCauses.Clear();
        _lastCommittedWorldSequence = 0;
        _worldRecoveryTargetSequence = 0;
        _worldRecoveryAwaitSnapshot.Clear();
        _worldRecoveryRequired = false;
        _worldRecoveryRequestRunning = false;
        _localWorldContext = null;
        if (resetWorldGeneration) _worldSessionGeneration = 0;
        _localWorldRuntimeGeneration = 0;
        _automaticWorldManifestPreparation = null;
        _remoteEquipmentCatalogPreparation = null;
        _worldActivationRunning = false;
        _worldActivationAttempted = false;
        _nextWorldActivationRetryUtc = DateTimeOffset.MinValue;
        ClearPendingWorldProbeState();
    }

    private void ClearPendingWorldProbeState()
    {
        lock (_stateGate)
        {
            _pendingWorldProbeSequence = 0;
            _pendingWorldProbeResult?.TrySetCanceled();
            _pendingWorldProbeAckResult?.TrySetCanceled();
            _pendingWorldProbeResult = null;
            _pendingWorldProbeAckResult = null;
            _pendingWorldProbeExpectedPeers = null;
            _pendingWorldProbeAcks.Clear();
        }
    }

    private PlayerSpawnPositionPayload GetLocalSpawnPosition(ulong peerId)
    {
        lock (_stateGate)
        {
            if (!_positionAvailable || !_localGameplayReady)
                throw new InvalidOperationException("The host player is not ready in gameplay.");
            return RemoteSpawnPlanner.CreateNetworkPosition(peerId, _position);
        }
    }

    private (LobbyHost Host, int Generation) GetSpawnCommandTarget()
    {
        lock (_stateGate)
        {
            if (_host is null) throw new InvalidOperationException(
                "Only the lobby host can execute the connected-player spawn command.");
            if (_start is null) throw new InvalidOperationException(
                "Start the multiplayer session before spawning connected players.");
            if ((_lobby?.Players.Count ?? 0) < 2) throw new InvalidOperationException(
                "At least one remote player must be connected.");
            if (!_localGameplayReady || !_positionAvailable) throw new InvalidOperationException(
                "Wait until the host player is fully loaded before spawning remote players.");
            if (_spawnCommandCollecting || _pendingSpawnRequest is not null || _spawnRequestRunning)
                throw new InvalidOperationException("A remote-player spawn command is already active.");
            _spawnCommandCollecting = true;
            return (_host, _start.LevelGeneration);
        }
    }

    private static GameCommandResultPayload CreateSpawnBroadcastResult(
        SpawnRemotePlayersPayload payload) => new(
        ProtocolConstants.ExperimentalSchemaVersion,
        payload.RequestId,
        payload.LevelGeneration,
        Guid.Empty,
        LauncherGameCommandNames.SpawnConnectedPlayers,
        "REMOTE_PLAYER_SPAWN_BROADCAST",
        "The spawn command was broadcast to every connected launcher.",
        true);

    private Task LogCharacterSelectionAsync(ScaleformBridgeMessage message)
    {
        var character = message.Get("characterName", message.Get("characterId", "unknown"));
        _diagnostics.Log("Multiplayer", $"Local character selected: {character}.");
        return Task.CompletedTask;
    }

    private async Task SendEmptyServerListAsync()
    {
        await _bridge.SendAsync("SERVER_LIST", new Dictionary<string, string>
        { ["count"] = "0" }).ConfigureAwait(false);
        await SendStatusAsync("Server discovery is disabled; use Direct Connection.")
            .ConfigureAwait(false);
    }

    private void AttachHost(LobbyHost host)
    {
        _aiSync.BindHost(host);
        host.VoiceFrameReceived += _voiceSync.Receive;
        host.SnapshotChanged += OnLobbySnapshot;
        host.Started += OnLobbyStarted;
        host.SessionReleased += OnSessionReleased;
        host.GameplayReadyReceived += OnGameplayReady;
        host.RoundEnded += OnRoundEnded;
        host.RoundProgressChanged += OnRoundProgress;
        host.MissionSignalCommitted += OnMissionSignalCommitted;
        host.MissionPreparationFailed += OnNetworkError;
        host.ItemCollected += OnItemCollected;
        host.ItemSpawnDecided += OnItemSpawnDecided;
        host.ThrowableProposed += OnRemoteThrowableProposed;
        host.SpawnRemotePlayersRequested += OnSpawnRemotePlayersRequested;
        host.PlayerPositionReceived += OnPlayerPositionReceived;
        host.PlayerEquipmentReceived += OnPlayerEquipmentReceived;
        host.PlayerActionReceived += OnPlayerActionReceived;
        host.WorldContextReceived += OnWorldContextReceived;
        host.WorldMutationAccepted += OnWorldMutationAccepted;
        host.WorldIntentRejected += OnWorldIntentRejected;
        host.WorldMutationAckReceived += OnWorldMutationAckReceived;
        host.Log += OnNetworkLog;
    }

    private void DetachHost(LobbyHost host)
    {
        _aiSync.UnbindHost(host);
        host.VoiceFrameReceived -= _voiceSync.Receive;
        SuspendVoiceSession();
        host.SnapshotChanged -= OnLobbySnapshot;
        host.Started -= OnLobbyStarted;
        host.SessionReleased -= OnSessionReleased;
        host.GameplayReadyReceived -= OnGameplayReady;
        host.RoundEnded -= OnRoundEnded;
        host.RoundProgressChanged -= OnRoundProgress;
        host.MissionSignalCommitted -= OnMissionSignalCommitted;
        host.MissionPreparationFailed -= OnNetworkError;
        host.ItemCollected -= OnItemCollected;
        host.ItemSpawnDecided -= OnItemSpawnDecided;
        host.ThrowableProposed -= OnRemoteThrowableProposed;
        host.SpawnRemotePlayersRequested -= OnSpawnRemotePlayersRequested;
        host.PlayerPositionReceived -= OnPlayerPositionReceived;
        host.PlayerEquipmentReceived -= OnPlayerEquipmentReceived;
        host.PlayerActionReceived -= OnPlayerActionReceived;
        host.WorldContextReceived -= OnWorldContextReceived;
        host.WorldMutationAccepted -= OnWorldMutationAccepted;
        host.WorldIntentRejected -= OnWorldIntentRejected;
        host.WorldMutationAckReceived -= OnWorldMutationAckReceived;
        host.Log -= OnNetworkLog;
    }

    private void AttachClient(LobbyClient client)
    {
        _aiSync.BindClient(client);
        client.VoiceFrameReceived += _voiceSync.Receive;
        client.SnapshotChanged += OnLobbySnapshot;
        client.Started += OnLobbyStarted;
        client.SessionReleased += OnSessionReleased;
        client.GameplayReadyReceived += OnGameplayReady;
        client.RoundEnded += OnRoundEnded;
        client.RoundProgressReceived += OnRoundProgress;
        client.MissionSignalCommitted += OnMissionSignalCommitted;
        client.ItemCollected += OnItemCollected;
        client.ItemSpawnDecided += OnItemSpawnDecided;
        client.ThrowableCommitted += OnRemoteThrowableCommitted;
        client.SpawnPositionRequested += OnSpawnPositionRequested;
        client.SpawnRemotePlayersRequested += OnSpawnRemotePlayersRequested;
        client.PlayerPositionReceived += OnPlayerPositionReceived;
        client.PlayerEquipmentReceived += OnPlayerEquipmentReceived;
        client.PlayerActionReceived += OnPlayerActionReceived;
        client.WorldContextReceived += OnWorldContextReceived;
        client.WorldMutationReceived += OnWorldMutationReceived;
        client.WorldIntentRejected += OnWorldIntentRejected;
        client.WorldRecoveryStatusReceived += OnWorldRecoveryStatusReceived;
        client.ErrorReceived += OnNetworkError;
        client.Disconnected += OnClientDisconnected;
        client.Log += OnNetworkLog;
    }

    private void DetachClient(LobbyClient client)
    {
        _aiSync.UnbindClient(client);
        client.VoiceFrameReceived -= _voiceSync.Receive;
        SuspendVoiceSession();
        client.SnapshotChanged -= OnLobbySnapshot;
        client.Started -= OnLobbyStarted;
        client.SessionReleased -= OnSessionReleased;
        client.GameplayReadyReceived -= OnGameplayReady;
        client.RoundEnded -= OnRoundEnded;
        client.RoundProgressReceived -= OnRoundProgress;
        client.MissionSignalCommitted -= OnMissionSignalCommitted;
        client.ItemCollected -= OnItemCollected;
        client.ItemSpawnDecided -= OnItemSpawnDecided;
        client.ThrowableCommitted -= OnRemoteThrowableCommitted;
        client.SpawnPositionRequested -= OnSpawnPositionRequested;
        client.SpawnRemotePlayersRequested -= OnSpawnRemotePlayersRequested;
        client.PlayerPositionReceived -= OnPlayerPositionReceived;
        client.PlayerEquipmentReceived -= OnPlayerEquipmentReceived;
        client.PlayerActionReceived -= OnPlayerActionReceived;
        client.WorldContextReceived -= OnWorldContextReceived;
        client.WorldMutationReceived -= OnWorldMutationReceived;
        client.WorldIntentRejected -= OnWorldIntentRejected;
        client.WorldRecoveryStatusReceived -= OnWorldRecoveryStatusReceived;
        client.ErrorReceived -= OnNetworkError;
        client.Disconnected -= OnClientDisconnected;
        client.Log -= OnNetworkLog;
    }

    private void OnLobbySnapshot(object? sender, LobbySnapshotPayload value) =>
        _ = PublishLobbyAsync(sender, value);

    private async Task PublishLobbyAsync(object? sender, LobbySnapshotPayload? value)
    {
        if (value is null) return;
        string[] departedNames;
        uint[] departedCharacters;
        lock (_stateGate)
        {
            if (!IsCurrentLobbySource(sender, value.SessionId)) return;
            departedNames = CaptureDepartedPlayerNames(value);
            departedCharacters = UpdateLobbySnapshot(value);
        }
        if (departedCharacters.Length != 0)
            await StopRemoteCharactersBestEffortAsync(departedCharacters).ConfigureAwait(false);
        lock (_stateGate)
            if (!IsCurrentLobbySource(sender, value.SessionId)) return;
        await ShowPlayersLeftAsync(departedNames, value.SessionId).ConfigureAwait(false);
        PublishState();
        LobbySnapshotPayload? latest;
        lock (_stateGate)
        {
            if (!IsCurrentLobbySource(sender, value.SessionId)) return;
            latest = _lobby;
        }
        if (latest is not null)
        {
            ScheduleLobbyPreparation(latest);
            await SendLobbyStateAsync(latest).ConfigureAwait(false);
        }
    }

    private uint[] UpdateLobbySnapshot(LobbySnapshotPayload value)
    {
        lock (_stateGate)
        {
            _lobby = value;
            _assignedCharacters.Clear();
            foreach (var player in value.Players)
                if (PlayerCharacterCatalog.IsKnown(player.Character))
                    _assignedCharacters[player.PeerId] = player.Character!;
            var departed = RemoveDepartedRemotePlayers(value);
            var activePeers = value.Players.Select(player => player.PeerId).ToHashSet();
            _remoteGameplayReadyPeers.RemoveWhere(peerId => !activePeers.Contains(peerId));
            _remoteGameplayReady = AreAllRemoteGameplayPlayersReady();
            return departed;
        }
    }

    private uint[] RemoveDepartedRemotePlayers(LobbySnapshotPayload value)
    {
        var activePeers = value.Players.Select(player => player.PeerId).ToHashSet();
        var departedPeers = _remoteCharacters.Keys.Where(peerId => !activePeers.Contains(peerId)).ToArray();
        var characters = departedPeers.Select(peerId => _remoteCharacters[peerId]).Distinct().ToArray();
        foreach (var peerId in departedPeers) RemoveRemotePeer(peerId);
        return characters;
    }

    private void RemoveRemotePeer(ulong peerId)
    {
        _remoteCharacters.Remove(peerId);
        _aiSync.SetParticipant(peerId, 0);
        _remotePositions.Remove(peerId);
        _remoteEquipment.Remove(peerId);
        _remoteLastPlayerSyncSequence.Remove(peerId);
        _remoteProvisionedEquipment.Remove(peerId);
        _remoteAppliedAim.Remove(peerId);
        _remoteAppliedPrimary.Remove(peerId);
        _remoteSpawnRetryAttempts.Remove(peerId);
        _remoteSpawnRetryNotBeforeUtc.Remove(peerId);
    }

    private void OnLobbyStarted(object? sender, LobbyStartPayload value) =>
        _ = HandleLobbyStartedAsync(sender, value);

    private async Task HandleLobbyStartedAsync(object? sender, LobbyStartPayload value)
    {
        try
        {
            await _commandGate.WaitAsync(_lifetime.Token).ConfigureAwait(false);
            try
            {
                lock (_stateGate)
                    if (!IsCurrentLobbySource(sender, value.SessionId)) return;
                await ApplyLobbyStartedAsync(value).ConfigureAwait(false);
            }
            finally { _commandGate.Release(); }
        }
        catch (OperationCanceledException) { }
        catch (Exception exception)
        { _diagnostics.Log("Multiplayer", $"Lobby start failed: {exception.Message}"); }
    }

    private async Task ApplyLobbyStartedAsync(LobbyStartPayload value)
    {
        var worldSyncEnabled = _performanceIsolation.IsEnabled(PerformanceIsolationModule.WorldSync);
        uint[] previousCharacters;
        LobbyHost? host;
        LobbyClient? client;
        lock (_stateGate)
        {
            previousCharacters = _remoteCharacters.Values.Distinct().ToArray();
            ResetGameplayState(value);
            _worldSyncEnabledForSession = worldSyncEnabled;
            host = _host;
            client = _client;
        }
        await BeginNativeRoundAsync(value).ConfigureAwait(false);
        if (host is not null)
            await SetNativeWorldSyncLifecycleAsync(
                host.SessionId, host.HostPeerId, true, value.LevelGeneration,
                true, false, "lobby-start")
                .ConfigureAwait(false);
        else if (client is not null)
            await SetNativeWorldSyncLifecycleAsync(
                client.SessionId, client.PeerId, false, value.LevelGeneration,
                true, false, "lobby-start")
                .ConfigureAwait(false);
        await SetSessionLoadingHoldAsync(value, true, _lifetime.Token).ConfigureAwait(false);
        lock (_stateGate)
        {
            if (_start?.SessionId != value.SessionId || _start.LevelGeneration != value.LevelGeneration)
                throw new OperationCanceledException("Lobby changed while arming preparation hold.");
            _loadingHoldStart = value;
            _worldQuiescenceWaitSinceUtc = null;
            _loadingHoldStartedUtc = DateTimeOffset.UtcNow;
            _nextPreparationProgressUtc = DateTimeOffset.MinValue;
            _loadingOverlayAcknowledged = false;
            _loadingOverlayConnection = Guid.Empty;
            _sessionPreparedSending = _sessionPreparedSent = _sessionReleaseAuthorized = _sessionReleaseRunning = false;
        }
        BeginRemoteEquipmentCatalogPreparation(value);
        if (worldSyncEnabled)
        {
            BeginAutomaticWorldManifestPreparation(value);
        }
        else
        {
            _diagnostics.Log("WorldSync",
                "WorldSync is disabled in Settings for this game session. Character spawn and movement replication remain active; no world manifest will be compiled or loaded.");
        }
        await StopRemoteCharactersBestEffortAsync(previousCharacters).ConfigureAwait(false);
        PublishState();
        await PublishLobbyStartAsync(value).ConfigureAwait(false);
    }

    private async Task PublishLobbyStartAsync(LobbyStartPayload value)
    {
        _diagnostics.Log("Multiplayer", $"Session starting: {value.Level}.");
        await SendStartGameAsync(value).ConfigureAwait(false);
    }

    private void ResetGameplayState(LobbyStartPayload start)
    {
        ResetMissionSync();
        ClearRoundLifecycleState();
        _start = start;
        _uiGameplayLoaded = false;
        _localGameplayReady = false;
        _localReadySending = false;
        _remoteGameplayReady = false;
        _worldSyncEnabledForSession = true;
        ResetWorldReplicationState();
        ResetRemoteSpawnState();
        _gameplayReadiness.Reset();
    }

    private void ResetRemoteSpawnState()
    {
        ++_remoteSpawnEpoch;
        _pendingSpawnPositionRequest = null;
        _spawnPositionSending = false;
        _spawnCommandCollecting = false;
        _pendingSpawnRequest = null;
        _spawnRequestRunning = false;
        _remoteSpawnConvergenceRunning = false;
        _remoteSpawnRetryAttempts.Clear();
        _remoteSpawnRetryNotBeforeUtc.Clear();
        _inFlightRemoteSpawns.Clear();
        _remoteGameplayReadyPeers.Clear();
        _remoteCharacters.Clear();
        _aiSync.ClearParticipants();
        _remotePositions.Clear();
        _processedSpawnRequests.Clear();
        _pendingLocalPosition = null;
        _pendingLocalPostureEdges.Clear();
        _lastQueuedPosture = null;
        _lastQueuedTorch = null;
        _lastQueuedHiding = null;
        _positionSendRunning = false;
        _localPositionSequence = 0;
        // Native PlayerSync state belongs to the AI.exe runtime, not to a lobby
        // session. Preserve the latest observed state across network restarts so
        // a new session can immediately publish a fresh snapshot.
        _lastQueuedEquipmentState = null;
        _nextEquipmentRefreshAtUtc = DateTimeOffset.MinValue;
        _pendingLocalEquipment = null;
        _playerSyncSendRunning = false;
        _localPlayerSyncSequence = 0;
        _pendingLocalPlayerActions.Clear();
        _remoteEquipment.Clear();
        _pendingRemotePlayerActions.Clear();
        _remoteLastPlayerSyncSequence.Clear();
        _remotePlayerActionApplyRunning = false;
        _remoteProvisionedEquipment.Clear();
        _remoteAppliedAim.Clear();
        _remoteAppliedPrimary.Clear();
        _movementCommandRunning = false;
    }

    private void OnGameplayReady(object? sender, GameplayReadyPayload value) =>
        _ = HandleGameplayReadyAsync(value);

    private async Task HandleGameplayReadyAsync(GameplayReadyPayload value)
    {
        if (!IsRemoteGameplayReady(value)) return;
        lock (_stateGate)
        {
            _remoteGameplayReadyPeers.Add(value.PeerId);
            _remoteGameplayReady = AreAllRemoteGameplayPlayersReady();
        }
        _diagnostics.Log("Multiplayer", $"Remote player is in gameplay (PeerId {value.PeerId}).");
        PublishState();
        var runtime = _controller.Snapshot;
        await TryAdvanceGameplayAsync(runtime).ConfigureAwait(false);
        await TryConvergeRemoteCharactersAsync(runtime, DateTimeOffset.UtcNow)
            .ConfigureAwait(false);
    }

    private bool IsRemoteGameplayReady(GameplayReadyPayload value)
    {
        lock (_stateGate)
        {
            var localPeer = _host?.HostPeerId ?? _client?.PeerId ?? 0;
            return _start is not null && value.SessionId == _start.SessionId
                && value.LevelGeneration == _start.LevelGeneration && value.PeerId != localPeer;
        }
    }

    private bool AreAllRemoteGameplayPlayersReady()
        => RemoteSpawnPlanner.AllRemotePlayersReady(
            _lobby,
            _host?.HostPeerId ?? _client?.PeerId ?? 0,
            _remoteGameplayReadyPeers);

    private void OnPlayerPositionReceived(
        object? sender,
        PlayerPositionSnapshotPayload value)
    {
        var now = DateTimeOffset.UtcNow;
        if (!AcceptRemotePosition(value, now)) return;
        var runtime = _controller.Snapshot;
        _ = TryConvergeRemoteCharactersAsync(runtime, now);
        _ = TryAdvanceRemoteMovementAsync(runtime, now);
    }

    private bool AcceptRemotePosition(
        PlayerPositionSnapshotPayload value,
        DateTimeOffset receivedAtUtc)
    {
        var recoveredGameplayReady = false;
        lock (_stateGate)
        {
            if (_start is null || _lobby is null || value.PeerId == LocalPeerId()) return false;
            if (value.SessionId != _start.SessionId
                || value.LevelGeneration != _start.LevelGeneration) return false;
            if (_lobby.Players.All(player => player.PeerId != value.PeerId)) return false;
            if (!_remotePositions.TryGetValue(value.PeerId, out var track))
                _remotePositions[value.PeerId] = track = new RemotePositionTrack();
            if (!track.TryAccept(value, receivedAtUtc)) return false;
            _aiSync.ObserveParticipantIncarnation(value.PeerId,value.CharacterIncarnation);

            // A current-generation position can only be published after the
            // sender completed its local gameplay-ready transition. Treat it
            // as durable evidence so a missed one-shot GameplayReady message
            // cannot block this peer's Character forever.
            recoveredGameplayReady = _remoteGameplayReadyPeers.Add(value.PeerId);
            if (recoveredGameplayReady)
                _remoteGameplayReady = AreAllRemoteGameplayPlayersReady();
        }

        if (recoveredGameplayReady)
        {
            _diagnostics.Log("Multiplayer",
                $"Remote gameplay readiness recovered from live position evidence " +
                $"(PeerId {value.PeerId}).");
            PublishState();
        }
        return true;
    }

    private void OnNativePlayerSyncStateProduced(
        object? sender,
        NativePlayerEquipmentStatePayload value)
    {
        if (Volatile.Read(ref _disposed) != 0 ||
            !_performanceIsolation.IsEnabled(PerformanceIsolationModule.EquipmentSync)) return;
        lock (_stateGate)
        {
            _localEquipmentState = value.Valid ? value : null;
            if (!value.Valid)
            {
                _lastQueuedEquipmentState = null;
                _nextEquipmentRefreshAtUtc = DateTimeOffset.MinValue;
            }
        }

        if (value.Valid && TryQueueLocalEquipment(DateTimeOffset.UtcNow))
            _ = PumpLocalPlayerSyncAsync();
    }

    private void OnNativePlayerSyncActionProduced(
        object? sender,
        NativePlayerActionEventPayload value)
    {
        if (Volatile.Read(ref _disposed) != 0 ||
            !_performanceIsolation.IsEnabled(PerformanceIsolationModule.ActionSync)) return;

        var startPump = false;
        lock (_stateGate)
        {
            if (_start is null || !_localGameplayReady) return;
            var peerId = LocalPeerId();
            if (peerId == 0 || (_host is null && _client is null)) return;

            _pendingLocalPlayerActions.Enqueue(new PlayerActionEventPayload(
                _start.SessionId,
                peerId,
                _start.LevelGeneration,
                ++_localPlayerSyncSequence,
                DateTimeOffset.UtcNow.ToUnixTimeMilliseconds(),
                value.Action,
                value.Active,
                value.CurrentEquipmentIndex,
                value.PresentationState));
            if (!_playerSyncSendRunning)
            {
                _playerSyncSendRunning = true;
                startPump = true;
            }
        }

        if (startPump) _ = PumpLocalPlayerSyncAsync();
    }

    private void OnPlayerEquipmentReceived(
        object? sender,
        PlayerEquipmentSnapshotPayload value)
    {
        if (!_performanceIsolation.IsEnabled(PerformanceIsolationModule.EquipmentSync)) return;
        PlayerEquipmentSnapshotPayload? previous;
        uint characterBase;
        lock (_stateGate)
        {
            if (_start is null || _lobby is null || value.PeerId == LocalPeerId()) return;
            if (value.SessionId != _start.SessionId
                || value.LevelGeneration != _start.LevelGeneration) return;
            if (_lobby.Players.All(player => player.PeerId != value.PeerId)) return;
            if (_remoteLastPlayerSyncSequence.TryGetValue(value.PeerId, out var lastSequence)
                && value.Sequence <= lastSequence) return;
            _remoteLastPlayerSyncSequence[value.PeerId] = value.Sequence;
            _remoteEquipment.TryGetValue(value.PeerId, out previous);
            _remoteEquipment[value.PeerId] = value;
            _remoteCharacters.TryGetValue(value.PeerId, out characterBase);
        }

        // Only a change in the equipment that can actually be represented on
        // the remote Character requires convergence. Retail can oscillate raw
        // current/previous/active fields while resolving to the same carried item;
        // treating those internal transitions as equipment changes causes needless
        // network/convergence work without changing the remote result.
        var equipmentChanged = previous is null
            || ResolveVisibleRemoteEquipmentIndex(previous)
                != ResolveVisibleRemoteEquipmentIndex(value);
        var actionStateChanged = previous is null
            || previous.AimHeld != value.AimHeld
            || previous.PrimaryHeld != value.PrimaryHeld
            || (IsSurvivalEquipmentSession() && RemoteEquipmentSelection.ActionEligibilityChanged(
                previous.CurrentEquipmentIndex, previous.EquipmentActive,
                value.CurrentEquipmentIndex, value.EquipmentActive));
        if (!equipmentChanged && !actionStateChanged) return;

        if (equipmentChanged)
        {
            var characterText = characterBase == 0
                ? "CharacterBase=pending"
                : $"CharacterBase=0x{characterBase:X8}";
            _diagnostics.Log("PlayerSync",
                $"Remote PeerId {value.PeerId}: equipment={value.CurrentEquipmentIndex}, previous={value.PreviousEquipmentIndex}, " +
                $"active={(value.EquipmentActive ? 1 : 0)}, aim={(value.AimHeld ? 1 : 0)}, primary={(value.PrimaryHeld ? 1 : 0)}, {characterText}.");
        }

        // Durable Aim/Primary state is also a loss-recovery path for the bounded
        // edge queue. This only runs on an actual state transition, never per frame.
        if (characterBase != 0U)
            _ = ConvergeRemotePlayerEquipmentAsync(value.PeerId);
    }

    private void OnPlayerActionReceived(
        object? sender,
        PlayerActionEventPayload value)
    {
        if (Volatile.Read(ref _disposed) != 0 ||
            !_performanceIsolation.IsEnabled(PerformanceIsolationModule.ActionSync)) return;
        var startPump = false;
        lock (_stateGate)
        {
            if (_start is null || _lobby is null || value.PeerId == LocalPeerId()) return;
            if (value.SessionId != _start.SessionId
                || value.LevelGeneration != _start.LevelGeneration) return;
            if (_lobby.Players.All(player => player.PeerId != value.PeerId)) return;
            if (_remoteLastPlayerSyncSequence.TryGetValue(value.PeerId, out var lastSequence)
                && value.Sequence <= lastSequence) return;
            _remoteLastPlayerSyncSequence[value.PeerId] = value.Sequence;

            _pendingRemotePlayerActions.Enqueue(value);
            if (!_remotePlayerActionApplyRunning)
            {
                _remotePlayerActionApplyRunning = true;
                startPump = true;
            }
        }

        if (startPump) _ = PumpRemotePlayerActionsAsync();
    }

    private void OnSpawnPositionRequested(
        object? sender,
        SpawnPositionRequestPayload value) =>
        _ = QueueSpawnPositionRequestAsync(value);

    private async Task QueueSpawnPositionRequestAsync(SpawnPositionRequestPayload value)
    {
        lock (_stateGate)
        {
            if (_start is null || value.SessionId != _start.SessionId
                || value.LevelGeneration != _start.LevelGeneration) return;
            _pendingSpawnPositionRequest = value;
        }
        _diagnostics.Log("Multiplayer",
            $"Host requested the local spawn position ({value.RequestId:N}).");
        await TryAdvanceGameplayAsync(_controller.Snapshot).ConfigureAwait(false);
    }

    private void OnSpawnRemotePlayersRequested(
        object? sender,
        SpawnRemotePlayersPayload value) =>
        _ = QueueSpawnRemotePlayersAsync(value);

    private async Task QueueSpawnRemotePlayersAsync(SpawnRemotePlayersPayload value)
    {
        if (!AcceptSpawnRequest(value)) return;
        _diagnostics.Log("Multiplayer",
            $"Remote-player spawn request {value.RequestId:N} received from the host.");
        PublishState();
        await TryAdvanceGameplayAsync(_controller.Snapshot).ConfigureAwait(false);
    }

    private bool AcceptSpawnRequest(SpawnRemotePlayersPayload value)
    {
        lock (_stateGate)
        {
            if (_start is null || _lobby is null) return false;
            if (value.SessionId != _start.SessionId
                || value.LevelGeneration != _start.LevelGeneration) return false;
            var host = _lobby.Players.SingleOrDefault(player => player.IsHost);
            if (host?.PeerId != value.HostPeerId) return false;
            if (!RemoteSpawnPlanner.MatchesLobby(value.Players, _lobby.Players)) return false;
            if (_processedSpawnRequests.Contains(value.RequestId)) return false;
            if (_pendingSpawnRequest is not null) return false;
            _pendingSpawnRequest = value;
            foreach (var player in value.Players.Where(player => player.Character is not null))
                _assignedCharacters[player.PeerId] = player.Character!;
            return true;
        }
    }

    private bool TryQueueLocalPosition(DateTimeOffset now)
    {
        if (!_performanceIsolation.IsEnabled(PerformanceIsolationModule.PositionNetworkTransmit))
            return false;
        lock (_stateGate)
        {
            if (_start is null || !_localGameplayReady || !_positionAvailable)
                return false;
            var peerId = LocalPeerId();
            if (peerId == 0 || (_host is null && _client is null)) return false;
            var runtime=_controller.Snapshot;var connection=_controller.AISyncConnectionIdentity;
            var incarnation=_playerIncarnations.Observe(_start.SessionId,peerId,_start.LevelGeneration,runtime,connection);
            _aiSync.SetLocalIncarnation(_start.SessionId,peerId,incarnation,runtime,connection);
            _pendingLocalPosition = new PlayerPositionSnapshotPayload(_start.SessionId, peerId,
                _start.LevelGeneration, ++_localPositionSequence, now.ToUnixTimeMilliseconds(),
                _position.X, _position.Y, _position.Z, _yaw, _moveType, _nativeMoveSpeed, _posture, _torch,
                incarnation, _hiding);
            QueueLocalPostureEdge(_pendingLocalPosition);
            if (_positionSendRunning) return false;
            _positionSendRunning = true;
            return true;
        }
    }

    private async Task PumpLocalPositionsAsync()
    {
        try
        {
            while (TakePendingLocalPosition() is { } value)
                await SendLocalPositionAsync(value).ConfigureAwait(false);
        }
        catch (OperationCanceledException) when (_lifetime.IsCancellationRequested) { }
        catch (Exception exception)
        {
            _diagnostics.Log("Network", $"Position publishing stopped: {exception.Message}");
            lock (_stateGate) { ClearPendingLocalPositions(); _positionSendRunning = false; }
        }
    }

    private PlayerPositionSnapshotPayload? TakePendingLocalPosition()
    {
        lock (_stateGate)
        {
            if (_pendingLocalPostureEdges.TryDequeue(out var edge))
            {
                if (_pendingLocalPosition?.Sequence == edge.Sequence) _pendingLocalPosition = null;
                return edge with { Hiding = PlayerPostureDispatchQueue.Advance(edge.Hiding,
                    DateTimeOffset.UtcNow.ToUnixTimeMilliseconds() - edge.SentAtUnixMilliseconds) };
            }
            var value = _pendingLocalPosition;
            _pendingLocalPosition = null;
            if (value is null) _positionSendRunning = false;
            return value;
        }
    }

    private void QueueLocalPostureEdge(PlayerPositionSnapshotPayload value)
    {
        var posture = value.Posture ?? _lastQueuedPosture;
        var torch = value.Torch ?? _lastQueuedTorch;
        var hiding = value.Hiding ?? _lastQueuedHiding;
        var changed = posture != _lastQueuedPosture || torch?.Enabled != _lastQueuedTorch?.Enabled
            || !PlayerHidingPresentationCodec.SameTransition(hiding, _lastQueuedHiding);
        _lastQueuedPosture = posture;
        _lastQueuedTorch = torch;
        _lastQueuedHiding = hiding;
        if (!changed) return;
        if (_pendingLocalPostureEdges.Count == PlayerPostureDispatchQueue.Capacity)
            _pendingLocalPostureEdges.Dequeue();
        _pendingLocalPostureEdges.Enqueue(value with { Posture = posture, Torch = torch, Hiding = hiding });
    }

    private void ClearPendingLocalPositions()
    {
        _pendingLocalPosition = null;
        _pendingLocalPostureEdges.Clear();
        _lastQueuedPosture = null;
        _lastQueuedTorch = null;
        _lastQueuedHiding = null;
    }

    private Task SendLocalPositionAsync(PlayerPositionSnapshotPayload value)
    {
        LobbyHost? host;
        LobbyClient? client;
        lock (_stateGate) { host = _host; client = _client; }
        if (host is not null) return host.PublishHostPlayerPositionAsync(value, _lifetime.Token);
        if (client is not null) return client.SendPlayerPositionAsync(value, _lifetime.Token);
        throw new InvalidOperationException("The lobby network is disconnected.");
    }

    private bool TryQueueLocalEquipment(DateTimeOffset now)
    {
        if (!_performanceIsolation.IsEnabled(PerformanceIsolationModule.EquipmentSync))
            return false;
        lock (_stateGate)
        {
            if (_start is null || !_localGameplayReady
                || _localEquipmentState is not { Valid: true } state) return false;
            var peerId = LocalPeerId();
            if (peerId == 0 || (_host is null && _client is null)) return false;

            var includeActions =
                _performanceIsolation.IsEnabled(PerformanceIsolationModule.ActionSync);
            var changed = _lastQueuedEquipmentState is null
                || ResolveVisibleLocalEquipmentIndex(_lastQueuedEquipmentState)
                    != ResolveVisibleLocalEquipmentIndex(state)
                || (includeActions && (_lastQueuedEquipmentState.AimHeld != state.AimHeld
                    || _lastQueuedEquipmentState.PrimaryHeld != state.PrimaryHeld
                    || (IsSurvivalEquipmentSession() && RemoteEquipmentSelection.ActionEligibilityChanged(
                        _lastQueuedEquipmentState.CurrentEquipmentIndex, _lastQueuedEquipmentState.EquipmentActive,
                        state.CurrentEquipmentIndex, state.EquipmentActive))));
            if (!changed && now < _nextEquipmentRefreshAtUtc) return false;

            _lastQueuedEquipmentState = state;
            _nextEquipmentRefreshAtUtc = now.AddSeconds(1);
            _pendingLocalEquipment = new PlayerEquipmentSnapshotPayload(
                _start.SessionId, peerId, _start.LevelGeneration, ++_localPlayerSyncSequence,
                now.ToUnixTimeMilliseconds(), state.CurrentEquipmentIndex,
                state.PreviousEquipmentIndex, state.EquipmentActive, state.PresentationState,
                includeActions && state.AimHeld, includeActions && state.PrimaryHeld);

            if (changed)
            {
                _diagnostics.Log("PlayerSync",
                    $"Native local equipment changed: revision={state.Revision}, " +
                    $"current={state.CurrentEquipmentIndex}, previous={state.PreviousEquipmentIndex}, " +
                    $"active={(state.EquipmentActive ? 1 : 0)}, aim={(state.AimHeld ? 1 : 0)}, " +
                    $"primary={(state.PrimaryHeld ? 1 : 0)}.");
            }

            if (_playerSyncSendRunning) return false;
            _playerSyncSendRunning = true;
            return true;
        }
    }

    private async Task PumpLocalPlayerSyncAsync()
    {
        try
        {
            while (true)
            {
                PlayerEquipmentSnapshotPayload? equipment = null;
                PlayerActionEventPayload? action = null;
                lock (_stateGate)
                {
                    var hasEquipment = _pendingLocalEquipment is not null;
                    var hasAction = _pendingLocalPlayerActions.Count != 0;
                    if (!hasEquipment && !hasAction)
                    {
                        _playerSyncSendRunning = false;
                        return;
                    }

                    if (hasEquipment && (!hasAction
                        || _pendingLocalEquipment!.Sequence
                            < _pendingLocalPlayerActions.Peek().Sequence))
                    {
                        equipment = _pendingLocalEquipment;
                        _pendingLocalEquipment = null;
                    }
                    else
                    {
                        action = _pendingLocalPlayerActions.Dequeue();
                    }
                }

                if (equipment is not null)
                    await SendLocalEquipmentAsync(equipment).ConfigureAwait(false);
                else if (action is not null)
                    await SendLocalPlayerActionAsync(action).ConfigureAwait(false);
            }
        }
        catch (OperationCanceledException) when (_lifetime.IsCancellationRequested) { }
        catch (Exception exception)
        {
            _diagnostics.Log("PlayerSync",
                $"PlayerSync publishing stopped: {exception.Message}");
            lock (_stateGate)
            {
                _pendingLocalEquipment = null;
                _pendingLocalPlayerActions.Clear();
                _playerSyncSendRunning = false;
            }
        }
    }

    private Task SendLocalEquipmentAsync(PlayerEquipmentSnapshotPayload value)
    {
        LobbyHost? host;
        LobbyClient? client;
        lock (_stateGate)
        {
            host = _host;
            client = _client;
        }
        if (host is not null)
            return host.PublishHostPlayerEquipmentAsync(value, _lifetime.Token);
        if (client is not null)
            return client.SendPlayerEquipmentAsync(value, _lifetime.Token);
        throw new InvalidOperationException("The lobby network is disconnected.");
    }

    private Task SendLocalPlayerActionAsync(PlayerActionEventPayload value)
    {
        LobbyHost? host;
        LobbyClient? client;
        lock (_stateGate)
        {
            host = _host;
            client = _client;
        }
        if (host is not null)
            return host.PublishHostPlayerActionAsync(value, _lifetime.Token);
        if (client is not null)
            return client.SendPlayerActionAsync(value, _lifetime.Token);
        throw new InvalidOperationException("The lobby network is disconnected.");
    }

    private async Task PumpRemotePlayerActionsAsync()
    {
        try
        {
            while (TakePendingRemotePlayerAction() is { } value)
                await ApplyRemotePlayerActionAsync(value).ConfigureAwait(false);
        }
        catch (OperationCanceledException) when (_lifetime.IsCancellationRequested) { }
        catch (Exception exception)
        {
            _diagnostics.Log("PlayerSync",
                $"Remote player-action application stopped: {exception.Message}");
            lock (_stateGate)
            {
                _pendingRemotePlayerActions.Clear();
                _remotePlayerActionApplyRunning = false;
            }
        }
    }

    private PlayerActionEventPayload? TakePendingRemotePlayerAction()
    {
        lock (_stateGate)
        {
            if (_pendingRemotePlayerActions.Count == 0)
            {
                _remotePlayerActionApplyRunning = false;
                return null;
            }
            return _pendingRemotePlayerActions.Dequeue();
        }
    }

    private async Task ApplyRemotePlayerActionAsync(PlayerActionEventPayload value)
    {
        if (!_performanceIsolation.IsEnabled(PerformanceIsolationModule.ActionSync)) return;
        await _remotePlayerSyncGate.WaitAsync(_lifetime.Token).ConfigureAwait(false);
        try
        {
            uint characterBase;
            PlayerEquipmentSnapshotPayload? equipment;
            lock (_stateGate)
            {
                if (_start is null || value.SessionId != _start.SessionId
                    || value.LevelGeneration != _start.LevelGeneration
                    || !_remoteCharacters.TryGetValue(value.PeerId, out characterBase)
                    || characterBase == 0U)
                    return;
                _remoteEquipment.TryGetValue(value.PeerId, out equipment);
                if (equipment is not null && equipment.Sequence > value.Sequence)
                    return;
            }

            if (value.Action == PlayerActionKind.Reload)
            {
                _diagnostics.Log("PlayerSync",
                    $"Remote PeerId {value.PeerId} entered Reloading (state 9); " +
                    "reload execution is intentionally not replicated yet.");
                return;
            }

            if (value.Active && !await EnsureRemotePlayerEquipmentAsync(
                    value.PeerId, characterBase, value.CurrentEquipmentIndex)
                    .ConfigureAwait(false))
            {
                if (IsSurvivalEquipmentSession())
                    await StopRemotePlayerActionsAsync(value.PeerId, characterBase).ConfigureAwait(false);
                return;
            }

            // Provisioning can await the catalog/Give path. A newer durable
            // snapshot supersedes this edge; a newer action edge does not, because
            // ordered action transitions must all be applied.
            lock (_stateGate)
            {
                if (_remoteEquipment.TryGetValue(value.PeerId, out var newestEquipment))
                {
                    if (newestEquipment.Sequence > value.Sequence) return;
                    equipment = newestEquipment;
                }
            }

            if (value.Action == PlayerActionKind.Aim)
            {
                var aimApplied = false;
                lock (_stateGate) _remoteAppliedAim.TryGetValue(value.PeerId, out aimApplied);
                if (aimApplied == value.Active) return;

                if (await ExecuteRemoteCharacterActionAsync(
                        characterBase, value.Active ? "aim-start" : "aim-stop")
                        .ConfigureAwait(false))
                {
                    lock (_stateGate) _remoteAppliedAim[value.PeerId] = value.Active;
                }
                return;
            }

            if (value.Action == PlayerActionKind.Primary)
            {
                var primaryApplied = false;
                lock (_stateGate) _remoteAppliedPrimary.TryGetValue(value.PeerId, out primaryApplied);
                if (primaryApplied == value.Active) return;

                if (value.Active)
                {
                    var aimApplied = false;
                    lock (_stateGate) _remoteAppliedAim.TryGetValue(value.PeerId, out aimApplied);
                    if (!aimApplied && equipment?.AimHeld == true
                        && (!IsSurvivalEquipmentSession()
                            || (equipment.CurrentEquipmentIndex == value.CurrentEquipmentIndex
                                && equipment.EquipmentActive)))
                    {
                        if (await ExecuteRemoteCharacterActionAsync(characterBase, "aim-start")
                                .ConfigureAwait(false))
                        {
                            lock (_stateGate) _remoteAppliedAim[value.PeerId] = true;
                        }
                    }
                }

                if (await ExecuteRemoteCharacterActionAsync(
                        characterBase, value.Active ? "primary-start" : "primary-stop")
                        .ConfigureAwait(false))
                {
                    lock (_stateGate) _remoteAppliedPrimary[value.PeerId] = value.Active;
                }
            }
        }
        finally
        {
            _remotePlayerSyncGate.Release();
        }
    }

    private async Task ConvergeRemotePlayerEquipmentAsync(ulong peerId)
    {
        var gateHeld = false;
        try
        {
            await _remotePlayerSyncGate.WaitAsync(_lifetime.Token).ConfigureAwait(false);
            gateHeld = true;

            uint characterBase;
            PlayerEquipmentSnapshotPayload? equipment;
            lock (_stateGate)
            {
                if (!_remoteCharacters.TryGetValue(peerId, out characterBase)
                    || characterBase == 0U
                    || !_remoteEquipment.TryGetValue(peerId, out equipment)
                    || equipment is null)
                    return;
            }

            // When first-person is holstered the retail controller may expose
            // current=10 while previous still identifies the weapon the player
            // carries. Keep that weapon visible on the remote Character with Aim
            // off instead of despawning it.
            var visibleEquipmentIndex = ResolveVisibleRemoteEquipmentIndex(equipment);
            if (!await EnsureRemotePlayerEquipmentAsync(
                    peerId, characterBase, visibleEquipmentIndex)
                    .ConfigureAwait(false))
            {
                // Stop inputs for an unsupported selection. A released held
                // flare also needs retail slot cleanup to detach its model/VFX.
                await StopRemotePlayerActionsAsync(peerId, characterBase)
                    .ConfigureAwait(false);
                await ReleaseRemoteHeldFlareAsync(peerId, characterBase).ConfigureAwait(false);
                return;
            }

            if (!_performanceIsolation.IsEnabled(PerformanceIsolationModule.ActionSync))
                return;

            // Provisioning from an older durable snapshot is still useful, but
            // never let its Aim/Primary state overwrite a newer ordered edge
            // that arrived while the Give command was running.
            lock (_stateGate)
            {
                if (_remoteLastPlayerSyncSequence.TryGetValue(peerId, out var newestSequence)
                    && equipment.Sequence < newestSequence)
                    return;
            }

            var canApplyInputs = !IsSurvivalEquipmentSession()
                || RemoteEquipmentSelection.SupportsActions(
                    equipment.CurrentEquipmentIndex, equipment.EquipmentActive);
            if (!canApplyInputs)
            {
                await StopRemotePlayerActionsAsync(peerId, characterBase).ConfigureAwait(false);
                return;
            }
            var desiredAim = equipment.AimHeld;
            var desiredPrimary = equipment.PrimaryHeld && desiredAim;
            bool appliedAim;
            bool appliedPrimary;
            lock (_stateGate)
            {
                _remoteAppliedAim.TryGetValue(peerId, out appliedAim);
                _remoteAppliedPrimary.TryGetValue(peerId, out appliedPrimary);
            }

            if (appliedAim != desiredAim)
            {
                if (await ExecuteRemoteCharacterActionAsync(
                        characterBase, desiredAim ? "aim-start" : "aim-stop")
                        .ConfigureAwait(false))
                {
                    lock (_stateGate) _remoteAppliedAim[peerId] = desiredAim;
                }
            }

            if (appliedPrimary != desiredPrimary)
            {
                if (await ExecuteRemoteCharacterActionAsync(
                        characterBase, desiredPrimary ? "primary-start" : "primary-stop")
                        .ConfigureAwait(false))
                {
                    lock (_stateGate) _remoteAppliedPrimary[peerId] = desiredPrimary;
                }
            }
        }
        catch (OperationCanceledException) when (_lifetime.IsCancellationRequested) { }
        catch (Exception exception)
        {
            _diagnostics.Log("PlayerSync",
                $"Remote PlayerSync convergence failed for PeerId {peerId}: {exception.Message}");
        }
        finally
        {
            if (gateHeld) _remotePlayerSyncGate.Release();
        }
    }

    private bool IsSurvivalEquipmentSession()
    {
        lock (_stateGate) return _start is not null && LevelGameMode(_start.Level) == "Survival";
    }

    private async Task ReleaseRemoteHeldFlareAsync(ulong peerId, uint characterBase)
    {
        if (!TryGetRemoteHeldFlareContext(peerId, out var player, out var preparation)
            || player is null || preparation is null) return;
        var catalogs = await preparation.ConfigureAwait(false);
        if (catalogs is null || !catalogs.TryGet("flare-held", out var catalog)) return;
        await _remoteEquipmentProvisionGate.WaitAsync(_lifetime.Token).ConfigureAwait(false);
        try
        {
            if (await ProvisionRemoteEquipmentAsync(player, characterBase, catalog, releaseHeld: true).ConfigureAwait(false))
                lock (_stateGate) _remoteProvisionedEquipment.Remove(peerId);
        }
        finally { _remoteEquipmentProvisionGate.Release(); }
    }

    private bool TryGetRemoteHeldFlareContext(ulong peerId, out LobbyPlayer? player,
        out Task<RemoteEquipmentCatalogSet?>? preparation)
    {
        lock (_stateGate)
        {
            player = null; preparation = null;
            if (!_remoteProvisionedEquipment.TryGetValue(peerId, out var index) || index is not (32 or 33)) return false;
            player = _lobby?.Players.FirstOrDefault(value => value.PeerId == peerId);
            preparation = _remoteEquipmentCatalogPreparation;
            return player is not null && preparation is not null;
        }
    }

    private int ResolveVisibleLocalEquipmentIndex(
        NativePlayerEquipmentStatePayload equipment)
    {
        if (IsSurvivalEquipmentSession())
            return RemoteEquipmentSelection.VisibleIndex(equipment.CurrentEquipmentIndex,
                equipment.PreviousEquipmentIndex, equipment.EquipmentActive);
        if (RemoteEquipmentDefinitions.TryGetPlayerSyncEquipmentId(
                equipment.CurrentEquipmentIndex, out _))
            return equipment.CurrentEquipmentIndex;
        if (RemoteEquipmentDefinitions.TryGetPlayerSyncEquipmentId(
                equipment.PreviousEquipmentIndex, out _))
            return equipment.PreviousEquipmentIndex;
        return equipment.CurrentEquipmentIndex;
    }

    private int ResolveVisibleRemoteEquipmentIndex(
        PlayerEquipmentSnapshotPayload equipment)
    {
        if (IsSurvivalEquipmentSession())
            return RemoteEquipmentSelection.VisibleIndex(equipment.CurrentEquipmentIndex,
                equipment.PreviousEquipmentIndex, equipment.EquipmentActive);
        if (RemoteEquipmentDefinitions.TryGetPlayerSyncEquipmentId(
                equipment.CurrentEquipmentIndex, out _))
            return equipment.CurrentEquipmentIndex;
        if (RemoteEquipmentDefinitions.TryGetPlayerSyncEquipmentId(
                equipment.PreviousEquipmentIndex, out _))
            return equipment.PreviousEquipmentIndex;
        return equipment.CurrentEquipmentIndex;
    }

    private async Task<bool> EnsureRemotePlayerEquipmentAsync(
        ulong peerId,
        uint characterBase,
        int equipmentIndex)
    {
        // Flare 32/33 are wire-only inventory identities, not NPC equipment
        // slots. Giving Flare_Held to a replicated Character can execute retail
        // first-person-only code and crash the observing game instance.
        if (equipmentIndex is 32 or 33)
        {
            _diagnostics.Log("PlayerSync",
                $"Remote PeerId {peerId}: held flare {equipmentIndex} kept as observation only; " +
                "no native equipment Give, activation or hand selection.");
            return false;
        }
        int provisionedIndex;
        lock (_stateGate)
        {
            if (_remoteProvisionedEquipment.TryGetValue(peerId, out provisionedIndex)
                && provisionedIndex == equipmentIndex)
                return true;
        }

        if (!RemoteEquipmentDefinitions.TryGetPlayerSyncEquipmentId(
                equipmentIndex, out var equipmentId))
            return false;
        if (!RemoteEquipmentDefinitions.TryGet(equipmentId, out var definition)
            || !definition.SelectorEnabled)
        {
            _diagnostics.Log("PlayerSync",
                $"Remote equipment index {equipmentIndex} ('{equipmentId}') is not provisionable by the certified bridge yet.");
            return false;
        }

        LobbyPlayer? player;
        Task<RemoteEquipmentCatalogSet?>? preparation;
        lock (_stateGate)
        {
            player = _lobby?.Players.FirstOrDefault(value => value.PeerId == peerId);
            preparation = _remoteEquipmentCatalogPreparation;
        }
        if (player is null || preparation is null) return false;

        var catalogs = await preparation.ConfigureAwait(false);
        if (catalogs is null || !catalogs.TryGet(equipmentId, out var catalog))
            return false;

        // A new equipment identity must never inherit a held cycle from the
        // previously-provisioned item.
        await StopRemotePlayerActionsAsync(peerId, characterBase).ConfigureAwait(false);

        await _remoteEquipmentProvisionGate.WaitAsync(_lifetime.Token).ConfigureAwait(false);
        bool provisioned;
        try
        {
            provisioned = await ProvisionRemoteEquipmentAsync(
                player, characterBase, catalog, equipmentIndex == 33).ConfigureAwait(false);
        }
        finally
        {
            _remoteEquipmentProvisionGate.Release();
        }
        if (!provisioned) return false;

        lock (_stateGate)
        {
            if (_remoteCharacters.TryGetValue(peerId, out var currentCharacter)
                && currentCharacter == characterBase)
            {
                _remoteProvisionedEquipment[peerId] = equipmentIndex;
                _remoteAppliedAim[peerId] = false;
                _remoteAppliedPrimary[peerId] = false;
            }
        }
        return true;
    }

    private async Task StopRemotePlayerActionsAsync(ulong peerId, uint characterBase)
    {
        bool primary;
        bool aim;
        lock (_stateGate)
        {
            _remoteAppliedPrimary.TryGetValue(peerId, out primary);
            _remoteAppliedAim.TryGetValue(peerId, out aim);
        }

        if (primary && await ExecuteRemoteCharacterActionAsync(
                characterBase, "primary-stop").ConfigureAwait(false))
        {
            lock (_stateGate) _remoteAppliedPrimary[peerId] = false;
        }
        if (aim && await ExecuteRemoteCharacterActionAsync(
                characterBase, "aim-stop").ConfigureAwait(false))
        {
            lock (_stateGate) _remoteAppliedAim[peerId] = false;
        }
    }

    private async Task<bool> ExecuteRemoteCharacterActionAsync(
        uint characterBase,
        string action)
    {
        try
        {
            var arguments = new Dictionary<string, string>(StringComparer.Ordinal)
            {
                ["characterAddress"] = $"0x{characterBase:X8}",
                ["action"] = action
            };
            var result = await _controller.ExecuteReplicationCommandAsync(
                GameCommandNames.CharacterActionTest, arguments, _lifetime.Token)
                .ConfigureAwait(false);
            if (result.Executed) return true;

            _diagnostics.Log("PlayerSync",
                $"Remote CharacterAction '{action}' rejected for 0x{characterBase:X8}: " +
                $"{result.StatusCode} - {result.Message}");
            return false;
        }
        catch (Exception exception) when (exception is ProtocolException
            or IOException or InvalidOperationException or TimeoutException)
        {
            _diagnostics.Log("PlayerSync",
                $"Remote CharacterAction '{action}' failed for 0x{characterBase:X8}: {exception.Message}");
            return false;
        }
    }

    private async Task TryAdvanceRemoteMovementAsync(
        NativeRuntimeSnapshot runtime,
        DateTimeOffset now)
    {
        if (!_performanceIsolation.IsEnabled(PerformanceIsolationModule.RemoteMovement)) return;
        if (!TryBeginRemoteMovement(runtime, out var targets)) return;
        try
        {
            foreach (var target in targets)
                if (!await ExecuteRemoteMovementBestEffortAsync(target, now).ConfigureAwait(false)) break;
        }
        finally { lock (_stateGate) _movementCommandRunning = false; }
    }

    private async Task<bool> ExecuteRemoteMovementBestEffortAsync(
        RemoteMovementTarget target,
        DateTimeOffset now)
    {
        try
        {
            await ExecuteRemoteMovementAsync(target, now).ConfigureAwait(false);
            return true;
        }
        catch (OperationCanceledException) when (_lifetime.IsCancellationRequested) { return false; }
        catch (Exception exception)
        {
            LogRemoteMovementFailure(target, exception.Message);
            return true;
        }
    }


    private void LogRemoteMovementFailure(RemoteMovementTarget target, string message)
    {
        var now = DateTimeOffset.UtcNow;
        var key = $"{target.PeerId}:{target.CharacterBase:X8}:{message}";
        lock (_stateGate)
        {
            if (key == _lastMovementFailure && now - _lastMovementFailureLoggedAt < TimeSpan.FromSeconds(2))
                return;
            _lastMovementFailure = key;
            _lastMovementFailureLoggedAt = now;
        }
        _diagnostics.Log("Replication",
            $"Remote movement update failed for PeerId {target.PeerId} / Character 0x{target.CharacterBase:X8}: {message}");
    }

    private bool TryBeginRemoteMovement(
        NativeRuntimeSnapshot runtime,
        out IReadOnlyList<RemoteMovementTarget> targets)
    {
        lock (_stateGate)
        {
            targets = Array.Empty<RemoteMovementTarget>();
            if (_movementCommandRunning || _start is null || !_localGameplayReady) return false;
            if (!runtime.IpcConnected || !runtime.EngineThreadReady) return false;

            var selected = SelectMovementTargets();
            if (selected.Count == 0) return false;
            _movementCommandRunning = true;
            targets = selected;
            return true;
        }
    }

    private IReadOnlyList<RemoteMovementTarget> SelectMovementTargets()
    {
        var targets = new List<RemoteMovementTarget>(_remoteCharacters.Count);
        var generation = _start?.LevelGeneration ?? 0;
        foreach (var pair in _remoteCharacters.OrderBy(value => value.Key))
        {
            if (!_remotePositions.TryGetValue(pair.Key, out var track)) continue;
            track.BindCharacter(pair.Value);
            var snapshot = track.Snapshot();
            if (snapshot is not null)
                targets.Add(new RemoteMovementTarget(
                    pair.Key, pair.Value, generation, snapshot));
        }
        return targets;
    }

    private async Task ExecuteRemoteMovementAsync(
        RemoteMovementTarget target,
        DateTimeOffset now)
    {
        if (!IsMovementTargetCurrent(target)) return;
        var plan = RemoteMovementPlanner.Create(target.Track, now);
        if (plan.Action == RemoteMovementAction.None || !IsMovementTargetCurrent(target)) return;
        var arguments = RemoteMovementCommandFactory.Create(target.CharacterBase, plan);
        var result = await _controller.ExecuteReplicationCommandAsync(
            GameCommandNames.CharacterDirectMove, arguments, _lifetime.Token)
            .ConfigureAwait(false);
        EnsureMovementAccepted(result);
        CompleteMovementCommand(target, plan, DateTimeOffset.UtcNow);
    }

    private bool IsMovementTargetCurrent(RemoteMovementTarget target)
    {
        lock (_stateGate)
        {
            return _start?.LevelGeneration == target.LevelGeneration
                && _remoteCharacters.TryGetValue(target.PeerId, out var characterBase)
                && characterBase == target.CharacterBase;
        }
    }

    private static void EnsureMovementAccepted(GameCommandResultPayload result)
    {
        if (result.Executed || NativeRuntimeController.IsQueuedResult(result)) return;
        throw new InvalidOperationException(
            $"Character Direct Move was rejected: {result.StatusCode} - {result.Message}");
    }

    private void CompleteMovementCommand(
        RemoteMovementTarget target,
        RemoteMovementPlan plan,
        DateTimeOffset appliedAtUtc)
    {
        var moving = plan.Action == RemoteMovementAction.Move;
        var previousMoving = target.Track.CommandMoving;
        var applied = false;
        lock (_stateGate)
        {
            if (_start?.LevelGeneration == target.LevelGeneration
                && _remoteCharacters.TryGetValue(target.PeerId, out var characterBase)
                && characterBase == target.CharacterBase
                && _remotePositions.TryGetValue(target.PeerId, out var track))
            {
                track.MarkCommand(plan, appliedAtUtc);
                applied = true;
            }
        }
        if (applied && moving != previousMoving) LogMovementTransition(target, plan, moving);
    }

    private void LogMovementTransition(
        RemoteMovementTarget target,
        RemoteMovementPlan plan,
        bool moving) => _diagnostics.Log("Replication", moving
            ? $"Remote Character 0x{target.CharacterBase:X8} started replicating PeerId {target.PeerId}; authoritativePosition=({plan.PositionX:0.000},{plan.PositionY:0.000},{plan.PositionZ:0.000}), sourceStep={plan.RemoteStepDistance:0.000}, moveType={plan.MoveType}, nativeMoveSpeed={plan.NativeMoveSpeed:0.000}. The engine thread drives the native locomotion cycle; no GoTo destination is created."
            : $"Remote Character 0x{target.CharacterBase:X8} stopped for PeerId {target.PeerId}.");

    private void OnNetworkError(object? sender, ErrorPayload value)
    {
        _ = SendErrorAsync($"{value.Code}: {value.Message}");
        if (value.Code == "MISSION_CATALOG_MISMATCH") _ = ShowPreparationFailureAsync(value.Message);
    }

    private void OnClientDisconnected(object? sender, EventArgs value) =>
        _ = HandleClientDisconnectedAsync(sender as LobbyClient);

    private async Task HandleClientDisconnectedAsync(LobbyClient? disconnected)
    {
        await Task.Yield();
        string? departedHost;
        long stopVersion;
        await _commandGate.WaitAsync(_lifetime.Token).ConfigureAwait(false);
        try
        {
            lock (_stateGate)
                if (disconnected is null || !ReferenceEquals(disconnected, _client)) return;
            departedHost = CaptureInGameHostName();
            await StopNetworkAsync("Disconnected from host").ConfigureAwait(false);
            lock (_stateGate) stopVersion = _networkStopVersion;
        }
        finally { _commandGate.Release(); }
        if (departedHost is not null) await HandleHostLeftGameAsync(departedHost, stopVersion).ConfigureAwait(false);
    }

    private void OnNetworkLog(object? sender, string value) =>
        _diagnostics.Log("Network", value);

    private void BeginRemoteEquipmentCatalogPreparation(LobbyStartPayload start)
    {
        var settings = _settingsStore.Load();
        var level = start.Level;
        var runtimeExecutable = _controller.Snapshot.ExecutablePath;
        var runtimeGameDirectory = ResolveRuntimeGameDirectory(runtimeExecutable);
        var gameDirectory = runtimeGameDirectory ?? settings.GameDirectory;
        var task = Task.Run<RemoteEquipmentCatalogSet?>(() =>
        {
            try
            {
                var source = WorldManifestAutoResolver.ResolveLevelSource(
                    gameDirectory, level, settings.DeveloperMode || LevelGameMode(level) == "Campaign");
                var catalogs = RemoteEquipmentCatalogCompiler.CompileAll(
                    source, out var diagnostic);
                _diagnostics.Log("PlayerSync",
                    $"Remote equipment catalog: level={level} {diagnostic}");
                return catalogs;
            }
            catch (Exception exception) when (exception is not OutOfMemoryException)
            {
                if (!_lifetime.IsCancellationRequested)
                    _diagnostics.Log("PlayerSync",
                        $"Remote equipment catalog unavailable for '{level}': " +
                        $"{exception.GetType().Name}: {exception.Message}");
                return null;
            }
        }, _lifetime.Token);
        lock (_stateGate)
        {
            if (_start?.Level == level)
                _remoteEquipmentCatalogPreparation = task;
        }
    }

    private void BeginAutomaticWorldManifestPreparation(LobbyStartPayload start)
    {
        lock (_stateGate)
        {
            if (!_worldSyncEnabledForSession || _start?.SessionId != start.SessionId
                || _start.LevelGeneration != start.LevelGeneration) return;
            // Preparation belongs to the lobby. Gameplay consumes its verified
            // cache entry and never compiles level archives on the engine path.
            _automaticWorldManifestPreparation = Task.FromResult(_preflightManifest);
            if (_preflightManifest is null)
                _diagnostics.Log("Preparation", "No verified lobby manifest is available; session remains held.");
        }
    }

    private static string? ResolveRuntimeGameDirectory(string executablePath)
    {
        if (string.IsNullOrWhiteSpace(executablePath)) return null;
        try
        {
            var fullPath = Path.GetFullPath(executablePath);
            if (!string.Equals(Path.GetFileName(fullPath), "AI.exe",
                    StringComparison.OrdinalIgnoreCase))
                return null;
            var directory = Path.GetDirectoryName(fullPath);
            return string.IsNullOrWhiteSpace(directory) ? null : directory;
        }
        catch (ArgumentException)
        {
            return null;
        }
        catch (NotSupportedException)
        {
            return null;
        }
    }

    private async Task TryActivateAutomaticWorldSyncAsync(NativeRuntimeSnapshot runtime)
    {
        if (!_performanceIsolation.IsEnabled(PerformanceIsolationModule.WorldSync)) return;
        LobbyHost? host;
        LobbyStartPayload loadingStart;
        Task<PreparedWorldManifest?>? preparation;
        lock (_stateGate)
        {
            host = _host;
            if (!_worldSyncEnabledForSession || host is null || _start is null || !_localGameplayReady
                || _localWorldContext is { Ready: true }
                || _worldActivationRunning || _worldActivationAttempted
                || DateTimeOffset.UtcNow < _nextWorldActivationRetryUtc
                || !runtime.IpcConnected || !runtime.EngineThreadReady || runtime.LevelGeneration <= 0)
                return;
            loadingStart = _start;
            preparation = _automaticWorldManifestPreparation;
            if (preparation is null) return;
            _worldActivationRunning = true;
            _worldActivationAttempted = true;
        }

        try
        {
            var prepared = await preparation.ConfigureAwait(false);
            if (prepared is null) return;
            var nativeGeneration = runtime.LevelGeneration;
            await EnsureNativeManifestLoadedAsync(prepared, loadingStart, nativeGeneration).ConfigureAwait(false);
            EnsureManifestLoadContext(loadingStart, nativeGeneration);

            WorldNetworkContext context;
            lock (_stateGate)
            {
                EnsureManifestLoadContext(loadingStart, nativeGeneration);
                _worldSessionGeneration = _worldGenerations.Next(host.SessionId);
                context = new WorldNetworkContext(
                    host.SessionId,
                    host.HostPeerId,
                    _worldSessionGeneration,
                    prepared.Identity,
                    prepared.Sha256,
                    true);
                _localWorldContext = context;
                _localWorldRuntimeGeneration = _controller.Snapshot.LevelGeneration;
            }
            await host.SetHostWorldContextAsync(context, _lifetime.Token).ConfigureAwait(false);
            if (!await WaitForAutomaticWorldCompatibilityAsync(host, context).ConfigureAwait(false))
                return;
            EnsureManifestLoadContext(loadingStart, nativeGeneration);
            var nativeOpened = await SetNativeWorldSyncLifecycleAsync(
                context.SessionId, host.HostPeerId, true, context.WorldGeneration,
                true, true, "world-context-compatible").ConfigureAwait(false);
            if (!nativeOpened)
                throw new InvalidOperationException(
                    "Compatible WorldContext was established but native authority/bus binding failed.");
            _diagnostics.Log("WorldSync",
                $"Automatic CATHODE world bus active: generation={context.WorldGeneration} " +
                $"map={context.Manifest.MapContentId}.");
        }
        catch (Exception exception) when (exception is not OutOfMemoryException)
        {
            if (!_lifetime.IsCancellationRequested && exception is not OperationCanceledException)
            {
                _diagnostics.Log("WorldSync",
                    $"World preparation remains held, retrying in 2 s: {exception.Message}");
                await ShowPreparationFailureAsync(exception.Message).ConfigureAwait(false);
            }
            lock (_stateGate)
            {
                if (_start?.SessionId == loadingStart.SessionId && _start.LevelGeneration == loadingStart.LevelGeneration
                    && _localWorldContext is not { Ready: true })
                {
                    _worldActivationAttempted = false;
                    _nextWorldActivationRetryUtc = DateTimeOffset.UtcNow.AddSeconds(2);
                }
            }
        }
        finally
        {
            lock (_stateGate)
                if (_start?.SessionId == loadingStart.SessionId && _start.LevelGeneration == loadingStart.LevelGeneration) _worldActivationRunning = false;
        }
    }

    private async Task<bool> WaitForAutomaticWorldCompatibilityAsync(
        LobbyHost host,
        WorldNetworkContext context)
    {
        var nextProgressLog = DateTimeOffset.UtcNow + TimeSpan.FromSeconds(30);
        while (!_lifetime.IsCancellationRequested)
        {
            lock (_stateGate)
            {
                if (!ReferenceEquals(_host, host)
                    || !_worldSyncEnabledForSession
                    || _start is null
                    || _localWorldContext is not { Ready: true } current
                    || !SameWorldContext(current, context))
                    return false;
            }

            if (host.AreWorldPeersCompatible())
                return true;

            var now = DateTimeOffset.UtcNow;
            if (now >= nextProgressLog)
            {
                _diagnostics.Log("WorldSync",
                    $"Automatic CATHODE world activation still waiting for compatible peer WorldContext: " +
                    $"generation={context.WorldGeneration} map={context.Manifest.MapContentId}.");
                nextProgressLog = now + TimeSpan.FromSeconds(30);
            }

            await Task.Delay(100, _lifetime.Token).ConfigureAwait(false);
        }

        return false;
    }

    private async Task TryAdvanceGameplayAsync(NativeRuntimeSnapshot runtime)
    {
        _ = PreloadCurrentManifestAsync(runtime);
        _ = TryCompleteSessionPreparationAsync();
        if (ShouldSendLocalReady(runtime))
            await SendLocalGameplayReadyAsync().ConfigureAwait(false);
        await TryActivateAutomaticWorldSyncAsync(runtime).ConfigureAwait(false);
        await TrySendPendingSpawnPositionAsync(runtime).ConfigureAwait(false);
        await TryRunPendingSpawnRequestAsync(runtime).ConfigureAwait(false);
    }

    private async Task TrySendPendingSpawnPositionAsync(NativeRuntimeSnapshot runtime)
    {
        if (!TryBeginSpawnPositionResponse(runtime)) return;
        try { await SendPendingSpawnPositionAsync().ConfigureAwait(false); }
        catch { lock (_stateGate) _spawnPositionSending = false; throw; }
    }

    private bool TryBeginSpawnPositionResponse(NativeRuntimeSnapshot runtime)
    {
        lock (_stateGate)
        {
            if (_pendingSpawnPositionRequest is null || _spawnPositionSending) return false;
            if (!_uiGameplayLoaded || !_positionAvailable || !_localGameplayReady) return false;
            if (!_gameplayReadiness.IsReady(DateTimeOffset.UtcNow)) return false;
            if (!runtime.IpcConnected || !runtime.EngineThreadReady || _client is null) return false;
            _spawnPositionSending = true;
            return true;
        }
    }

    private async Task SendPendingSpawnPositionAsync()
    {
        var state = GetPendingSpawnPositionState();
        await state.Client.SendSpawnPositionAsync(state.Request, state.Position,
            _lifetime.Token).ConfigureAwait(false);
        lock (_stateGate) { _pendingSpawnPositionRequest = null; _spawnPositionSending = false; }
        _diagnostics.Log("Multiplayer",
            $"Local spawn position sent to host: X={state.Position.X:0.000}, " +
            $"Y={state.Position.Y:0.000}, Z={state.Position.Z:0.000}.");
    }

    private (LobbyClient Client, SpawnPositionRequestPayload Request,
        PlayerSpawnPositionPayload Position) GetPendingSpawnPositionState()
    {
        lock (_stateGate)
        {
            var client = _client ?? throw new InvalidOperationException("Lobby client is missing.");
            var request = _pendingSpawnPositionRequest
                ?? throw new InvalidOperationException("No spawn-position request is pending.");
            return (client, request, RemoteSpawnPlanner.CreateNetworkPosition(client.PeerId, _position));
        }
    }

    private bool ShouldSendLocalReady(NativeRuntimeSnapshot runtime)
    {
        lock (_stateGate)
        {
            if (_start is null || _localGameplayReady || _localReadySending) return false;
            if (!_uiGameplayLoaded || !_positionAvailable) return false;
            if (!_gameplayReadiness.IsReady(DateTimeOffset.UtcNow)) return false;
            if (!runtime.IpcConnected || !runtime.EngineThreadReady) return false;
            _localReadySending = true;
            return true;
        }
    }

    private async Task SendLocalGameplayReadyAsync()
    {
        try
        {
            var generation = GetStartGeneration();
            await SendReadyOverNetworkAsync(generation).ConfigureAwait(false);
            lock (_stateGate) { _localGameplayReady = true; _localReadySending = false; }
            _diagnostics.Log("Multiplayer",
                "Local player is in gameplay; readiness sent. Per-peer Character convergence is active.");
            PublishState();
            await TryConvergeRemoteCharactersAsync(_controller.Snapshot, DateTimeOffset.UtcNow)
                .ConfigureAwait(false);
        }
        catch
        {
            lock (_stateGate) _localReadySending = false;
            throw;
        }
    }

    private int GetStartGeneration()
    {
        lock (_stateGate) return _start?.LevelGeneration
            ?? throw new InvalidOperationException("No active lobby start generation.");
    }

    private async Task SendReadyOverNetworkAsync(int generation)
    {
        LobbyHost? host;
        LobbyClient? client;
        lock (_stateGate) { host = _host; client = _client; }
        if (host is not null)
            await host.SetHostGameplayReadyAsync(generation).ConfigureAwait(false);
        else if (client is not null)
            await client.SendGameplayReadyAsync(generation).ConfigureAwait(false);
        else throw new InvalidOperationException("The lobby network is disconnected.");
    }

    private async Task TryConvergeRemoteCharactersAsync(
        NativeRuntimeSnapshot runtime,
        DateTimeOffset now)
    {
        if (!_performanceIsolation.IsEnabled(PerformanceIsolationModule.RemoteCharacterSpawn)) return;
        RemotePlayerSpawnTarget target;
        long epoch;
        lock (_stateGate)
        {
            if (!TryBeginRemoteCharacterConvergence(runtime, now, out target)) return;
            epoch = _remoteSpawnEpoch;
        }

        var gateEntered = false;
        try
        {
            await _spawnGate.WaitAsync(_lifetime.Token).ConfigureAwait(false);
            gateEntered = true;

            // The target may have moved while another local spawn command owned
            // the gate. Re-read its latest live snapshot rather than spawning at
            // the position that originally caused convergence to begin.
            if (!TryRefreshIndependentSpawnTarget(target.Player.PeerId, out target))
                return;

            await SpawnRemotePlayerAsync(target).ConfigureAwait(false);
            lock (_stateGate)
                if (epoch == _remoteSpawnEpoch)
                    RegisterIndependentSpawnSuccess(target.Player.PeerId);
        }
        catch (RemoteSpawnContextChangedException)
        {
            _diagnostics.Log("Multiplayer", "Stopped observing a Character spawn from an ended context.");
        }
        catch (OperationCanceledException) when (_lifetime.IsCancellationRequested)
        {
        }
        catch (Exception exception)
        {
            lock (_stateGate)
                if (epoch == _remoteSpawnEpoch)
                    RegisterIndependentSpawnFailure(target.Player, exception);
        }
        finally
        {
            lock (_stateGate)
                if (epoch == _remoteSpawnEpoch) _remoteSpawnConvergenceRunning = false;
            if (gateEntered) _spawnGate.Release();

            // Drain the next eligible peer immediately instead of relying on
            // a later UI/runtime/network callback to restart convergence.
            if (!_lifetime.IsCancellationRequested)
                _ = TryConvergeRemoteCharactersAsync(
                    _controller.Snapshot, DateTimeOffset.UtcNow);
        }
    }

    private bool TryBeginRemoteCharacterConvergence(
        NativeRuntimeSnapshot runtime,
        DateTimeOffset now,
        out RemotePlayerSpawnTarget target)
    {
        target = default!;
        if (_remoteSpawnConvergenceRunning || _start is null || _lobby is null
            || !_localGameplayReady || !_uiGameplayLoaded)
            return false;
        if (!runtime.IpcConnected || !runtime.EngineThreadReady
            || !runtime.CharacterCreateHookInstalled)
            return false;
        // Manifest registration can occupy the engine thread for seconds.
        // Do not queue short-lived spawn commands behind it during preparation.
        if (_loadingHoldStart is not null && _worldSyncEnabledForSession
            && (_loadedManifest is not { } loaded || loaded.Session != _start.SessionId
                || loaded.Start != _start.LevelGeneration || loaded.Native != runtime.LevelGeneration
                || loaded.Process != runtime.ProcessId)) return false;

        var localPeerId = LocalPeerId();
        foreach (var player in _lobby.Players
                     .Where(player => player.PeerId != localPeerId)
                     .OrderBy(player => player.PeerId))
        {
            if (_remoteCharacters.ContainsKey(player.PeerId)) continue;
            if (!_remoteGameplayReadyPeers.Contains(player.PeerId)) continue;
            if (_remoteSpawnRetryNotBeforeUtc.TryGetValue(player.PeerId, out var retryAt)
                && now < retryAt)
                continue;
            if (!TryCreateSpawnTarget(player, out target)) continue;
            _remoteSpawnConvergenceRunning = true;
            return true;
        }
        return false;
    }

    private bool TryRefreshIndependentSpawnTarget(
        ulong peerId,
        out RemotePlayerSpawnTarget target)
    {
        lock (_stateGate)
        {
            target = default!;
            if (_start is null || _lobby is null || !_localGameplayReady
                || _remoteCharacters.ContainsKey(peerId)
                || !_remoteGameplayReadyPeers.Contains(peerId))
                return false;
            var player = _lobby.Players.SingleOrDefault(value => value.PeerId == peerId);
            return player is not null && TryCreateSpawnTarget(player, out target);
        }
    }

    private bool TryCreateSpawnTarget(
        LobbyPlayer player,
        out RemotePlayerSpawnTarget target)
    {
        target = default!;
        if (!_assignedCharacters.TryGetValue(player.PeerId, out var character)) return false;
        if (!_remotePositions.TryGetValue(player.PeerId, out var track)) return false;
        var snapshot = track.Snapshot();
        if (snapshot is null) return false;
        var latest = snapshot.Latest;
        if (!float.IsFinite(latest.X) || !float.IsFinite(latest.Y) || !float.IsFinite(latest.Z))
            return false;

        // RemotePositionTrack is reset on LobbyStart and only accepts packets
        // for the active SessionId/LevelGeneration. Once current-generation
        // position evidence exists, an arbitrary one-second freshness window
        // must not prevent Character creation. Movement convergence will catch
        // up if the peer moved after this snapshot.
        target = new RemotePlayerSpawnTarget(
            player,
            RemoteSpawnPlanner.CreateNetworkPosition(latest) with
            {
                Character = character
            });
        return true;
    }

    private void RegisterIndependentSpawnSuccess(ulong peerId)
    {
        lock (_stateGate)
        {
            _remoteSpawnRetryAttempts.Remove(peerId);
            _remoteSpawnRetryNotBeforeUtc.Remove(peerId);
        }
    }

    private void RegisterIndependentSpawnFailure(LobbyPlayer player, Exception exception)
    {
        int attempt;
        DateTimeOffset retryAt;
        lock (_stateGate)
        {
            _remoteSpawnRetryAttempts.TryGetValue(player.PeerId, out var previousAttempt);
            attempt = previousAttempt + 1;
            _remoteSpawnRetryAttempts[player.PeerId] = attempt;
            var shift = Math.Clamp(attempt - 1, 0, 4);
            var delayMilliseconds = Math.Min(500 * (1 << shift), 5000);
            retryAt = DateTimeOffset.UtcNow.AddMilliseconds(delayMilliseconds);
            _remoteSpawnRetryNotBeforeUtc[player.PeerId] = retryAt;
        }
        _diagnostics.Log("Multiplayer",
            $"Independent Character spawn for {player.DisplayName} failed on attempt {attempt}: " +
            $"{exception.Message}. Automatic retry after {retryAt:O}.");
    }

    private async Task TryRunPendingSpawnRequestAsync(NativeRuntimeSnapshot runtime)
    {
        if (!TryBeginPendingSpawnRequest(runtime)) return;
        await _spawnGate.WaitAsync(_lifetime.Token).ConfigureAwait(false);
        try { await ExecutePendingSpawnRequestAsync().ConfigureAwait(false); }
        catch (OperationCanceledException) when (_lifetime.IsCancellationRequested) { }
        catch (RemoteSpawnContextChangedException) { }
        catch (Exception exception) { RegisterSpawnRequestFailure(exception); }
        finally
        {
            lock (_stateGate) _spawnRequestRunning = false;
            _spawnGate.Release();
        }
    }

    private bool TryBeginPendingSpawnRequest(NativeRuntimeSnapshot runtime)
    {
        if (!_performanceIsolation.IsEnabled(PerformanceIsolationModule.RemoteCharacterSpawn))
            return false;
        lock (_stateGate)
        {
            if (_pendingSpawnRequest is null || _spawnRequestRunning) return false;
            if (!_uiGameplayLoaded || !_positionAvailable) return false;
            if (!_gameplayReadiness.IsReady(DateTimeOffset.UtcNow)) return false;
            if (!runtime.IpcConnected || !runtime.EngineThreadReady) return false;
            if (!runtime.CharacterCreateHookInstalled) return false;
            _spawnRequestRunning = true;
            return true;
        }
    }

    private async Task ExecutePendingSpawnRequestAsync()
    {
        var request = GetPendingSpawnRequest();
        var players = GetUnspawnedRemotePlayers();
        if (players.Count == 0)
            _diagnostics.Log("Multiplayer", "All connected remote players already have Characters.");
        foreach (var player in players)
            await SpawnRemotePlayerAsync(player).ConfigureAwait(false);
        CompleteSpawnRequest(request.RequestId);
    }

    private SpawnRemotePlayersPayload GetPendingSpawnRequest()
    {
        lock (_stateGate) return _pendingSpawnRequest
            ?? throw new InvalidOperationException("No remote-player spawn request is pending.");
    }

    private IReadOnlyList<RemotePlayerSpawnTarget> GetUnspawnedRemotePlayers()
    {
        lock (_stateGate) return RemoteSpawnPlanner.SelectPendingPlayers(
            _lobby, _pendingSpawnRequest?.Players, LocalPeerId(), _remoteCharacters);
    }

    private RemoteSpawnContext CaptureRemoteSpawnContext(NativeRuntimeSnapshot runtime,ulong peer)
    {
        lock (_stateGate)
        {
            if (_start is null || !runtime.AiRunning || runtime.ProcessId == 0)
                throw new RemoteSpawnContextChangedException();
            return new(_start.SessionId, _start.LevelGeneration,
                runtime.LevelGeneration, runtime.ProcessId, _remoteSpawnEpoch,peer,
                _remotePositions.TryGetValue(peer,out var track)?track.Snapshot()?.Latest.CharacterIncarnation??0:0,
                _controller.AISyncConnectionIdentity);
        }
    }

    private void EnsureRemoteSpawnContext(RemoteSpawnContext context, NativeRuntimeSnapshot runtime)
    {
        lock (_stateGate)
        {
            if (_start is null || _start.SessionId != context.SessionId
                || _start.LevelGeneration != context.WorldGeneration
                || _remoteSpawnEpoch != context.Epoch
                || _controller.AISyncConnectionIdentity!=context.ConnectionIdentity
                || (_remotePositions.TryGetValue(context.Peer,out var track)?track.Snapshot()?.Latest.CharacterIncarnation??0:0)!=context.Incarnation
                || !runtime.AiRunning || runtime.ProcessId != context.ProcessId
                || runtime.LevelGeneration != context.NativeGeneration)
                throw new RemoteSpawnContextChangedException();
        }
    }

    private async Task SpawnRemotePlayerAsync(RemotePlayerSpawnTarget target)
    {
        var initialRuntime = _controller.Snapshot;
        var context = CaptureRemoteSpawnContext(initialRuntime,target.Player.PeerId);
        if (TryResumeInFlightSpawn(target.Player.PeerId, context, out var inFlight))
        {
            _diagnostics.Log("Multiplayer",
                $"Resuming the accepted Character spawnKey={inFlight.SpawnKey?.ToString() ?? "pending"} " +
                $"for {target.Player.DisplayName}; no new spawn is submitted.");
            await CompleteRemoteSpawnAsync(target.Player, inFlight.PreviousKey, inFlight.SpawnKey, context)
                .ConfigureAwait(false);
            return;
        }
        var previousKey = initialRuntime.RemoteCharacterSpawnKey;
        var arguments = RemoteSpawnPlanner.Create(target.Position);
        LogSpawnPosition(target.Player, target.Position);
        using var commandLifetime = CancellationTokenSource.CreateLinkedTokenSource(_lifetime.Token);
        var submission = _controller.ExecuteCommandAsync(GameCommandNames.SpawnCharacter,
            arguments, commandLifetime.Token, waitForExecution: true);
        GameCommandResultPayload result;
        try
        {
            // Wait for the exact final response, including a queued command's
            // explicit rejection/expiry. Never infer acceptance from its ack.
            while (!submission.IsCompleted)
            {
                EnsureRemoteSpawnContext(context, _controller.Snapshot);
                await Task.Delay(25, _lifetime.Token).ConfigureAwait(false);
            }
            result = await submission.ConfigureAwait(false);
        }
        catch
        {
            commandLifetime.Cancel();
            // Observe completion of the canceled final-result waiter.
            try { await submission.ConfigureAwait(false); } catch { }
            throw;
        }
        EnsureSpawnAccepted(result);
        var expectedSpawnKey = TryParseSpawnKey(result.Message);
        // Record before any context check: from here on the engine owns a
        // Character for this peer, whatever happens to this observer.
        lock (_stateGate)
            _inFlightRemoteSpawns[target.Player.PeerId] = new InFlightRemoteSpawn(
                previousKey, expectedSpawnKey, context.SessionId, context.WorldGeneration,
                context.ProcessId, DateTimeOffset.UtcNow,context.NativeGeneration,context.Incarnation,context.ConnectionIdentity);
        EnsureRemoteSpawnContext(context, _controller.Snapshot);
        await CompleteRemoteSpawnAsync(target.Player, previousKey, expectedSpawnKey, context)
            .ConfigureAwait(false);
    }

    private async Task CompleteRemoteSpawnAsync(
        LobbyPlayer player, uint previousKey, uint? expectedSpawnKey, RemoteSpawnContext context)
    {
        NativeRuntimeSnapshot tracked;
        try
        {
            tracked = await WaitForTrackedSpawnAsync(previousKey, expectedSpawnKey, context)
                .ConfigureAwait(false);
        }
        catch (InvalidOperationException)
        {
            // The tracker reported a failed spawn: nothing is left to resume.
            lock (_stateGate) _inFlightRemoteSpawns.Remove(player.PeerId);
            throw;
        }
        StoreRemoteCharacter(player, tracked, context);
        lock (_stateGate) _inFlightRemoteSpawns.Remove(player.PeerId);
        await ShowPlayerJoinedMessageAsync(player.DisplayName).ConfigureAwait(false);
    }

    // Same session, launch generation and game process: the accepted spawn is
    // still the engine's. Too old (never became ready) or another context: drop it.
    private bool TryResumeInFlightSpawn(ulong peerId, RemoteSpawnContext context, out InFlightRemoteSpawn inFlight)
    {
        lock (_stateGate)
        {
            if (!_inFlightRemoteSpawns.TryGetValue(peerId, out inFlight)) return false;
            if(inFlight.SessionId==context.SessionId&&inFlight.StartGeneration==context.WorldGeneration
                &&inFlight.ProcessId==context.ProcessId&&inFlight.NativeGeneration==context.NativeGeneration
                &&inFlight.ConnectionIdentity==context.ConnectionIdentity&&inFlight.Incarnation!=context.Incarnation)
                throw new RemoteSpawnContextChangedException(); // Retain accepted old spawn; never relabel or duplicate it.
            if (inFlight.SessionId == context.SessionId && inFlight.StartGeneration == context.WorldGeneration
                && inFlight.ProcessId == context.ProcessId
                && inFlight.NativeGeneration==context.NativeGeneration&&inFlight.Incarnation==context.Incarnation
                && inFlight.ConnectionIdentity==context.ConnectionIdentity
                && DateTimeOffset.UtcNow - inFlight.SubmittedUtc < InFlightRemoteSpawnLifetime)
                return true;
            _inFlightRemoteSpawns.Remove(peerId);
            return false;
        }
    }

    private async Task<GameCommandResultPayload> ProvisionRemoteEquipmentManualAsync(
        IReadOnlyDictionary<string, string> arguments)
    {
        if (!arguments.TryGetValue("equipmentId", out var equipmentId)
            || !RemoteEquipmentDefinitions.TryGet(equipmentId, out var definition))
            throw new InvalidOperationException("Select a valid equipment entry before provisioning.");
        if (!definition.SelectorEnabled)
            throw new InvalidOperationException(
                $"'{definition.Label}' is not enabled for the external WEAPON_GiveToCharacter bridge. " +
                $"{definition.DisabledReason}");

        await _remoteEquipmentProvisionGate.WaitAsync(_lifetime.Token)
            .ConfigureAwait(false);
        try
        {
            List<(LobbyPlayer Player, uint CharacterBase)> targets;
            Task<RemoteEquipmentCatalogSet?>? preparation;
            lock (_stateGate)
            {
                if (_start is null || _lobby is null)
                    throw new InvalidOperationException(
                        "Start the multiplayer session before provisioning equipment.");

                var localPeerId = LocalPeerId();
                targets = _lobby.Players
                    .Where(player => player.PeerId != localPeerId)
                    .Select(player =>
                    {
                        _remoteCharacters.TryGetValue(player.PeerId, out var characterBase);
                        return (Player: player, CharacterBase: characterBase);
                    })
                    .Where(target => target.CharacterBase != 0U)
                    .ToList();
                preparation = _remoteEquipmentCatalogPreparation;
            }

            if (targets.Count == 0)
                throw new InvalidOperationException(
                    "No mapped remote Character is available. Spawn the remote Character first and wait until it is visibly initialized.");

            var runtime = _controller.Snapshot;
            if (!runtime.IpcConnected || !runtime.EngineThreadReady
                || runtime.LevelGeneration <= 0)
                throw new InvalidOperationException(
                    "Wait until the DLL and certified engine thread are ready before provisioning equipment.");

            if (preparation is null)
                throw new InvalidOperationException(
                    "The equipment catalog has not been prepared for the current level.");

            var catalogs = await preparation.ConfigureAwait(false);
            if (catalogs is null)
                throw new InvalidOperationException(
                    "The equipment catalog is unavailable for the current level.");
            if (!catalogs.TryGet(equipmentId, out var catalog))
            {
                catalogs.Unavailable.TryGetValue(equipmentId, out var reason);
                throw new InvalidOperationException(
                    $"'{definition.Label}' is not provisionable from this level's COMMANDS graph. {reason}");
            }

            _diagnostics.Log("PlayerSync",
                $"Manual remote equipment provisioning started: equipment='{definition.Label}', " +
                $"composite='{catalog.CompositeName}', targets={targets.Count}.");

            var succeeded = 0;
            var failed = new List<ulong>();
            foreach (var target in targets)
            {
                var provisioned = await ProvisionRemoteEquipmentAsync(
                    target.Player, target.CharacterBase, catalog).ConfigureAwait(false);
                if (provisioned)
                    ++succeeded;
                else
                    failed.Add(target.Player.PeerId);
            }

            var complete = failed.Count == 0;
            var status = complete
                ? "REMOTE_EQUIPMENT_MANUAL_COMPLETE"
                : "REMOTE_EQUIPMENT_MANUAL_PARTIAL";
            var failedText = failed.Count == 0
                ? "none"
                : string.Join(",", failed.Select(value =>
                    value.ToString(CultureInfo.InvariantCulture)));
            var message =
                $"Manual equipment provisioning finished: equipment='{definition.Label}' " +
                $"targets={targets.Count} succeeded={succeeded} failed={failed.Count} " +
                $"failedPeers={failedText}.";
            _diagnostics.Log("PlayerSync", message);

            return new GameCommandResultPayload(
                ProtocolConstants.ExperimentalSchemaVersion,
                Guid.NewGuid(),
                runtime.LevelGeneration,
                Guid.Empty,
                LauncherGameCommandNames.ProvisionRemoteEquipment,
                status,
                message,
                complete && succeeded > 0);
        }
        finally
        {
            _remoteEquipmentProvisionGate.Release();
        }
    }

    private async Task<bool> ProvisionRemoteEquipmentAsync(
        LobbyPlayer player,
        uint characterBase,
        RemoteEquipmentCatalog catalog,
        bool activateHeld = false, bool releaseHeld = false)
    {
        // Also protects manual/catalog calls, not only the automatic 32/33
        // PlayerSync path. Keep the legacy release operation available solely
        // to detach an already-attached flare; it never creates held equipment.
        if (!releaseHeld && RemoteEquipmentDefinitions.IsUnsafeRemoteHeldThrowable(catalog.Definition.Id))
        {
            _diagnostics.Log("PlayerSync",
                $"Remote PeerId {player.PeerId}: blocked unsafe held throwable '{catalog.Definition.Id}' " +
                "on proxy Character; ThrowableSync handles the released object.");
            return false;
        }
        var arguments = new Dictionary<string, string>(StringComparer.Ordinal)
        {
            ["characterAddress"] = $"0x{characterBase:X8}",
            ["compositeName"] = catalog.CompositeName,
            ["equipmentEntityShortGuid"] =
                $"0x{catalog.EquipmentEntityShortGuid:X8}",
            ["equipmentOutputShortGuid"] =
                $"0x{catalog.EquipmentOutputShortGuid:X8}",
            ["spawnSchedulerParameterShortGuid"] =
                $"0x{catalog.SpawnSchedulerParameterShortGuid:X8}",
            ["spawnTargetInputShortGuid"] =
                $"0x{catalog.SpawnTargetInputShortGuid:X8}",
            ["equipmentSlot"] =
                catalog.EquipmentSlot.ToString(CultureInfo.InvariantCulture)
        };
        var deadline = DateTimeOffset.UtcNow.AddSeconds(8);
        if (activateHeld) arguments["activateHeld"] = "true";
        if (releaseHeld) arguments["releaseHeld"] = "true";
        string lastStatus = string.Empty;
        string lastMessage = string.Empty;
        while (DateTimeOffset.UtcNow < deadline)
        {
            try
            {
                var result = await _controller.ExecuteReplicationCommandAsync(
                    GameCommandNames.RemoteEquipmentProvision,
                    arguments,
                    _lifetime.Token).ConfigureAwait(false);
                if (result.Executed)
                {
                    _diagnostics.Log("PlayerSync",
                        $"Remote equipment assigned through WEAPON_GiveToCharacter: " +
                        $"PeerId={player.PeerId} CharacterBase=0x{characterBase:X8} " +
                        $"equipment='{catalog.Definition.Label}' composite='{catalog.CompositeName}'.");
                    return true;
                }

                lastStatus = result.StatusCode;
                lastMessage = result.Message;
                if (!string.Equals(result.StatusCode,
                        "REMOTE_EQUIPMENT_NOT_READY", StringComparison.Ordinal))
                {
                    _diagnostics.Log("PlayerSync",
                        $"Remote equipment assignment failed for PeerId {player.PeerId} " +
                        $"('{catalog.Definition.Label}'): {result.StatusCode} - {result.Message}");
                    return false;
                }
            }
            catch (TimeoutException exception)
            {
                lastStatus = "COMMAND_TIMEOUT";
                lastMessage = exception.Message;
                _diagnostics.Log("PlayerSync",
                    $"Remote equipment command timed out for PeerId {player.PeerId}; " +
                    $"retrying observation of the same provisioning operation: {exception.Message}");
            }
            catch (Exception exception) when (
                exception is ProtocolException or IOException or InvalidOperationException)
            {
                _diagnostics.Log("PlayerSync",
                    $"Remote equipment assignment failed for PeerId {player.PeerId}: {exception.Message}");
                return false;
            }

            await Task.Delay(250, _lifetime.Token).ConfigureAwait(false);
        }

        _diagnostics.Log("PlayerSync",
            $"Remote equipment assignment timed out for PeerId {player.PeerId}: " +
            $"{lastStatus} - {lastMessage}");
        return false;
    }

    private async Task ShowPlayerJoinedMessageAsync(string playerName)
    {
        try
        {
            var arguments = RemoteSpawnPlanner.CreateJoinMessage(playerName);
            var result = await _controller.ExecuteCommandAsync(
                GameCommandNames.PopupMessage, arguments, _lifetime.Token)
                .ConfigureAwait(false);
            if (result.Executed || NativeRuntimeController.IsQueuedResult(result)) return;
            _diagnostics.Log("Multiplayer",
                $"Player join PopMessage was rejected: {result.StatusCode} - {result.Message}");
        }
        catch (Exception exception)
        {
            _diagnostics.Log("Multiplayer",
                $"Player join PopMessage failed without interrupting spawn: {exception.Message}");
        }
    }

    private static void EnsureSpawnAccepted(GameCommandResultPayload result)
    {
        if (result.Executed || NativeRuntimeController.IsQueuedResult(result)) return;
        throw new InvalidOperationException(
            $"SpawnCharacter was rejected: {result.StatusCode} - {result.Message}");
    }

    private async Task<NativeRuntimeSnapshot> WaitForTrackedSpawnAsync(
        uint previousKey,
        uint? expectedSpawnKey,
        RemoteSpawnContext context)
    {
        var nextNotice = DateTimeOffset.UtcNow.AddSeconds(15);
        while (true)
        {
            _lifetime.Token.ThrowIfCancellationRequested();
            var runtime = _controller.Snapshot;
            EnsureRemoteSpawnContext(context, runtime);
            // Pin the first new key even if an older DLL omitted it in its
            // response. An unrelated later tracker value must never be mapped.
            if (!expectedSpawnKey.HasValue && runtime.RemoteCharacterSpawnKey != previousKey)
                expectedSpawnKey = runtime.RemoteCharacterSpawnKey;
            var resolved = ResolveTrackedSpawn(previousKey, expectedSpawnKey, runtime);
            if (resolved.HasValue) return runtime;
            if (DateTimeOffset.UtcNow >= nextNotice)
            {
                _diagnostics.Log("Multiplayer",
                    $"Accepted Character spawnKey={expectedSpawnKey?.ToString() ?? "pending"} " +
                    "is still loading. Keeping this request; no duplicate spawn is submitted.");
                nextNotice = DateTimeOffset.UtcNow.AddSeconds(15);
            }
            await Task.Delay(25, _lifetime.Token).ConfigureAwait(false);
        }
    }

    private static uint? ResolveTrackedSpawn(
        uint previousKey,
        uint? expectedSpawnKey,
        NativeRuntimeSnapshot runtime)
    {
        if (expectedSpawnKey.HasValue)
        {
            if (runtime.RemoteCharacterSpawnKey != expectedSpawnKey.Value) return null;
        }
        else if (runtime.RemoteCharacterSpawnKey == previousKey)
        {
            return null;
        }

        if (runtime.RemoteCharacterState == RemoteCharacterStateNames.Failed)
            throw new InvalidOperationException("The Spawn Tracker reported a failed Character spawn.");
        return runtime.RemoteCharacterState == RemoteCharacterStateNames.Ready
            && runtime.RemoteCharacterBase != 0U
            ? runtime.RemoteCharacterBase
            : null;
    }

    private static uint? TryParseSpawnKey(string? message)
    {
        const string marker = "spawnKey=";
        if (string.IsNullOrEmpty(message)) return null;
        var markerIndex = message.IndexOf(marker, StringComparison.Ordinal);
        if (markerIndex < 0) return null;
        var start = markerIndex + marker.Length;
        var end = start;
        while (end < message.Length && message[end] >= '0' && message[end] <= '9') end++;
        if (end == start) return null;
        return uint.TryParse(message.AsSpan(start, end - start), NumberStyles.None,
            CultureInfo.InvariantCulture, out var key)
            ? key
            : null;
    }

    private void StoreRemoteCharacter(
        LobbyPlayer player, NativeRuntimeSnapshot tracked, RemoteSpawnContext context)
    {
        var characterBase=tracked.RemoteCharacterBase;
        lock (_stateGate)
        {
            var current=_controller.Snapshot;
            EnsureRemoteSpawnContext(context, current);
            if(tracked.RemoteCharacterCreationSerial!=0&&(current.RemoteCharacterCreationSerial!=tracked.RemoteCharacterCreationSerial
                ||current.RemoteCharacterBase!=characterBase||current.RemoteCharacterRuntimeId!=tracked.RemoteCharacterRuntimeId
                ||current.RemoteCharacterSpawnKey!=tracked.RemoteCharacterSpawnKey))throw new RemoteSpawnContextChangedException();
            if (_lobby?.Players.Any(value => value.PeerId == player.PeerId) != true)
                throw new RemoteSpawnContextChangedException();
            if (!_remoteCharacters.TryGetValue(player.PeerId, out var previousCharacter)
                || previousCharacter != characterBase)
            {
                _remoteProvisionedEquipment.Remove(player.PeerId);
                _remoteAppliedAim.Remove(player.PeerId);
                _remoteAppliedPrimary.Remove(player.PeerId);
            }
            _remoteCharacters[player.PeerId] = characterBase;
            _remoteSpawnRetryAttempts.Remove(player.PeerId);
            _remoteSpawnRetryNotBeforeUtc.Remove(player.PeerId);
            _aiSync.SetParticipant(player.PeerId, characterBase,AISyncParticipantEvidence.FromSpawn(context.Incarnation,
                characterBase,tracked.RemoteCharacterSpawnKey,tracked,context.ConnectionIdentity));
        }
        _diagnostics.Log("Multiplayer",
            $"Remote player {player.DisplayName} mapped to CharacterBase=0x{characterBase:X8}.");
        PlayerEquipmentSnapshotPayload? equipment;
        lock (_stateGate) _remoteEquipment.TryGetValue(player.PeerId, out equipment);
        if (equipment is not null)
        {
            _diagnostics.Log("PlayerSync",
                $"Remote PeerId {player.PeerId} state is ready for CharacterBase=0x{characterBase:X8}: " +
                $"equipment={equipment.CurrentEquipmentIndex}, previous={equipment.PreviousEquipmentIndex}, " +
                $"active={(equipment.EquipmentActive ? 1 : 0)}, aim={(equipment.AimHeld ? 1 : 0)}, " +
                $"primary={(equipment.PrimaryHeld ? 1 : 0)}.");
            _ = ConvergeRemotePlayerEquipmentAsync(player.PeerId);
        }
    }

    private void CompleteSpawnRequest(Guid requestId)
    {
        lock (_stateGate)
        {
            _processedSpawnRequests.Add(requestId);
            if (_pendingSpawnRequest?.RequestId == requestId) _pendingSpawnRequest = null;
        }
        _diagnostics.Log("Multiplayer", $"Remote-player spawn request {requestId:N} completed.");
        PublishState();
    }

    private void RegisterSpawnRequestFailure(Exception exception)
    {
        Guid? requestId;
        lock (_stateGate)
        {
            requestId = _pendingSpawnRequest?.RequestId;
            if (requestId.HasValue) _processedSpawnRequests.Add(requestId.Value);
            _pendingSpawnRequest = null;
        }
        _diagnostics.Log("Multiplayer",
            $"Remote-player spawn request failed: {exception.Message}");
    }

    private void LogSpawnPosition(
        LobbyPlayer player,
        PlayerSpawnPositionPayload position) =>
        _diagnostics.Log("Multiplayer",
            $"Spawning Character for {player.DisplayName} at that player's network position: " +
            $"X={position.X:0.000}, Y={position.Y:0.000}, Z={position.Z:0.000}.");

    internal async Task ResetForLauncherAsync(CancellationToken cancellationToken)
    {
        await _commandGate.WaitAsync(cancellationToken).ConfigureAwait(false);
        try { await StopNetworkAsync("Launcher reset").ConfigureAwait(false); }
        finally { _commandGate.Release(); }
    }

    private async Task StopNetworkAsync(string reason)
    {
        await ReleaseSessionLoadingBestEffortAsync().ConfigureAwait(false);
        var stopState = BeginNetworkStop();
        DetachNetwork(stopState.Host, stopState.Client);
        try
        {
            await UnbindNativeWorldSyncAuthorityAsync($"network-stop:{reason}").ConfigureAwait(false);
            await StopRemoteCharactersBestEffortAsync(stopState.RemoteCharacters).ConfigureAwait(false);
        }
        finally
        {
            lock (_stateGate) ClearSessionState();
            await DisposeNetworkAsync(stopState.Host, stopState.Client).ConfigureAwait(false);
        }
        _diagnostics.Log("Multiplayer", reason);
        PublishState();
        if (stopState.Host is not null || stopState.Client is not null)
        {
            await SendMenuSessionClosedAsync(reason).ConfigureAwait(false);
            await SendStatusAsync("Lobby disconnected").ConfigureAwait(false);
        }
    }

    private (LobbyHost? Host, LobbyClient? Client, uint[] RemoteCharacters) BeginNetworkStop()
    {
        lock (_stateGate)
        {
            ++_networkStopVersion;
            _localGameplayReady = false;
            var characters = _remoteCharacters.Values.Distinct().ToArray();
            ResetMissionSync();
            _start = null;
            return (_host, _client, characters);
        }
    }

    private void DetachNetwork(LobbyHost? host, LobbyClient? client)
    {
        if (host is not null) DetachHost(host);
        if (client is not null) DetachClient(client);
    }

    private async Task DisposeNetworkAsync(LobbyHost? host, LobbyClient? client)
    {
        if (host is not null) await host.DisposeAsync().ConfigureAwait(false);
        if (client is not null) await client.DisposeAsync().ConfigureAwait(false);
        if (host is not null || client is not null) await StopSteamRelayAsync().ConfigureAwait(false);
    }

    private async Task StopRemoteCharactersBestEffortAsync(IEnumerable<uint> characterBases)
    {
        foreach (var characterBase in characterBases.Distinct())
            await StopRemoteCharacterBestEffortAsync(characterBase).ConfigureAwait(false);
    }

    private async Task StopRemoteCharacterBestEffortAsync(uint characterBase)
    {
        try
        {
            var runtime = _controller.Snapshot;
            if (!runtime.IpcConnected || !runtime.EngineThreadReady) return;
            var result = await _controller.ExecuteReplicationCommandAsync(
                GameCommandNames.CharacterDirectMove, RemoteMovementCommandFactory.CreateStop(characterBase),
                CancellationToken.None).ConfigureAwait(false);
            EnsureMovementAccepted(result);
        }
        catch (Exception exception)
        {
            _diagnostics.Log("Replication",
                $"Best-effort stop failed for Character 0x{characterBase:X8}: {exception.Message}");
        }
    }

    private void ClearSessionState()
    {
        ResetMissionSync();
        ClearRoundLifecycleState();
        _preflightToken = Guid.Empty;
        _preflightTask = null;
        _preflightManifest = null;
        _loadedManifest = null;
        _host = null;
        _client = null;
        _lobby = null;
        _start = null;
        _uiGameplayLoaded = false;
        _localGameplayReady = false;
        _localReadySending = false;
        _remoteGameplayReady = false;
        ResetWorldReplicationState(preserveCommittedCheckpoint: true);
        ResetRemoteSpawnState();
        _localPositionSequence = 0;
        _gameplayReadiness.Reset();
    }

    private async Task SendConfigAsync()
    {
        var settings = _settingsStore.Load();
        await _bridge.SendAsync("CONFIG", BuildConfigValues(settings)).ConfigureAwait(false);
    }

    private IReadOnlyDictionary<string, string> BuildConfigValues(LauncherSettings settings) =>
        AddDeveloperLevels(settings, new Dictionary<string, string>(StringComparer.Ordinal)
        {
            ["mode"] = State.IsHost ? "Host" : "Client",
            ["playerName"] = settings.PlayerName,
            ["serverName"] = $"{settings.PlayerName}'s Lobby",
            ["serverPassword"] = string.Empty,
            ["hostIp"] = settings.HostAddress,
            ["port"] = settings.Port.ToString(CultureInfo.InvariantCulture),
            ["maxPlayers"] = ProtocolConstants.DefaultLobbyCapacity.ToString(CultureInfo.InvariantCulture),
            ["gameMode"] = "Survival",
            ["visibility"] = "Private",
            ["survivorMapId"] = "ChallengeMap16",
            ["survivorMapName"] = "ChallengeMap16"
        });

    private async Task SendCurrentLobbyAsync()
    {
        LobbySnapshotPayload? snapshot;
        lock (_stateGate) snapshot = _lobby;
        if (snapshot is not null) await SendLobbyStateAsync(snapshot).ConfigureAwait(false);
    }

    private Task SendLobbyStateAsync(LobbySnapshotPayload snapshot) =>
        _bridge.SendAsync("LOBBY_STATE", BuildLobbyValues(snapshot));

    private IReadOnlyDictionary<string, string> BuildLobbyValues(LobbySnapshotPayload snapshot)
    {
        var players = snapshot.Players.OrderByDescending(value => value.IsHost)
            .ThenBy(value => value.PeerId).ToArray();
        return WithMenuLobbyControls(snapshot, new Dictionary<string, string>(StringComparer.Ordinal)
        {
            ["role"] = State.IsHost ? "host" : "client",
            ["status"] = LobbyReadiness.Status(snapshot),
            ["mode"] = LevelGameMode(snapshot.Level),
            ["mapId"] = snapshot.Level,
            ["mapName"] = snapshot.Level,
            ["p1"] = FormatPlayer(players, 0, 1),
            ["p2"] = FormatPlayer(players, 1, 2),
            ["p3"] = FormatPlayer(players, 2, 3),
            ["p4"] = FormatPlayer(players, 3, 4)
        });
    }

    private static string FormatPlayer(
        IReadOnlyList<LobbyPlayer> players,
        int index,
        int slot)
    {
        if (index >= players.Count) return $"{slot}  Empty";
        var player = players[index];
        var role = player.IsHost ? "Owner" : "Player";
        var ready = player.Ready ? "Ready" : "Waiting";
        return $"{slot}  {role}  {player.DisplayName}  {ready}";
    }

    private Task SendStartGameAsync(LobbyStartPayload start)
    {
        var values = new Dictionary<string, string>(StringComparer.Ordinal)
        {
            ["sessionId"] = start.SessionId.ToString("N"),
            ["sessionGeneration"] = start.LevelGeneration.ToString(CultureInfo.InvariantCulture),
            ["lobbyId"] = start.SessionId.ToString("N"),
            ["syncProtocol"] = ProtocolConstants.Version.ToString(CultureInfo.InvariantCulture),
            ["mode"] = LevelGameMode(start.Level),
            ["mapId"] = start.Level,
            ["mapName"] = start.Level,
            ["engineLevelPath"] = @"Production\" + start.Level,
            ["hostId"] = ResolveHostId(),
            ["status"] = "starting"
        };
        return _bridge.SendAsync("START_GAME", values);
    }

    private string ResolveHostId()
    {
        lock (_stateGate)
        {
            var host = _lobby?.Players.FirstOrDefault(value => value.IsHost);
            return host?.LauncherPlayerId.ToString("N") ?? string.Empty;
        }
    }

    private Task SendStatusAsync(string message) => _bridge.SendAsync(
        "STATUS", new Dictionary<string, string> { ["message"] = message });

    private Task SendErrorAsync(string message) => _bridge.SendAsync(
        "ERROR", new Dictionary<string, string> { ["message"] = message });

    private Task LogOnlyAsync(string message)
    {
        _diagnostics.Log("Multiplayer", message);
        return Task.CompletedTask;
    }

    private async Task ReportBridgeFailureAsync(Exception exception)
    {
        _diagnostics.Log("Multiplayer", $"Command failed: {exception.Message}");
        await SendErrorAsync(exception.Message).ConfigureAwait(false);
    }

    private MultiplayerSessionView CreateView()
    {
        var role = _host is not null ? "Hosting" : _client is not null ? "Connected" : "Disconnected";
        var remote = _lobby?.Players.FirstOrDefault(value => !IsLocalPlayer(value));
        return new MultiplayerSessionView(role, _host is not null, _host is not null || _client is not null,
            _localGameplayReady, _remoteGameplayReady, remote?.DisplayName ?? string.Empty);
    }

    private ulong LocalPeerId() => _host?.HostPeerId ?? _client?.PeerId ?? 0;

    private bool IsLocalPlayer(LobbyPlayer player) => player.PeerId == LocalPeerId();

    private void PublishState() => StateChanged?.Invoke(this, State);
}

internal sealed record RemoteMovementTarget(
    ulong PeerId,
    uint CharacterBase,
    int LevelGeneration,
    RemotePositionTrackSnapshot Track);

internal static class LobbyCompatibility
{
    private const string ProfileId = "aimp-ws3-logicswitch-lifecycle-v33-root10-wm08c9";

    private const string EffectiveProfileId = ProfileId +
        ":ws3" +
        ":probe-callback-test" +
        ":aisync-v6-participants" +
        ":round-end-v1" +
        ":itemsync-v2";

    internal static CompatibilityInfo Create(LauncherSettings settings)
    {
        var profileId = EffectiveProfileId;
        return new CompatibilityInfo(
            ProtocolConstants.Version,
            ProductInfo.LauncherVersion,
            ProductInfo.ExpectedDllVersion,
            ProtocolConstants.HistoricalAiSha256,
            profileId,
            ComputeHash(profileId));
    }

    private static string ComputeHash(string profileId)
    {
        var text = $"{ProtocolConstants.Version}|{ProductInfo.LauncherVersion}|" +
            $"{ProductInfo.ExpectedDllVersion}|{profileId}";
        return Convert.ToHexString(SHA256.HashData(Encoding.UTF8.GetBytes(text)));
    }
}


internal sealed record RemotePlayerSpawnTarget(
    LobbyPlayer Player,
    PlayerSpawnPositionPayload Position);

internal static class RemoteSpawnPlanner
{
    internal static bool AllRemotePlayersReady(
        LobbySnapshotPayload? lobby,
        ulong localPeerId,
        IReadOnlySet<ulong> readyPeers)
    {
        var remotePeers = (lobby?.Players ?? Array.Empty<LobbyPlayer>())
            .Where(player => player.PeerId != localPeerId)
            .Select(player => player.PeerId)
            .ToArray();
        return remotePeers.Length > 0
            && remotePeers.All(readyPeers.Contains);
    }

    internal static IReadOnlyList<RemotePlayerSpawnTarget> SelectPendingPlayers(
        LobbySnapshotPayload? lobby,
        IReadOnlyList<PlayerSpawnPositionPayload>? positions,
        ulong localPeerId,
        IReadOnlyDictionary<ulong, uint> existing)
    {
        var byPeer = (positions ?? Array.Empty<PlayerSpawnPositionPayload>())
            .ToDictionary(value => value.PeerId);
        return (lobby?.Players ?? Array.Empty<LobbyPlayer>())
            .Where(player => player.PeerId != localPeerId)
            .Where(player => !existing.ContainsKey(player.PeerId))
            .Where(player => byPeer.ContainsKey(player.PeerId))
            .OrderBy(player => player.PeerId)
            .Select(player => new RemotePlayerSpawnTarget(player, byPeer[player.PeerId]))
            .ToArray();
    }

    internal static bool MatchesLobby(
        IReadOnlyList<PlayerSpawnPositionPayload> positions,
        IReadOnlyList<LobbyPlayer> players) =>
        positions.Select(value => value.PeerId).OrderBy(value => value)
            .SequenceEqual(players.Select(value => value.PeerId).OrderBy(value => value));

    internal static PlayerSpawnPositionPayload CreateNetworkPosition(
        ulong peerId,
        LocalPlayerPosition position) =>
        new(peerId, position.X, position.Y, position.Z);

    internal static PlayerSpawnPositionPayload CreateNetworkPosition(
        PlayerPositionSnapshotPayload position) =>
        new(position.PeerId, position.X, position.Y, position.Z);

    internal static IReadOnlyDictionary<string, string> Create(
        PlayerSpawnPositionPayload position) =>
        new Dictionary<string, string>(StringComparer.Ordinal)
        {
            ["x"] = position.X.ToString("0.000", CultureInfo.InvariantCulture),
            ["y"] = position.Y.ToString("0.000", CultureInfo.InvariantCulture),
            ["z"] = position.Z.ToString("0.000", CultureInfo.InvariantCulture),
            ["character"] = position.Character ?? PlayerCharacterCatalog.DefaultName
        };

    internal static IReadOnlyDictionary<string, string> CreateJoinMessage(
        string playerName) =>
        new Dictionary<string, string>(StringComparer.Ordinal)
        {
            ["headerText"] = "AIMP",
            ["mainText"] = $"{playerName} join the game",
            ["duration"] = "5.0",
            ["icon"] = PopupMessageIcons.Alert,
            ["soundEvent"] = PopupMessageSoundEvents.ObjectiveNew
        };

    internal static IReadOnlyDictionary<string, string> CreateLeftMessage(
        string playerName) =>
        new Dictionary<string, string>(StringComparer.Ordinal)
        {
            ["headerText"] = "AIMP",
            ["mainText"] = $"{playerName} left the game",
            ["duration"] = "5.0",
            ["icon"] = PopupMessageIcons.Alert,
            ["soundEvent"] = PopupMessageSoundEvents.ObjectiveNew
        };
}
