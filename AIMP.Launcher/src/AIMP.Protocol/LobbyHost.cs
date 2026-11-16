using System.Buffers.Binary;
using System.Collections.Concurrent;
using System.Net;
using System.Net.Sockets;
using System.Security.Cryptography;
using System.Threading.Channels;

namespace AIMP.Protocol;

public sealed record LobbyHostOptions(
    string ServerName,
    string HostDisplayName,
    Guid HostLauncherPlayerId,
    int Port,
    int Capacity,
    string Level,
    CompatibilityInfo Compatibility,
    bool OpaqueRelayEnabled = false,
    bool RequireWorldPreparation = false,
    bool AlienEnabled = true,
    bool AlienVsSurvivors = false,
    CampaignLaunchDescriptor? Campaign = null,
    bool AllowSoloStart = false);

public readonly record struct WorldHostAcceptedMutation(
    WorldNetworkMutation Mutation,
    WorldAuthorityContextIdentity AuthorityContext);

public sealed partial class LobbyHost : IAsyncDisposable
{
    private static readonly TimeSpan HeartbeatPeriod = TimeSpan.FromSeconds(2);
    private static readonly TimeSpan PeerTimeout = TimeSpan.FromSeconds(10);

    private readonly record struct WorldAuthorityBroadcastContext(
        Guid SessionId,
        int WorldGeneration,
        WorldManifestIdentity Manifest,
        string ManifestSha256,
        ulong Epoch);

    private sealed record QueuedWorldBroadcast(
        WorldNetworkMutation Mutation,
        WorldAuthorityBroadcastContext Context);

    private readonly LobbyHostOptions _options;
    private readonly TcpListener _listener;
    private readonly ConcurrentDictionary<ulong, ClientState> _clients = new();
    private readonly CancellationTokenSource _shutdown = new();
    private readonly object _stateLock = new();
    private readonly Dictionary<ulong, string> _characterByPeer = new();
    private readonly PlayerPositionReceiveWindow _hostPositionWindow = new();
    private readonly object _snapshotPublishLock = new();
    private readonly SemaphoreSlim _admissionGate = new(1, 1);
    private readonly SemaphoreSlim _snapshotBroadcastGate = new(1, 1);
    private readonly SemaphoreSlim _spawnPositionGate = new(1, 1);
    private readonly SemaphoreSlim _worldMutationGate = new(1, 1);
    private readonly WorldReplicationAuthority _worldAuthority;
    private readonly object _worldAuthorityBroadcastLock = new();
    private CancellationTokenSource _worldAuthorityBroadcastCancellation = new();
    private WorldAuthorityBroadcastContext? _worldAuthorityBroadcastContext;
    private readonly Channel<QueuedWorldBroadcast> _worldMutationBroadcasts =
        Channel.CreateBounded<QueuedWorldBroadcast>(new BoundedChannelOptions(512)
        {
            SingleReader = true,
            SingleWriter = false,
            FullMode = BoundedChannelFullMode.Wait,
            AllowSynchronousContinuations = false,
        });
    private readonly List<Task> _clientTasks = new();
    private Task? _acceptLoop;
    private Task? _heartbeatLoop;
    private Task? _worldMutationBroadcastLoop;
    private string _level;
    private string _state = "Lobby";
    private ulong _alienPeerId; // Host-owned lobby role, never assigned by a client packet.
    private int _levelGeneration;
    // Participants whose THE_PLAYER died during _levelGeneration. Guarded by
    // _stateLock and cleared whenever a generation starts or a round ends.
    private readonly HashSet<ulong> _deadPeers = new();
    // ItemSync: first collector of each pickup in the active generation.
    private readonly Dictionary<string, ulong> _collectedItems = new(StringComparer.Ordinal);
    // ItemSync spawn authority: first decision per owner key, in decision order.
    private readonly Dictionary<string, ItemSpawnPayload> _spawnDecisions = new(StringComparer.Ordinal);
    private readonly List<ItemSpawnPayload> _spawnDecisionOrder = new();
    private SpawnPositionRound? _activeSpawnRound;
    private int _disposed;

    public LobbyHost(LobbyHostOptions options)
    {
        ArgumentNullException.ThrowIfNull(options);
        if (!PlayerName.TryNormalize(options.HostDisplayName, out var normalized, out var error))
        {
            throw new ArgumentException(error, nameof(options));
        }

        if (string.IsNullOrWhiteSpace(options.ServerName)
            || options.ServerName.Length > ProtocolConstants.MaximumServerNameLength)
        {
            throw new ArgumentException("The server name is out of bounds.", nameof(options));
        }

        if (options.Port is < 0 or > 65535)
        {
            throw new ArgumentOutOfRangeException(nameof(options));
        }

        if (options.Capacity is < 1 or > 32)
        {
            throw new ArgumentOutOfRangeException(nameof(options));
        }

        if (options.HostLauncherPlayerId == Guid.Empty)
        {
            throw new ArgumentException("HostLauncherPlayerId is invalid.", nameof(options));
        }

        if (string.IsNullOrWhiteSpace(options.Level) || options.Level.Length > 128)
        {
            throw new ArgumentException("The level name is out of bounds.", nameof(options));
        }

        ModelValidation.Validate(options.Compatibility);
        if (options.Campaign is { } campaignLaunch) CampaignSaveValidation.Validate(campaignLaunch);
        _campaignLaunch = options.Campaign;
        _options = options with { HostDisplayName = normalized };
        _level = options.Level;
        SessionId = Guid.NewGuid();
        HostPeerId = NewPeerId();
        _worldAuthority = new WorldReplicationAuthority(SessionId, HostPeerId);
        _listener = new TcpListener(IPAddress.Any, options.Port);
    }

    public Guid SessionId { get; }

    public ulong HostPeerId { get; }

    public int BoundPort => (_listener.LocalEndpoint as IPEndPoint)?.Port ?? _options.Port;

    // A one-slot lobby, or one that allows it (Private), may start without any client.
    public bool AllowsSoloStart => _options.Capacity == 1 || _options.AllowSoloStart;

    // Solo: nobody else is in the lobby, so the round runs without remote players.
    public bool IsSolo => AllowsSoloStart && _clients.IsEmpty;

    private int MinimumClientsToStart => AllowsSoloStart ? 0 : 1;

    public bool CanStart
    {
        get
        {
            lock (_stateLock)
                return string.Equals(_state, "Lobby", StringComparison.Ordinal)
                    && _clients.Count >= MinimumClientsToStart
                    && _clients.Values.All(client => client.IsReady)
                    && (!_options.AlienVsSurvivors || (_alienPeerId != 0 &&
                        (_alienPeerId == HostPeerId || _clients.ContainsKey(_alienPeerId))))
                    && (!_options.RequireWorldPreparation || AllLobbyPreparationsMatchLocked());
        }
    }

    // Only the local host API may choose the unique Alien. There is deliberately no
    // corresponding client-to-host message, preventing role spoofing.
    public async Task SetAlienPlayerAsync(ulong peerId, CancellationToken cancellationToken = default)
    {
        if (!_options.AlienVsSurvivors)
            throw new InvalidOperationException("The lobby is not an Alien vs Survivors game.");
        await _admissionGate.WaitAsync(cancellationToken).ConfigureAwait(false);
        try
        {
            lock (_stateLock)
            {
                if (_state != "Lobby") throw new InvalidOperationException("Roles are locked after start.");
                if (peerId != 0 && peerId != HostPeerId && !_clients.ContainsKey(peerId))
                    throw new InvalidOperationException("Selected Alien is not in the lobby.");
                _alienPeerId = peerId;
            }
        }
        finally { _admissionGate.Release(); }
        PublishSnapshot();
        await BroadcastSnapshotAsync(cancellationToken).ConfigureAwait(false);
    }

    public event EventHandler<LobbySnapshotPayload>? SnapshotChanged;

    public event EventHandler<string>? Log;

    public event EventHandler<LobbyStartPayload>? Started;

    public event EventHandler<GameplayReadyPayload>? GameplayReadyReceived;

    public event EventHandler<PlayerDeathPayload>? PlayerDeathReceived;

    public event EventHandler<RoundEndedPayload>? RoundEnded;

    public event EventHandler<ItemCollectedPayload>? ItemCollected;

    public event EventHandler<ItemSpawnPayload>? ItemSpawnDecided;

    public event EventHandler<SpawnRemotePlayersPayload>? SpawnRemotePlayersRequested;

    public event EventHandler<PlayerPositionSnapshotPayload>? PlayerPositionReceived;

    public event EventHandler<PlayerEquipmentSnapshotPayload>? PlayerEquipmentReceived;

    public event EventHandler<PlayerActionEventPayload>? PlayerActionReceived;
    // Host emits a single authoritative skip edge per held-Enter press.
    public Task PublishHostCinematicSkipAsync(CinematicSkipPayload payload,
        CancellationToken cancellationToken = default)
    {
        ModelValidation.Validate(payload);
        lock (_stateLock)
            if (payload.SessionId != SessionId || payload.LevelGeneration != _levelGeneration)
                throw new InvalidOperationException("Cinematic skip belongs to another lobby generation.");
        return BroadcastAsync(MessageType.CinematicSkipCommitted,
            JsonPayload.Serialize(payload),cancellationToken);
    }
    public event EventHandler<ReadOnlyMemory<byte>>? AISyncImpactReceived;

    public event EventHandler<WorldNetworkContext>? WorldContextReceived;

    public event EventHandler<WorldHostAcceptedMutation>? WorldMutationAccepted;

    public event EventHandler<WorldNetworkIntent>? WorldIntentRejected;

    public event EventHandler<WorldNetworkMutationAck>? WorldMutationAckReceived;

    public void Start()
    {
        lock (_stateLock)
        {
            if (_campaignLaunch?.Action == "continue" && _campaignSaveBundle is null)
                throw new InvalidOperationException("The selected campaign save bundle must be frozen before lobby startup.");
            if (_acceptLoop is not null)
            {
                throw new InvalidOperationException("The lobby is already running.");
            }

            _listener.Start(_options.Capacity);
            _acceptLoop = AcceptLoopAsync(_shutdown.Token);
            _heartbeatLoop = HeartbeatLoopAsync(_shutdown.Token);
            _worldMutationBroadcastLoop = WorldMutationBroadcastLoopAsync(_shutdown.Token);
            _missionInboundLoop = RunMissionInboundAsync();
        }
        PublishSnapshot();
    }

    public void SetHostReady(bool ready)
    {
        if (!ready) Log?.Invoke(this, "The lobby host is always ready.");
    }

    public void SetLevel(string level)
    {
        ArgumentNullException.ThrowIfNull(level);
        if (string.IsNullOrWhiteSpace(level) || level.Length > 128)
        {
            throw new ArgumentOutOfRangeException(nameof(level));
        }

        lock (_stateLock)
        {
            if (!string.Equals(_state, "Lobby", StringComparison.Ordinal))
                throw new InvalidOperationException("The level cannot change after launch.");
            if (!string.Equals(_level, level, StringComparison.OrdinalIgnoreCase)) {
                _preparationToken = Guid.NewGuid();
                _lobbyPreparations.Clear();
            }
            _level = level;
        }

        PublishSnapshot();
        _ = BroadcastSnapshotSafelyAsync();
    }

    public DiscoveryAnnouncement CreateAnnouncement()
    {
        var snapshot = CreateSnapshot();
        var announcement = new DiscoveryAnnouncement(
            ProtocolConstants.Version,
            SessionId,
            _options.ServerName,
            _options.HostDisplayName,
            snapshot.Players.Count,
            snapshot.Capacity,
            snapshot.Level,
            snapshot.State,
            BoundPort,
            _options.Compatibility.CompatibilityHash);
        ModelValidation.Validate(announcement);
        return announcement;
    }

    public async Task<LobbyStartPayload> StartGameAsync(
        DateTimeOffset startAtUtc,
        CancellationToken cancellationToken = default)
    {
        LobbyStartPayload payload;
        await _admissionGate.WaitAsync(cancellationToken).ConfigureAwait(false);
        try
        {
            lock (_stateLock)
            {
                if (!string.Equals(_state, "Lobby", StringComparison.Ordinal))
                {
                    throw new InvalidOperationException(
                        "The session has already left the Lobby state.");
                }

                if (_clients.Count < MinimumClientsToStart
                    || _clients.Values.Any(client => !client.IsReady))
                {
                    throw new InvalidOperationException(
                        "At least one connected client is required and every client must be ready.");
                }

                if (_options.AlienVsSurvivors && (_alienPeerId == 0 ||
                    (_alienPeerId != HostPeerId && !_clients.ContainsKey(_alienPeerId))))
                    throw new InvalidOperationException("The host must assign exactly one Alien before launch.");
                if (_options.RequireWorldPreparation && !AllLobbyPreparationsMatchLocked())
                    throw new InvalidOperationException(
                        "Every participant must prepare the selected level with the same WorldManifest before launch.");
                _state = "Starting";
                _levelGeneration++;
                ResetMissionCatalogLocked();
                _sessionPreparedPeers.Clear();
                _sessionReleasedGeneration = 0;
                _deadPeers.Clear();
                _finishedPeers.Clear();
                _collectedItems.Clear();
                ClearSpawnDecisionsLocked();
                payload = new LobbyStartPayload(
                    SessionId,
                    startAtUtc.ToUnixTimeMilliseconds(),
                    _level,
                    _levelGeneration,
                    _options.AlienEnabled,
                    _options.AlienVsSurvivors,
                    _alienPeerId,
                    Campaign: _campaignLaunch);
            }
        }
        finally
        {
            _admissionGate.Release();
        }

        ModelValidation.Validate(payload);
        await BroadcastAsync(MessageType.LobbyStart, JsonPayload.Serialize(payload),
            cancellationToken).ConfigureAwait(false);

        // LevelGeneration is incremented above, so clients must receive a fresh
        // LobbySnapshot before generation-bound gameplay messages can be
        // validated. The host-side SnapshotChanged event alone does not update
        // remote clients to the new generation.
        await BroadcastSnapshotAsync(cancellationToken).ConfigureAwait(false);
        PublishSnapshot();
        Started?.Invoke(this, payload);
        return payload;
    }

    public async Task<GameplayReadyPayload> SetHostGameplayReadyAsync(
        int levelGeneration,
        CancellationToken cancellationToken = default)
    {
        var payload = CreateGameplayReady(HostPeerId, levelGeneration);
        await BroadcastAsync(MessageType.GameplayReady, JsonPayload.Serialize(payload),
            cancellationToken).ConfigureAwait(false);
        GameplayReadyReceived?.Invoke(this, payload);
        return payload;
    }

    public const int RoundReturnDelayMilliseconds = 5000;

    public Task ReportHostPlayerDeathAsync(
        int levelGeneration,
        CancellationToken cancellationToken = default)
    {
        var payload = new PlayerDeathPayload(SessionId, HostPeerId, levelGeneration);
        ModelValidation.Validate(payload);
        return RecordPlayerDeathAsync(payload, cancellationToken);
    }

    private async Task RecordPlayerDeathAsync(
        PlayerDeathPayload payload,
        CancellationToken cancellationToken)
    {
        RoundEndedPayload? ended;
        RoundProgressPayload? progress;
        lock (_stateLock)
        {
            // A finished participant left the level alive; a later local
            // death edge (outro, cleanup) must not reclassify it.
            if (!string.Equals(_state, "Starting", StringComparison.Ordinal)
                || payload.LevelGeneration != _levelGeneration
                || _finishedPeers.Contains(payload.PeerId)
                // The Alien's hidden technical THE_PLAYER is not the Alien NPC.
                || (_options.AlienVsSurvivors && payload.PeerId == _alienPeerId)
                || !_deadPeers.Add(payload.PeerId)) return;
            progress = CreateRoundProgressLocked();
            ended = TryEndRoundLocked();
        }

        Log?.Invoke(this, $"Participant {payload.PeerId} died in generation {payload.LevelGeneration}.");
        PlayerDeathReceived?.Invoke(this, payload);
        await PublishRoundOutcomeAsync(progress, ended, cancellationToken).ConfigureAwait(false);
    }

    // A departed participant can no longer survive: re-evaluate the round so
    // the remaining dead players are not stranded in a finished level.
    private async Task EvaluateRoundAfterDepartureAsync()
    {
        RoundEndedPayload? ended;
        RoundProgressPayload? progress;
        lock (_stateLock)
        {
            if (!string.Equals(_state, "Starting", StringComparison.Ordinal)) return;
            progress = CreateRoundProgressLocked();
            ended = TryEndRoundLocked();
        }

        try { await PublishRoundOutcomeAsync(progress, ended, _shutdown.Token).ConfigureAwait(false); }
        catch (OperationCanceledException) when (_shutdown.IsCancellationRequested) { }
    }

    // Caller holds _stateLock. Ends the round only when the Host and every
    // connected client of the active generation is terminal: dead or finished.
    private RoundEndedPayload? TryEndRoundLocked()
    {
        string reason;
        if (_options.AlienVsSurvivors)
        {
            // In PvP the Alien's hidden retail player is not a survivor.
            // All survivor deaths -> Alien victory. Any survivor escape ->
            // survivor victory. A departed client Alien forfeits the round.
            var survivors = _clients.Keys.Append(HostPeerId)
                .Where(peer => peer != _alienPeerId).ToArray();
            var alienLeft = _alienPeerId != 0 && _alienPeerId != HostPeerId && !_clients.ContainsKey(_alienPeerId);
            var survivorsEscape = survivors.Any(_finishedPeers.Contains);
            // If the last survivor leaves an active round, do not keep the
            // Alien alone in a round that can never finish. Alien departure
            // still has priority below, including simultaneous disconnects.
            var survivorsAllDead = survivors.All(_deadPeers.Contains);
            if (!alienLeft && !survivorsEscape && !survivorsAllDead) return null;
            reason = survivorsEscape || alienLeft ? "SurvivorsWin" : "AlienWin";
        }
        else
        {
            if (!RoundTerminalLocked(HostPeerId)
                || _clients.Keys.Any(peerId => !RoundTerminalLocked(peerId))) return null;
            reason = RoundEndReasonLocked();
        }
        _state = "Lobby";
        _sessionPreparedPeers.Clear();
        _sessionReleasedGeneration = 0;
        _deadPeers.Clear();
        _finishedPeers.Clear();
        _collectedItems.Clear();
        ClearSpawnDecisionsLocked();
        if (_options.AlienVsSurvivors && _alienPeerId != HostPeerId && !_clients.ContainsKey(_alienPeerId))
            _alienPeerId = 0;
        var payload = new RoundEndedPayload(SessionId, _levelGeneration,
            reason, RoundReturnDelayMilliseconds);
        ModelValidation.Validate(payload);
        return payload;
    }

    private async Task PublishRoundEndedAsync(
        RoundEndedPayload payload,
        CancellationToken cancellationToken)
    {
        await RevokeAlienControlAsync(cancellationToken).ConfigureAwait(false);
        await StopAISyncShadowAsync().ConfigureAwait(false);
        await StopAISyncExecutionAsync().ConfigureAwait(false);
        Log?.Invoke(this, $"Round {payload.LevelGeneration} ended: {payload.Reason}.");
        await BroadcastAsync(MessageType.RoundEnded, JsonPayload.Serialize(payload),
            cancellationToken).ConfigureAwait(false);
        await BroadcastSnapshotAsync(cancellationToken).ConfigureAwait(false);
        PublishSnapshot();
        RoundEnded?.Invoke(this, payload);
    }

    public Task ReportHostItemPickupAsync(
        int levelGeneration,
        string entityId,
        CancellationToken cancellationToken = default)
    {
        var payload = new ItemPickupRequestPayload(SessionId, HostPeerId, levelGeneration, entityId);
        ModelValidation.Validate(payload);
        return RecordItemPickupAsync(payload, cancellationToken);
    }

    // First collector wins. A later request for the same pickup is dropped:
    // its sender already holds the item locally and the pickup was removed
    // everywhere else by the first ItemCollected broadcast.
    private async Task RecordItemPickupAsync(
        ItemPickupRequestPayload payload,
        CancellationToken cancellationToken)
    {
        ItemCollectedPayload collected;
        var entityId = payload.EntityId.ToUpperInvariant();
        lock (_stateLock)
        {
            if (!string.Equals(_state, "Starting", StringComparison.Ordinal)
                || payload.LevelGeneration != _levelGeneration) return;
            if (!_collectedItems.TryAdd(entityId, payload.PeerId))
            {
                if (_collectedItems[entityId] != payload.PeerId)
                    Log?.Invoke(this, $"ItemSync: participant {payload.PeerId} also collected {entityId}; " +
                        $"participant {_collectedItems[entityId]} was first.");
                return;
            }

            collected = new ItemCollectedPayload(SessionId, payload.LevelGeneration,
                entityId, payload.PeerId);
        }

        ModelValidation.Validate(collected);
        Log?.Invoke(this, $"ItemSync: participant {payload.PeerId} collected {entityId}.");
        await BroadcastAsync(MessageType.ItemCollected, JsonPayload.Serialize(collected),
            cancellationToken).ConfigureAwait(false);
        ItemCollected?.Invoke(this, collected);
    }

    public Task ReportHostItemSpawnAsync(
        int levelGeneration,
        NativeItemSyncSpawnPayload value,
        CancellationToken cancellationToken = default)
    {
        var payload = new ItemSpawnPayload(SessionId, HostPeerId, levelGeneration, value.Kind,
            value.OwnerKey, value.Spawn, value.Path, value.Quantity, value.Transform);
        ModelValidation.Validate(payload);
        return RecordItemSpawnAsync(payload, cancellationToken);
    }

    // Caller holds _stateLock.
    private void ClearSpawnDecisionsLocked()
    {
        _spawnDecisions.Clear();
        _spawnDecisionOrder.Clear();
    }

    // The first proposal per owner key becomes the decision for every peer,
    // including "nothing spawns here". Drops have unique synthetic owners.
    private async Task RecordItemSpawnAsync(
        ItemSpawnPayload payload,
        CancellationToken cancellationToken)
    {
        ItemSpawnPayload decision;
        lock (_stateLock)
        {
            if (!string.Equals(_state, "Starting", StringComparison.Ordinal)
                || payload.LevelGeneration != _levelGeneration) return;
            decision = payload with { OwnerKey = payload.OwnerKey.ToUpperInvariant() };
            if (!_spawnDecisions.TryAdd(decision.OwnerKey, decision)) return;
            _spawnDecisionOrder.Add(decision);
        }

        Log?.Invoke(this, decision.Spawn
            ? $"ItemSync: {decision.Kind} {decision.OwnerKey} spawns {decision.Path} x{decision.Quantity} " +
              $"(participant {decision.PeerId})."
            : $"ItemSync: owner {decision.OwnerKey} spawns nothing (participant {decision.PeerId}).");
        await BroadcastAsync(MessageType.ItemSpawnDecision, JsonPayload.Serialize(decision),
            cancellationToken).ConfigureAwait(false);
        ItemSpawnDecided?.Invoke(this, decision);
    }

    // A client that reaches gameplay after pickups were already collected
    // receives the full set; the native side applies each id once loaded.
    private async Task ReplayCollectedItemsAsync(
        ClientState state,
        int levelGeneration,
        CancellationToken cancellationToken)
    {
        ItemCollectedPayload[] replay;
        ItemSpawnPayload[] decisions;
        lock (_stateLock)
        {
            if (!string.Equals(_state, "Starting", StringComparison.Ordinal)
                || levelGeneration != _levelGeneration) return;
            // Spawn decisions first: a collected pickup whose decision arrives
            // later is then never spawned (the native removal stays pending).
            decisions = _spawnDecisionOrder.ToArray();
            replay = _collectedItems
                .Where(item => item.Value != state.PeerId)
                .Select(item => new ItemCollectedPayload(SessionId, levelGeneration, item.Key, item.Value))
                .ToArray();
        }

        foreach (var decision in decisions)
            await SendToClientSafelyAsync(state, MessageType.ItemSpawnDecision,
                JsonPayload.Serialize(decision), cancellationToken).ConfigureAwait(false);
        foreach (var collected in replay)
            await SendToClientSafelyAsync(state, MessageType.ItemCollected,
                JsonPayload.Serialize(collected), cancellationToken).ConfigureAwait(false);
    }

    public async Task<SpawnRemotePlayersPayload> RequestRemotePlayerSpawnsAsync(
        int levelGeneration,
        PlayerSpawnPositionPayload hostPosition,
        CancellationToken cancellationToken = default)
    {
        await _spawnPositionGate.WaitAsync(cancellationToken).ConfigureAwait(false);
        try { return await CollectAndBroadcastSpawnPositionsAsync(
            levelGeneration, hostPosition, cancellationToken).ConfigureAwait(false); }
        finally { ClearActiveSpawnRound(); _spawnPositionGate.Release(); }
    }

    public async Task PublishHostPlayerPositionAsync(
        PlayerPositionSnapshotPayload payload,
        CancellationToken cancellationToken = default)
    {
        ValidatePlayerPosition(payload, HostPeerId);
        lock (_stateLock)
            if (!_hostPositionWindow.TryAccept(payload))
                throw new ProtocolException("The host player-position snapshot is stale.");
        await BroadcastAsync(MessageType.PlayerPositionSnapshot, JsonPayload.Serialize(payload),
            cancellationToken).ConfigureAwait(false);
        PlayerPositionReceived?.Invoke(this, payload);
    }

    public async Task PublishHostPlayerEquipmentAsync(
        PlayerEquipmentSnapshotPayload payload,
        CancellationToken cancellationToken = default)
    {
        ValidatePlayerEquipment(payload, HostPeerId);
        await BroadcastAsync(MessageType.PlayerEquipmentSnapshot, JsonPayload.Serialize(payload),
            cancellationToken).ConfigureAwait(false);
        PlayerEquipmentReceived?.Invoke(this, payload);
    }

    public async Task PublishHostPlayerActionAsync(
        PlayerActionEventPayload payload,
        CancellationToken cancellationToken = default)
    {
        ValidatePlayerAction(payload, HostPeerId);
        await BroadcastAsync(MessageType.PlayerActionEvent, JsonPayload.Serialize(payload),
            cancellationToken).ConfigureAwait(false);
        PlayerActionReceived?.Invoke(this, payload);
    }

    public async Task SetHostWorldContextAsync(
        WorldNetworkContext context,
        CancellationToken cancellationToken = default)
    {
        WorldAuthorityHealth authorityHealth;
        // Context replacement and HostAccepted/enqueue are serialized by the same
        // gate. This prevents an intent accepted under SHA A from capturing the
        // freshly-created broadcast context for SHA B before it is queued.
        await _worldMutationGate.WaitAsync(cancellationToken).ConfigureAwait(false);
        try
        {
            _worldAuthority.SetHostContext(context);
            if (!_worldAuthority.TryGetAuthorityContextIdentity(out var authorityContext))
                throw new ProtocolException("Host authority context identity is unavailable after WorldContext installation.");
            ResetWorldAuthorityBroadcastContextIfChanged(context, authorityContext);
            authorityHealth = _worldAuthority.AuthorityHealth;
        }
        finally
        {
            _worldMutationGate.Release();
        }

        Log?.Invoke(this, authorityHealth.Healthy
            ? $"WS3 Host authority healthy: generation={authorityHealth.WorldGeneration} epoch={authorityHealth.AuthorityContextEpoch} lastHostGameplayCommitted={authorityHealth.LastHostGameplayCommittedSequence}."
            : $"WS3 Host authority remains fail-stopped: generation={authorityHealth.WorldGeneration} failedTransaction={authorityHealth.FailedAuthorityTransaction}.");
        await BroadcastAsync(
            MessageType.WorldContext,
            WorldReplicationCodec.EncodeNetworkContext(context),
            cancellationToken).ConfigureAwait(false);
        WorldContextReceived?.Invoke(this, context);
    }

    public bool IsCurrentWorldAuthorityContext(WorldAuthorityContextIdentity authorityContext) =>
        _worldAuthority.IsCurrentAuthorityContext(authorityContext);

    private long _hostCommitLogTick;
    private long _hostCommitCount;

    // One summary per second: busy maps commit hundreds of transactions per second.
    private void LogHostGameplayCommitted(WorldAuthorityContextIdentity authorityContext, ulong endSequence)
    {
        var count = Interlocked.Increment(ref _hostCommitCount);
        var now = Environment.TickCount64;
        var last = Interlocked.Read(ref _hostCommitLogTick);
        if (now - last < 1_000 || Interlocked.CompareExchange(ref _hostCommitLogTick, now, last) != last) return;
        Log?.Invoke(this, $"WS3 HostGameplayCommitted: generation={authorityContext.WorldGeneration} " +
            $"epoch={authorityContext.Epoch} committed={count} through={endSequence}.");
    }

    public bool ReportWorldGameplayCommitted(
        WorldAuthorityContextIdentity authorityContext,
        ulong startSequence,
        ulong endSequence,
        out string rejection)
    {
        if (_worldAuthority.TryCommitHostGameplayTransaction(
                authorityContext, startSequence, endSequence, out rejection))
        {
            LogHostGameplayCommitted(authorityContext, endSequence);
            return true;
        }

        if (string.Equals(rejection, "HOST_GAMEPLAY_COMMIT_CONTEXT_STALE", StringComparison.Ordinal))
        {
            Log?.Invoke(this,
                $"WS3 stale HostGameplayCommitted ignored: generation={authorityContext.WorldGeneration} " +
                $"epoch={authorityContext.Epoch} transaction={startSequence}-{endSequence}.");
            return false;
        }

        MarkWorldAuthorityUnhealthy(
            authorityContext, startSequence,
            $"Host gameplay commit bookkeeping rejected transaction {startSequence}-{endSequence}: {rejection}");
        return false;
    }

    // Returns false only when the failure belongs to a superseded authority
    // context. Current-context failures (including an already unhealthy context)
    // remain terminal for the local Coordinator.
    public bool MarkWorldAuthorityUnhealthy(
        WorldAuthorityContextIdentity authorityContext,
        ulong failedAuthorityTransaction,
        string reason)
    {
        if (!_worldAuthority.TryFailStopAuthority(
                authorityContext, failedAuthorityTransaction, reason, out var health))
        {
            if (!_worldAuthority.IsCurrentAuthorityContext(authorityContext))
            {
                Log?.Invoke(this,
                    $"WS3 stale Host authority fail-stop ignored: generation={authorityContext.WorldGeneration} " +
                    $"epoch={authorityContext.Epoch} transaction={failedAuthorityTransaction}.");
                return false;
            }
            return !health.Healthy;
        }

        if (!TryCancelWorldAuthorityBroadcastContext(health, out var failedBroadcastContext))
        {
            Log?.Invoke(this,
                $"WS3 Host authority fail-stop belonged to a superseded broadcast context: " +
                $"generation={health.WorldGeneration} epoch={health.AuthorityContextEpoch} " +
                $"sha256={health.ManifestSha256}.");
            return false;
        }

        Log?.Invoke(this,
            $"WS3 HOST AUTHORITY FAIL-STOP: generation={health.WorldGeneration} " +
            $"epoch={health.AuthorityContextEpoch} failedTransaction={health.FailedAuthorityTransaction} " +
            $"lastHostGameplayCommitted={health.LastHostGameplayCommittedSequence} " +
            $"latestAccepted={health.LatestAcceptedSequence}. {health.FailureReason}");
        _ = NotifyWorldAuthorityUnhealthySafelyAsync(health, failedBroadcastContext);
        return true;
    }

    private static bool SameWorldAuthorityBroadcastIdentity(
        WorldAuthorityBroadcastContext current,
        WorldNetworkContext context) =>
        current.SessionId == context.SessionId
        && current.WorldGeneration == context.WorldGeneration
        && current.Manifest == context.Manifest
        && string.Equals(current.ManifestSha256, context.ManifestSha256,
            StringComparison.OrdinalIgnoreCase);

    private static bool SameWorldAuthorityBroadcastContext(
        WorldAuthorityBroadcastContext left,
        WorldAuthorityBroadcastContext right) =>
        left.Epoch == right.Epoch
        && left.SessionId == right.SessionId
        && left.WorldGeneration == right.WorldGeneration
        && left.Manifest == right.Manifest
        && string.Equals(left.ManifestSha256, right.ManifestSha256,
            StringComparison.OrdinalIgnoreCase);

    private void ResetWorldAuthorityBroadcastContextIfChanged(
        WorldNetworkContext context,
        WorldAuthorityContextIdentity authorityContext)
    {
        if (!authorityContext.Matches(context))
            throw new ProtocolException("Host authority/broadcast context identity mismatch.");
        lock (_worldAuthorityBroadcastLock)
        {
            if (_worldAuthorityBroadcastContext is { } current
                && SameWorldAuthorityBroadcastIdentity(current, context)
                && current.Epoch == authorityContext.Epoch)
                return;

            var previous = _worldAuthorityBroadcastCancellation;
            _worldAuthorityBroadcastCancellation = new CancellationTokenSource();
            _worldAuthorityBroadcastContext = new WorldAuthorityBroadcastContext(
                context.SessionId,
                context.WorldGeneration,
                context.Manifest,
                context.ManifestSha256,
                authorityContext.Epoch);
            try { previous.Cancel(); } catch (ObjectDisposedException) { }
            previous.Dispose();
        }
    }

    private bool TryCancelWorldAuthorityBroadcastContext(
        WorldAuthorityHealth health,
        out WorldAuthorityBroadcastContext failedContext)
    {
        lock (_worldAuthorityBroadcastLock)
        {
            if (_worldAuthorityBroadcastContext is not { } current
                || current.WorldGeneration != health.WorldGeneration
                || current.Manifest != health.Manifest
                || !string.Equals(current.ManifestSha256, health.ManifestSha256,
                    StringComparison.OrdinalIgnoreCase)
                || current.Epoch != health.AuthorityContextEpoch)
            {
                failedContext = default;
                return false;
            }
            failedContext = current;
            try { _worldAuthorityBroadcastCancellation.Cancel(); }
            catch (ObjectDisposedException) { }
            return true;
        }
    }

    private bool TryCaptureWorldAuthorityBroadcast(
        WorldNetworkMutation mutation,
        out QueuedWorldBroadcast queued)
    {
        lock (_worldAuthorityBroadcastLock)
        {
            if (_worldAuthorityBroadcastContext is not { } current
                || _worldAuthorityBroadcastCancellation.IsCancellationRequested
                || mutation.SessionId != current.SessionId
                || mutation.WorldGeneration != current.WorldGeneration
                || mutation.Mutation.Manifest != current.Manifest)
            {
                queued = null!;
                return false;
            }

            // ManifestSha256 is intentionally captured from the authority context,
            // not reconstructed from WorldNetworkMutation (which does not carry it).
            // SetHostWorldContextAsync shares _worldMutationGate with admission, so
            // this capture cannot cross a SHA-changing Host context reset.
            queued = new QueuedWorldBroadcast(mutation, current);
            return true;
        }
    }

    private bool IsSameWorldAuthorityBroadcastContext(
        WorldAuthorityBroadcastContext expected)
    {
        lock (_worldAuthorityBroadcastLock)
            return _worldAuthorityBroadcastContext is { } current
                && SameWorldAuthorityBroadcastContext(current, expected);
    }

    private bool IsCurrentWorldAuthorityBroadcastContext(
        WorldAuthorityBroadcastContext queuedContext)
    {
        lock (_worldAuthorityBroadcastLock)
            return _worldAuthorityBroadcastContext is { } current
                && SameWorldAuthorityBroadcastContext(current, queuedContext)
                && !_worldAuthorityBroadcastCancellation.IsCancellationRequested;
    }

    private CancellationToken GetWorldAuthorityBroadcastToken(
        WorldAuthorityBroadcastContext queuedContext)
    {
        lock (_worldAuthorityBroadcastLock)
        {
            if (_worldAuthorityBroadcastContext is not { } current
                || !SameWorldAuthorityBroadcastContext(current, queuedContext))
                return new CancellationToken(canceled: true);
            return _worldAuthorityBroadcastCancellation.Token;
        }
    }

    private async Task NotifyWorldAuthorityUnhealthySafelyAsync(
        WorldAuthorityHealth health,
        WorldAuthorityBroadcastContext failedBroadcastContext)
    {
        try
        {
            foreach (var client in _clients.Values)
            {
                // A SHA-changing WorldContext reset may legally restore healthy
                // authority with the same generation+manifest while this async
                // notification is still pending. Never poison that new context
                // with the previous context's terminal Unavailable status.
                if (!IsSameWorldAuthorityBroadcastContext(failedBroadcastContext)) return;

                var status = new WorldTransactionRecoveryStatus(
                    SessionId,
                    client.PeerId,
                    health.WorldGeneration,
                    WorldTransactionRecoveryStatusCode.Unavailable,
                    health.LastHostGameplayCommittedSequence,
                    health.LastHostGameplayCommittedSequence,
                    $"Host authority unhealthy after transaction {health.FailedAuthorityTransaction}; rejoin/resynchronization is required.");
                try
                {
                    await client.Connection.SendAsync(
                        MessageType.WorldTransactionRecoveryStatus,
                        JsonPayload.Serialize(status),
                        _shutdown.Token).ConfigureAwait(false);
                }
                catch (Exception exception) when (exception is IOException or SocketException
                    or ObjectDisposedException or OperationCanceledException)
                {
                    if (!_shutdown.IsCancellationRequested)
                        Log?.Invoke(this,
                            $"Could not notify peer {client.PeerId} of WS3 authority fail-stop: {exception.Message}");
                }
            }
        }
        catch (OperationCanceledException) when (_shutdown.IsCancellationRequested)
        {
        }
    }

    public async Task<WorldNetworkMutation?> SubmitWorldIntentAsync(
        WorldNetworkIntent intent,
        CancellationToken cancellationToken = default)
    {
        await _worldMutationGate.WaitAsync(cancellationToken).ConfigureAwait(false);
        try
        {
            if (!AreWorldPeersCompatible())
            {
                if (IsRejectedPublicRoot(intent)) WorldIntentRejected?.Invoke(this, intent);
                throw new ProtocolException("WorldIntent rejected: WorldManifest compatibility barrier is not open.");
            }
            if (!_worldAuthority.TryAcceptCausalFragment(intent, HostPeerId,
                    out var mutations, out var rejectedIntents, out var rejection)) {
                foreach (var rejected in rejectedIntents)
                    if (IsRejectedPublicRoot(rejected)) WorldIntentRejected?.Invoke(this, rejected);
                throw new ProtocolException($"WorldIntent rejected: {rejection}.");
            }
            WorldAuthorityContextIdentity acceptedAuthorityContext = default;
            if (mutations.Count != 0)
            {
                if (!_worldAuthority.TryGetAuthorityContextIdentity(out acceptedAuthorityContext))
                    throw new ProtocolException(
                        "WorldIntent was accepted but its immutable Host authority context is unavailable.");
            }
            foreach (var mutation in mutations) {
                if (!TryCaptureWorldAuthorityBroadcast(mutation, out var queued))
                    throw new ProtocolException(
                        "WorldIntent was accepted but its Host authority broadcast context was invalidated before enqueue.");
                WorldMutationAccepted?.Invoke(this,
                    new WorldHostAcceptedMutation(mutation, acceptedAuthorityContext));
                await _worldMutationBroadcasts.Writer.WriteAsync(queued, cancellationToken).ConfigureAwait(false);
            }
            return mutations.Count == 0 ? null : mutations[^1];
        }
        finally
        {
            _worldMutationGate.Release();
        }
    }

    public async Task WaitForWorldCompatibilityAsync(
        TimeSpan timeout,
        CancellationToken cancellationToken = default)
    {
        if (timeout <= TimeSpan.Zero || timeout > TimeSpan.FromMinutes(1))
            throw new ArgumentOutOfRangeException(nameof(timeout));
        var deadline = Environment.TickCount64 + (long)timeout.TotalMilliseconds;
        while (Environment.TickCount64 < deadline)
        {
            cancellationToken.ThrowIfCancellationRequested();
            var peers = _clients.Keys.ToArray();
            if (peers.Length != 0 && _worldAuthority.AllPeersCompatible(peers)) return;
            await Task.Delay(50, cancellationToken).ConfigureAwait(false);
        }
        throw new TimeoutException("Not every connected peer acknowledged the same WorldManifest before the deadline.");
    }

    public bool AreWorldPeersCompatible()
    {
        var peers = _clients.Keys.ToArray();
        return peers.Length != 0 && _worldAuthority.AllPeersCompatible(peers);
    }

    public IReadOnlyList<WorldNetworkMutation> CreateWorldReplaySnapshot() =>
        _worldAuthority.ReplaySnapshot();

    private void ClearActiveSpawnRound()
    {
        lock (_stateLock) _activeSpawnRound = null;
    }

    private async Task<SpawnRemotePlayersPayload> CollectAndBroadcastSpawnPositionsAsync(
        int levelGeneration,
        PlayerSpawnPositionPayload hostPosition,
        CancellationToken cancellationToken)
    {
        var round = CreateSpawnPositionRound(levelGeneration, hostPosition);
        await BroadcastSpawnPositionRequestAsync(round, cancellationToken).ConfigureAwait(false);
        var positions = await round.WaitAsync(TimeSpan.FromSeconds(20), cancellationToken)
            .ConfigureAwait(false);
        return await BroadcastSpawnRemotePlayersAsync(round, positions, cancellationToken)
            .ConfigureAwait(false);
    }

    private SpawnPositionRound CreateSpawnPositionRound(
        int levelGeneration,
        PlayerSpawnPositionPayload hostPosition)
    {
        ValidateActiveGeneration(levelGeneration);
        ModelValidation.Validate(hostPosition);
        if (hostPosition.PeerId != HostPeerId)
            throw new InvalidOperationException("The host spawn position identity is invalid.");
        var request = new SpawnPositionRequestPayload(
            SessionId, Guid.NewGuid(), HostPeerId, levelGeneration);
        ModelValidation.Validate(request);
        var round = new SpawnPositionRound(request, _clients.Keys, hostPosition);
        lock (_stateLock) _activeSpawnRound = round;
        return round;
    }

    private Task BroadcastSpawnPositionRequestAsync(
        SpawnPositionRound round,
        CancellationToken cancellationToken) =>
        BroadcastAsync(MessageType.SpawnPositionRequest,
            JsonPayload.Serialize(round.Request), cancellationToken);

    private async Task<SpawnRemotePlayersPayload> BroadcastSpawnRemotePlayersAsync(
        SpawnPositionRound round,
        IReadOnlyList<PlayerSpawnPositionPayload> positions,
        CancellationToken cancellationToken)
    {
        var payload = new SpawnRemotePlayersPayload(SessionId, round.Request.RequestId,
            HostPeerId, round.Request.LevelGeneration, AssignCharacters(positions));
        ModelValidation.Validate(payload);
        await BroadcastAsync(MessageType.SpawnRemotePlayers, JsonPayload.Serialize(payload),
            cancellationToken).ConfigureAwait(false);
        SpawnRemotePlayersRequested?.Invoke(this, payload);
        return payload;
    }

    // One distinct crew character per player. A player keeps its character for
    // the whole session (later rounds and levels); departed players free theirs.
    // The Host is E_RIPLEY, newcomers take the first free one in catalog order.
    private IReadOnlyList<PlayerSpawnPositionPayload> AssignCharacters(
        IReadOnlyList<PlayerSpawnPositionPayload> positions)
    {
        IReadOnlyDictionary<ulong, string> characters;
        lock (_stateLock)
            characters = PlayerCharacterCatalog.Assign(_characterByPeer,
                positions.Select(p => p.PeerId).ToArray(), HostPeerId);
        return positions.Select(p => p with
        {
            Character = characters.TryGetValue(p.PeerId, out var name) ? name : null
        }).ToArray();
    }

    private GameplayReadyPayload CreateGameplayReady(ulong peerId, int levelGeneration)
    {
        ValidateActiveGeneration(levelGeneration);
        var payload = new GameplayReadyPayload(SessionId, peerId, levelGeneration);
        ModelValidation.Validate(payload);
        return payload;
    }

    private void ValidateActiveGeneration(int levelGeneration)
    {
        lock (_stateLock)
        {
            if (string.Equals(_state, "Starting", StringComparison.Ordinal)
                && levelGeneration == _levelGeneration) return;
        }
        throw new InvalidOperationException(
            "The request does not match the active launch generation.");
    }

    private async Task AcceptLoopAsync(CancellationToken cancellationToken)
    {
        while (!cancellationToken.IsCancellationRequested)
        {
            try
            {
                var tcpClient = await _listener.AcceptTcpClientAsync(cancellationToken)
                    .ConfigureAwait(false);
                var task = HandleClientAsync(tcpClient, cancellationToken);
                lock (_clientTasks)
                {
                    _clientTasks.Add(task);
                }

                _ = task.ContinueWith(
                    completed =>
                    {
                        lock (_clientTasks)
                        {
                            _clientTasks.Remove(completed);
                        }
                    },
                    CancellationToken.None,
                    TaskContinuationOptions.ExecuteSynchronously,
                    TaskScheduler.Default);
            }
            catch (OperationCanceledException) when (cancellationToken.IsCancellationRequested)
            {
                break;
            }
            catch (SocketException) when (cancellationToken.IsCancellationRequested)
            {
                break;
            }
        }
    }

    private async Task HandleClientAsync(TcpClient tcpClient, CancellationToken cancellationToken)
    {
        await using var connection = new ProtocolConnection(tcpClient);
        ClientState? state = null;
        try
        {
            using var handshakeTimeout = CancellationTokenSource.CreateLinkedTokenSource(
                cancellationToken);
            handshakeTimeout.CancelAfter(TimeSpan.FromSeconds(5));
            var first = await connection.ReadAsync(handshakeTimeout.Token).ConfigureAwait(false);
            if (first.Type != MessageType.LobbyJoin)
            {
                await SendErrorAsync(connection, "HANDSHAKE_EXPECTED", "Expected LOBBY_JOIN.",
                    cancellationToken).ConfigureAwait(false);
                return;
            }

            var request = JsonPayload.Deserialize<LobbyJoinPayload>(first.Payload.Span);
            try
            {
                ModelValidation.Validate(request);
            }
            catch (ProtocolException exception)
            {
                await SendErrorAsync(connection, "INVALID_JOIN", exception.Message,
                    cancellationToken).ConfigureAwait(false);
                return;
            }

            if (!PlayerName.TryNormalize(request.DisplayName, out var displayName, out var nameError))
            {
                await SendErrorAsync(connection, "INVALID_NAME", nameError, cancellationToken)
                    .ConfigureAwait(false);
                return;
            }

            var incompatibility = FindIncompatibility(request.Compatibility);
            if (incompatibility is not null)
            {
                await SendErrorAsync(connection, "INCOMPATIBLE", incompatibility, cancellationToken)
                    .ConfigureAwait(false);
                return;
            }

            string? admissionErrorCode = null;
            string? admissionErrorMessage = null;
            await _admissionGate.WaitAsync(cancellationToken).ConfigureAwait(false);
            try
            {
                bool lobbyAcceptsJoin;
                lock (_stateLock)
                {
                    lobbyAcceptsJoin = string.Equals(
                        _state,
                        "Lobby",
                        StringComparison.Ordinal);
                }

                if (!lobbyAcceptsJoin)
                {
                    admissionErrorCode = "LOBBY_ALREADY_STARTING";
                    admissionErrorMessage =
                        "The lobby no longer accepts players after launch.";
                }
                else if (_clients.Count + 1 >= _options.Capacity)
                {
                    admissionErrorCode = "LOBBY_FULL";
                    admissionErrorMessage = "The lobby is full.";
                }
                else if (request.LauncherPlayerId == _options.HostLauncherPlayerId
                    || _clients.Values.Any(client =>
                        client.LauncherPlayerId == request.LauncherPlayerId))
                {
                    admissionErrorCode = "DUPLICATE_PLAYER_ID";
                    admissionErrorMessage = "LauncherPlayerId is already present in the lobby.";
                }
                else
                {
                    var peerId = NewUniquePeerId();
                    state = new ClientState(
                        peerId,
                        request.LauncherPlayerId,
                        displayName,
                        connection,
                        request.ConnectionNonce);
                    if (!_clients.TryAdd(peerId, state))
                    {
                        throw new InvalidOperationException("PeerId collision.");
                    }
                }
            }
            finally
            {
                _admissionGate.Release();
            }

            if (admissionErrorCode is not null)
            {
                await SendErrorAsync(
                    connection,
                    admissionErrorCode,
                    admissionErrorMessage!,
                    cancellationToken).ConfigureAwait(false);
                return;
            }

            if (state is null)
            {
                throw new InvalidOperationException("Client admission is inconsistent.");
            }

            var accepted = new LobbyJoinAcceptedPayload(
                SessionId,
                state.PeerId,
                _options.Capacity,
                _options.OpaqueRelayEnabled,
                request.ConnectionNonce);
            await connection.SendAsync(MessageType.LobbyJoinAccepted,
                JsonPayload.Serialize(accepted), cancellationToken).ConfigureAwait(false);

            Log?.Invoke(this, $"{displayName} joined the lobby (PeerId {state.PeerId}).");
            PublishSnapshot();
            await BroadcastSnapshotAsync(cancellationToken).ConfigureAwait(false);
            await ClientReadLoopAsync(state, cancellationToken).ConfigureAwait(false);
        }
        catch (OperationCanceledException) when (cancellationToken.IsCancellationRequested)
        {
        }
        catch (OperationCanceledException)
        {
            Log?.Invoke(this, "Client handshake timed out.");
        }
        catch (EndOfStreamException)
        {
        }
        catch (IOException)
        {
        }
        catch (ProtocolException exception)
        {
            Log?.Invoke(this, $"Client rejected: {exception.Message}");
        }
        finally
        {
            var removed = false;
            if (state is not null)
            {
                await _admissionGate.WaitAsync().ConfigureAwait(false);
                try
                {
                    removed = _clients.TryRemove(state.PeerId, out _);
                }
                finally
                {
                    _admissionGate.Release();
                }
            }

            if (state is not null && removed)
            {
                await RemoveShadowPeerAsync(state.PeerId).ConfigureAwait(false);
                await RemoveExecutionPeerAsync(state.PeerId).ConfigureAwait(false);
                lock (_stateLock) {
                    // Preserve the departed Alien identity until PvP round logic
                    // can declare a survivor victory. Clear it for a lobby exit.
                    if (_alienPeerId == state.PeerId && _state == "Lobby") _alienPeerId = 0;
                    _lobbyPreparations.Remove(state.PeerId);
                    _sessionPreparedPeers.Remove(state.PeerId);
                }
                _ = EvaluateSessionReleaseAfterDepartureAsync();
                _worldAuthority.RemovePeer(state.PeerId);
                Log?.Invoke(this, $"{state.DisplayName} left the lobby.");
                PublishSnapshot();
                _ = BroadcastSnapshotSafelyAsync();
                _ = EvaluateRoundAfterDepartureAsync();
            }
        }
    }

    private async Task ClientReadLoopAsync(ClientState state, CancellationToken cancellationToken)
    {
        while (!cancellationToken.IsCancellationRequested)
        {
            var frame = await state.Connection.ReadAsync(cancellationToken).ConfigureAwait(false);
            state.Touch();
            switch (frame.Type)
            {
                case MessageType.CampaignSaveRequest:
                    QueueCampaignSaveDownload(state, frame.Payload);
                    break;
                case MessageType.CampaignSaveChunk:
                    throw new ProtocolException("Clients may not upload campaign saves.");
                case MessageType.CinematicSkipCommitted:
                    throw new ProtocolException("Only the Host can skip a campaign cinematic.");
                case MessageType.AISyncExecutionControl:
                    HandleAISyncExecutionControl(state, frame.Payload);
                    break;
                case MessageType.AISyncExecutionNetworkReceipt:
                    HandleAISyncExecutionReceipt(state, frame.Payload);
                    break;
                case MessageType.AISyncExecutionNetworkBatch:
                case MessageType.AISyncCausalStateNetwork:
                case MessageType.AISyncRouteNetwork:
                case MessageType.AISyncExecutionNetworkRetry:
                    throw new ProtocolException("Only the Host may publish executable AISync commands.");
                case MessageType.AISyncShadowReceipt:
                    await HandleAISyncShadowReceiptAsync(state, frame.Payload, cancellationToken).ConfigureAwait(false);
                    break;
                case MessageType.AISyncShadowRecovery:
                    HandleAISyncShadowRecovery(state, frame.Payload);
                    break;
                case MessageType.AISyncShadowOpen:
                case MessageType.AISyncShadowData:
                    throw new ProtocolException("Only the host may publish AISync shadow observations.");
                case MessageType.Heartbeat:
                    break;

                case MessageType.LobbyReady:
                    var ready = JsonPayload.Deserialize<LobbyReadyPayload>(frame.Payload.Span);
                    var lobbyAcceptsReady = false;
                    await _admissionGate.WaitAsync(cancellationToken).ConfigureAwait(false);
                    try
                    {
                        lock (_stateLock)
                        {
                            lobbyAcceptsReady = string.Equals(
                                _state,
                                "Lobby",
                                StringComparison.Ordinal);
                            if (lobbyAcceptsReady)
                            {
                                state.SetReady(ready.Ready);
                            }
                        }
                    }
                    finally
                    {
                        _admissionGate.Release();
                    }

                    if (!lobbyAcceptsReady)
                    {
                        await SendErrorAsync(
                            state.Connection,
                            "LOBBY_ALREADY_STARTING",
                            "Ready state cannot change after launch.",
                            cancellationToken).ConfigureAwait(false);
                        break;
                    }

                    PublishSnapshot();
                    await BroadcastSnapshotAsync(cancellationToken).ConfigureAwait(false);
                    break;

                case MessageType.LobbyPreparation:
                    await HandleLobbyPreparationAsync(state, frame.Payload, cancellationToken)
                        .ConfigureAwait(false);
                    break;

                case MessageType.SessionPrepared:
                    await HandleSessionPreparedAsync(state, frame.Payload, cancellationToken)
                        .ConfigureAwait(false);
                    break;

                case MessageType.GameplayReady:
                    await HandleGameplayReadyAsync(state, frame.Payload, cancellationToken)
                        .ConfigureAwait(false);
                    break;

                case MessageType.SpawnPositionResponse:
                    await HandleSpawnPositionResponseAsync(state, frame.Payload, cancellationToken)
                        .ConfigureAwait(false);
                    break;

                case MessageType.PlayerDeath:
                    await HandlePlayerDeathAsync(state, frame.Payload, cancellationToken)
                        .ConfigureAwait(false);
                    break;

                case MessageType.PlayerMissionComplete:
                    await HandlePlayerMissionCompleteAsync(state, frame.Payload, cancellationToken)
                        .ConfigureAwait(false);
                    break;

                case MessageType.VoiceFrame:
                    await HandleVoiceFrameAsync(state, frame.Payload, cancellationToken)
                        .ConfigureAwait(false);
                    break;

                case MessageType.MissionSignalProposal:
                    await HandleMissionSignalAsync(state, frame.Payload, cancellationToken)
                        .ConfigureAwait(false);
                    break;

                case MessageType.ItemPickupRequest:
                    await HandleItemPickupRequestAsync(state, frame.Payload, cancellationToken)
                        .ConfigureAwait(false);
                    break;

                case MessageType.ItemSpawnProposal:
                    await HandleItemSpawnProposalAsync(state, frame.Payload, cancellationToken)
                        .ConfigureAwait(false);
                    break;

                case MessageType.ThrowableProposal:
                    HandleThrowableProposal(state, frame.Payload);
                    break;

                case MessageType.PlayerPositionSnapshot:
                    await HandlePlayerPositionAsync(state, frame.Payload, cancellationToken)
                        .ConfigureAwait(false);
                    break;

                case MessageType.PlayerEquipmentSnapshot:
                    await HandlePlayerEquipmentAsync(state, frame.Payload, cancellationToken)
                        .ConfigureAwait(false);
                    break;

                case MessageType.AlienControlProposal:
                    await HandleAlienControlProposalAsync(state, frame.Payload, cancellationToken)
                        .ConfigureAwait(false);
                    break;

                case MessageType.AlienControlReady:
                    await HandleAlienReadyAsync(state,frame.Payload,cancellationToken).ConfigureAwait(false);
                    break;
                case MessageType.AlienPoseObservation:
                    await HandleAlienPoseObservationAsync(state,frame.Payload,cancellationToken).ConfigureAwait(false);
                    break;
                case MessageType.AlienControlLease:
                case MessageType.AlienActionResolved:
                case MessageType.AlienControlPose:
                case MessageType.AlienControlCommitted:
                    throw new ProtocolException("Only the Host may commit Alien control intents.");

                case MessageType.PlayerActionEvent:
                    await HandlePlayerActionAsync(state, frame.Payload, cancellationToken)
                        .ConfigureAwait(false);
                    break;

                case MessageType.AISyncNetworkImpact:
                    await HandleAISyncImpactAsync(state, frame.Payload, cancellationToken)
                        .ConfigureAwait(false);
                    break;

                case MessageType.WorldContext:
                    await HandleWorldContextAsync(state, frame.Payload, cancellationToken)
                        .ConfigureAwait(false);
                    break;

                case MessageType.WorldNetworkIntent:
                    await HandleWorldIntentAsync(state, frame.Payload, cancellationToken)
                        .ConfigureAwait(false);
                    break;

                case MessageType.WorldNetworkMutationAck:
                    HandleWorldMutationAck(state, frame.Payload);
                    break;

                case MessageType.WorldTransactionRecoveryRequest:
                    await HandleWorldRecoveryRequestAsync(state, frame.Payload, cancellationToken)
                        .ConfigureAwait(false);
                    break;

                case MessageType.LobbyLeave:
                case MessageType.Shutdown:
                    return;

                case MessageType.OutboundNetZFrame:
                    if (!_options.OpaqueRelayEnabled)
                    {
                        await SendErrorAsync(
                            state.Connection,
                            "NATIVE_RELAY_DISABLED",
                            "NetZ relay remains disabled until native framing is proven.",
                            cancellationToken).ConfigureAwait(false);
                        break;
                    }

                    await RelayOpaqueAsync(state.PeerId, frame.Payload, cancellationToken)
                        .ConfigureAwait(false);
                    break;

                default:
                    await SendErrorAsync(state.Connection, "UNEXPECTED_MESSAGE",
                        $"Message {frame.Type} is not allowed in the lobby.", cancellationToken)
                        .ConfigureAwait(false);
                    break;
            }
        }
    }

    private async Task HandleWorldContextAsync(
        ClientState state,
        ReadOnlyMemory<byte> bytes,
        CancellationToken cancellationToken)
    {
        WorldNetworkContext context;
        await _worldMutationGate.WaitAsync(cancellationToken).ConfigureAwait(false);
        try
        {
            try
            {
                context = WorldReplicationCodec.DecodeNetworkContext(bytes.Span);
                if (context.SessionId != SessionId || context.PeerId != state.PeerId)
                    throw new ProtocolException("World context peer identity is invalid.");
                _worldAuthority.SetPeerContext(context);
            }
            catch (ProtocolException exception)
            {
                await SendErrorAsync(state.Connection, "WORLD_CONTEXT_REJECTED", exception.Message,
                    cancellationToken).ConfigureAwait(false);
                return;
            }

            // Protocol v5 recovery is transaction/checkpoint based. Do not push
            // ReplaySnapshot here: it contains only selected persistent state and
            // cannot prove that the causal sequence before those mutations was
            // committed. The compatible peer will immediately request the exact
            // journal suffix after its last committed authority sequence.
        }
        finally
        {
            _worldMutationGate.Release();
        }
        WorldContextReceived?.Invoke(this, context);
    }

    // Only a refused v2 ROOT can retire its local observation this way. An
    // accepted intent keeps the observation until its authoritative echo arrives.
    private static bool IsRejectedPublicRoot(WorldNetworkIntent envelope) =>
        envelope.Intent.Operation == WorldReplicationConstants.CompositePublicInputOperation
        && envelope.Intent.RequestedKind == WorldMutationKind.Event
        && WorldPublicActionCodec.TryDecode(envelope.Intent.Payload, out _, out _, out var retailRoot)
        && retailRoot;

    private async Task HandleWorldIntentAsync(
        ClientState state,
        ReadOnlyMemory<byte> bytes,
        CancellationToken cancellationToken)
    {
        WorldNetworkIntent intent;
        try
        {
            intent = WorldReplicationCodec.DecodeNetworkIntent(bytes.Span);
        }
        catch (ProtocolException exception)
        {
            await SendErrorAsync(state.Connection, "WORLD_INTENT_MALFORMED", exception.Message,
                cancellationToken).ConfigureAwait(false);
            return;
        }
        await _worldMutationGate.WaitAsync(cancellationToken).ConfigureAwait(false);
        try
        {
            if (!AreWorldPeersCompatible())
            {
                if (IsRejectedPublicRoot(intent))
                    await state.Connection.SendAsync(MessageType.WorldNetworkIntentRejected,
                        WorldReplicationCodec.EncodeNetworkIntent(intent), cancellationToken)
                        .ConfigureAwait(false);
                await SendErrorAsync(state.Connection, "WORLD_INTENT_REJECTED",
                    "WorldManifest compatibility barrier is not open.",
                    cancellationToken).ConfigureAwait(false);
                return;
            }
            if (!_worldAuthority.TryAcceptCausalFragment(intent, state.PeerId,
                    out var mutations, out var rejectedIntents, out var rejection)) {
                foreach (var rejected in rejectedIntents)
                    if (IsRejectedPublicRoot(rejected))
                        await state.Connection.SendAsync(MessageType.WorldNetworkIntentRejected,
                            WorldReplicationCodec.EncodeNetworkIntent(rejected), cancellationToken).ConfigureAwait(false);
                var rejectionCode = string.Equals(rejection, "WS3_AUTHORITY_UNHEALTHY", StringComparison.Ordinal)
                    ? "WORLD_AUTHORITY_UNHEALTHY"
                    : "WORLD_INTENT_REJECTED";
                await SendErrorAsync(state.Connection, rejectionCode, rejection,
                    cancellationToken).ConfigureAwait(false);
                return;
            }
            WorldAuthorityContextIdentity acceptedAuthorityContext = default;
            if (mutations.Count != 0)
            {
                if (!_worldAuthority.TryGetAuthorityContextIdentity(out acceptedAuthorityContext))
                {
                    await SendErrorAsync(state.Connection, "WORLD_AUTHORITY_UNHEALTHY",
                        "Host authority context identity disappeared after acceptance; rejoin/resynchronization is required.",
                        cancellationToken).ConfigureAwait(false);
                    return;
                }
            }
            foreach (var mutation in mutations) {
                if (!TryCaptureWorldAuthorityBroadcast(mutation, out var queued))
                {
                    await SendErrorAsync(state.Connection, "WORLD_AUTHORITY_UNHEALTHY",
                        "Host authority broadcast context was invalidated after acceptance; rejoin/resynchronization is required.",
                        cancellationToken).ConfigureAwait(false);
                    return;
                }
                WorldMutationAccepted?.Invoke(this,
                    new WorldHostAcceptedMutation(mutation, acceptedAuthorityContext));
                await _worldMutationBroadcasts.Writer.WriteAsync(queued, cancellationToken).ConfigureAwait(false);
            }
        }
        finally
        {
            _worldMutationGate.Release();
        }
    }

    private void HandleWorldMutationAck(ClientState state, ReadOnlyMemory<byte> bytes)
    {
        var ack = WorldReplicationCodec.DecodeNetworkAck(bytes.Span);
        if (ack.SessionId != SessionId || ack.PeerId != state.PeerId)
            throw new ProtocolException("World mutation acknowledgement identity is invalid.");
        WorldMutationAckReceived?.Invoke(this, ack);
    }

    private async Task HandleWorldRecoveryRequestAsync(
        ClientState state,
        ReadOnlyMemory<byte> bytes,
        CancellationToken cancellationToken)
    {
        var request = JsonPayload.Deserialize<WorldTransactionRecoveryRequest>(bytes.Span);
        ModelValidation.Validate(request);
        if (request.SessionId != SessionId || request.PeerId != state.PeerId)
            throw new ProtocolException("World recovery request identity is invalid.");

        await _worldMutationGate.WaitAsync(cancellationToken).ConfigureAwait(false);
        try
        {
            var health = _worldAuthority.AuthorityHealth;
            // A recovery frontier is a HostGameplayCommitted frontier, never the
            // latest sequence merely accepted/journaled by the authority.
            var latest = health.LastHostGameplayCommittedSequence;
            WorldTransactionRecoveryStatus status;
            if (!health.Healthy && health.WorldGeneration == request.WorldGeneration)
            {
                status = new WorldTransactionRecoveryStatus(SessionId, state.PeerId,
                    request.WorldGeneration, WorldTransactionRecoveryStatusCode.Unavailable,
                    request.LastCommittedSequence, latest,
                    $"Host authority is unhealthy after transaction {health.FailedAuthorityTransaction}; rejoin/resynchronization is required.");
            }
            else if (!_worldAuthority.IsPeerRecoveryCompatible(state.PeerId, request.WorldGeneration))
            {
                status = new WorldTransactionRecoveryStatus(SessionId, state.PeerId,
                    request.WorldGeneration, WorldTransactionRecoveryStatusCode.ContextRejected,
                    request.LastCommittedSequence, latest,
                    "The peer WorldContext is not compatible with the current Host generation.");
            }
            else if (health.LatestAcceptedSequence != health.LastHostGameplayCommittedSequence)
            {
                status = new WorldTransactionRecoveryStatus(SessionId, state.PeerId,
                    request.WorldGeneration, WorldTransactionRecoveryStatusCode.Unavailable,
                    request.LastCommittedSequence, latest,
                    "Host recovery frontier is not commit-stable because an accepted transaction has not reached HostGameplayCommitted.");
            }
            else if (_worldAuthority.TryGetCausalTransactionsAfter(
                request.LastCommittedSequence, out var missing))
            {
                foreach (var mutation in missing)
                    await state.Connection.SendAsync(MessageType.WorldNetworkMutation,
                        WorldReplicationCodec.EncodeNetworkMutation(mutation), cancellationToken)
                        .ConfigureAwait(false);
                status = new WorldTransactionRecoveryStatus(SessionId, state.PeerId,
                    request.WorldGeneration, WorldTransactionRecoveryStatusCode.Complete,
                    request.LastCommittedSequence, latest,
                    missing.Count == 0 ? "Already current." : "Journal replay sent.");
            }
            else
            {
                status = new WorldTransactionRecoveryStatus(SessionId, state.PeerId,
                    request.WorldGeneration, WorldTransactionRecoveryStatusCode.Unavailable,
                    request.LastCommittedSequence, latest,
                    "The Host journal no longer contains one contiguous recovery suffix.");
            }
            await state.Connection.SendAsync(MessageType.WorldTransactionRecoveryStatus,
                JsonPayload.Serialize(status), cancellationToken).ConfigureAwait(false);
        }
        finally
        {
            _worldMutationGate.Release();
        }
    }

    private async Task HandlePlayerDeathAsync(
        ClientState state,
        ReadOnlyMemory<byte> bytes,
        CancellationToken cancellationToken)
    {
        var payload = JsonPayload.Deserialize<PlayerDeathPayload>(bytes.Span);
        ModelValidation.Validate(payload);
        if (payload.SessionId != SessionId || payload.PeerId != state.PeerId)
        {
            await SendErrorAsync(state.Connection, "INVALID_PLAYER_DEATH",
                "Player death identity is invalid.", cancellationToken)
                .ConfigureAwait(false);
            return;
        }

        // A stale generation (death reported after the round already ended)
        // is ignored without error: the client is already returning to Lobby.
        await RecordPlayerDeathAsync(payload, cancellationToken).ConfigureAwait(false);
    }

    private async Task HandleItemPickupRequestAsync(
        ClientState state,
        ReadOnlyMemory<byte> bytes,
        CancellationToken cancellationToken)
    {
        var payload = JsonPayload.Deserialize<ItemPickupRequestPayload>(bytes.Span);
        ModelValidation.Validate(payload);
        if (payload.SessionId != SessionId || payload.PeerId != state.PeerId)
        {
            await SendErrorAsync(state.Connection, "INVALID_ITEM_PICKUP",
                "Item pickup identity is invalid.", cancellationToken)
                .ConfigureAwait(false);
            return;
        }

        // A stale generation is ignored without error, like PlayerDeath.
        await RecordItemPickupAsync(payload, cancellationToken).ConfigureAwait(false);
    }

    private async Task HandleItemSpawnProposalAsync(
        ClientState state,
        ReadOnlyMemory<byte> bytes,
        CancellationToken cancellationToken)
    {
        var payload = JsonPayload.Deserialize<ItemSpawnPayload>(bytes.Span);
        ModelValidation.Validate(payload);
        if (payload.SessionId != SessionId || payload.PeerId != state.PeerId)
        {
            await SendErrorAsync(state.Connection, "INVALID_ITEM_SPAWN",
                "Item spawn identity is invalid.", cancellationToken)
                .ConfigureAwait(false);
            return;
        }

        await RecordItemSpawnAsync(payload, cancellationToken).ConfigureAwait(false);
    }

    private async Task HandleGameplayReadyAsync(
        ClientState state,
        ReadOnlyMemory<byte> bytes,
        CancellationToken cancellationToken)
    {
        var payload = JsonPayload.Deserialize<GameplayReadyPayload>(bytes.Span);
        ModelValidation.Validate(payload);
        if (payload.SessionId != SessionId || payload.PeerId != state.PeerId)
        {
            await SendErrorAsync(state.Connection, "INVALID_GAMEPLAY_READY",
                "Gameplay readiness identity is invalid.", cancellationToken)
                .ConfigureAwait(false);
            return;
        }

        try
        {
            CreateGameplayReady(state.PeerId, payload.LevelGeneration);
        }
        catch (InvalidOperationException exception)
        {
            await SendErrorAsync(state.Connection, "INVALID_GAMEPLAY_READY",
                exception.Message, cancellationToken).ConfigureAwait(false);
            return;
        }

        GameplayReadyReceived?.Invoke(this, payload);
        await BroadcastAsync(MessageType.GameplayReady, JsonPayload.Serialize(payload),
            cancellationToken).ConfigureAwait(false);
        await ReplayCollectedItemsAsync(state, payload.LevelGeneration, cancellationToken)
            .ConfigureAwait(false);
    }

    private async Task HandleSpawnPositionResponseAsync(
        ClientState state,
        ReadOnlyMemory<byte> bytes,
        CancellationToken cancellationToken)
    {
        var payload = JsonPayload.Deserialize<SpawnPositionResponsePayload>(bytes.Span);
        ModelValidation.Validate(payload);
        if (payload.PeerId == state.PeerId && TryAcceptSpawnPosition(payload)) return;
        await SendErrorAsync(state.Connection, "INVALID_SPAWN_POSITION",
            "The spawn-position response is invalid or no longer active.", cancellationToken)
            .ConfigureAwait(false);
    }

    private bool TryAcceptSpawnPosition(SpawnPositionResponsePayload payload)
    {
        lock (_stateLock) return payload.SessionId == SessionId
            && _activeSpawnRound?.TryAdd(payload) == true;
    }

    private async Task HandlePlayerPositionAsync(
        ClientState state,
        ReadOnlyMemory<byte> bytes,
        CancellationToken cancellationToken)
    {
        var payload = JsonPayload.Deserialize<PlayerPositionSnapshotPayload>(bytes.Span);
        if (!TryAcceptPlayerPosition(state, payload))
        {
            await SendErrorAsync(state.Connection, "INVALID_PLAYER_POSITION",
                "The player-position snapshot is invalid or stale.", cancellationToken)
                .ConfigureAwait(false);
            return;
        }
        PlayerPositionReceived?.Invoke(this, payload);
        await BroadcastExceptAsync(MessageType.PlayerPositionSnapshot,
            JsonPayload.Serialize(payload), state.PeerId, cancellationToken).ConfigureAwait(false);
    }

    private bool TryAcceptPlayerPosition(
        ClientState state,
        PlayerPositionSnapshotPayload payload)
    {
        try { ValidatePlayerPosition(payload, state.PeerId); }
        catch (Exception exception) when (exception is ProtocolException
            or InvalidOperationException) { return false; }
        return state.PositionWindow.TryAccept(payload);
    }

    private async Task HandlePlayerEquipmentAsync(
        ClientState state,
        ReadOnlyMemory<byte> bytes,
        CancellationToken cancellationToken)
    {
        var payload = JsonPayload.Deserialize<PlayerEquipmentSnapshotPayload>(bytes.Span);
        if (!TryAcceptPlayerEquipment(state, payload))
        {
            await SendErrorAsync(state.Connection, "INVALID_PLAYER_EQUIPMENT",
                "The player-equipment snapshot is invalid or stale.", cancellationToken)
                .ConfigureAwait(false);
            return;
        }
        PlayerEquipmentReceived?.Invoke(this, payload);
        await BroadcastExceptAsync(MessageType.PlayerEquipmentSnapshot,
            JsonPayload.Serialize(payload), state.PeerId, cancellationToken).ConfigureAwait(false);
    }

    private bool TryAcceptPlayerEquipment(
        ClientState state,
        PlayerEquipmentSnapshotPayload payload)
    {
        try { ValidatePlayerEquipment(payload, state.PeerId); }
        catch (Exception exception) when (exception is ProtocolException
            or InvalidOperationException) { return false; }
        return state.TryAcceptEquipmentSequence(payload.LevelGeneration, payload.Sequence);
    }

    private async Task HandlePlayerActionAsync(
        ClientState state,
        ReadOnlyMemory<byte> bytes,
        CancellationToken cancellationToken)
    {
        var payload = JsonPayload.Deserialize<PlayerActionEventPayload>(bytes.Span);
        if (!TryAcceptPlayerAction(state, payload))
        {
            await SendErrorAsync(state.Connection, "INVALID_PLAYER_ACTION",
                "The player-action event is invalid or stale.", cancellationToken)
                .ConfigureAwait(false);
            return;
        }
        PlayerActionReceived?.Invoke(this, payload);
        await BroadcastExceptAsync(MessageType.PlayerActionEvent,
            JsonPayload.Serialize(payload), state.PeerId, cancellationToken).ConfigureAwait(false);
    }

    private bool TryAcceptPlayerAction(
        ClientState state,
        PlayerActionEventPayload payload)
    {
        try { ValidatePlayerAction(payload, state.PeerId); }
        catch (Exception exception) when (exception is ProtocolException
            or InvalidOperationException) { return false; }
        return state.TryAcceptActionSequence(payload.LevelGeneration, payload.Sequence);
    }

    private async Task HandleAISyncImpactAsync(
        ClientState state,
        ReadOnlyMemory<byte> bytes,
        CancellationToken cancellationToken)
    {
        try
        {
            var native = AISyncCodec.DecodeImpactNetwork(bytes, SessionId);
            if (!_worldAuthority.TryGetAuthorityContextIdentity(out var context)
                || !AISyncCodec.ImpactBelongsTo(native.Span, SessionId, context.WorldGeneration)
                || AISyncCodec.ImpactSourcePeer(native.Span) != state.PeerId
                || !state.TryAcceptAISyncImpactSequence(context.WorldGeneration, AISyncCodec.ImpactSequence(native.Span)))
                throw new ProtocolException("AISync impact identity or sequence is invalid.");
            AISyncImpactReceived?.Invoke(this, native);
        }
        catch (Exception exception) when (exception is ProtocolException or InvalidOperationException)
        {
            await SendErrorAsync(state.Connection, "INVALID_AISYNC_IMPACT",
                exception.Message, cancellationToken).ConfigureAwait(false);
        }
    }

    private void ValidatePlayerAction(
        PlayerActionEventPayload payload,
        ulong expectedPeerId)
    {
        RejectAlienTechnicalPlayerSync(expectedPeerId);
        ModelValidation.Validate(payload);
        if (payload.SessionId != SessionId || payload.PeerId != expectedPeerId)
            throw new InvalidOperationException("Player-action identity is invalid.");
        ValidateActiveGeneration(payload.LevelGeneration);
    }

    private void ValidatePlayerEquipment(
        PlayerEquipmentSnapshotPayload payload,
        ulong expectedPeerId)
    {
        RejectAlienTechnicalPlayerSync(expectedPeerId);
        ModelValidation.Validate(payload);
        if (payload.SessionId != SessionId || payload.PeerId != expectedPeerId)
            throw new InvalidOperationException("Player-equipment identity is invalid.");
        ValidateActiveGeneration(payload.LevelGeneration);
    }

    // PvP inputs and native Alien outcomes travel on their own transport. No
    // hidden Amanda position, torch, weapon or attack may be presented as the
    // bound Alien to other participants.
    private void RejectAlienTechnicalPlayerSync(ulong peerId)
    {
        lock (_stateLock)
            if (_options.AlienVsSurvivors && _state == "Starting" && _alienPeerId == peerId)
                throw new ProtocolException("Human PlayerSync from the Alien peer is forbidden.");
    }

    private void ValidatePlayerPosition(
        PlayerPositionSnapshotPayload payload,
        ulong expectedPeerId)
    {
        RejectAlienTechnicalPlayerSync(expectedPeerId);
        ModelValidation.Validate(payload);
        if (payload.SessionId != SessionId || payload.PeerId != expectedPeerId)
            throw new InvalidOperationException("Player-position identity is invalid.");
        ValidateActiveGeneration(payload.LevelGeneration);
    }

    private async Task RelayOpaqueAsync(
        ulong senderPeerId,
        ReadOnlyMemory<byte> payload,
        CancellationToken cancellationToken)
    {
        // Preserve the payload exactly. This branch remains disabled until
        // the inbound and outbound NetZ formats have been demonstrated.
        var destinations = _clients.Values.Where(client => client.PeerId != senderPeerId).ToArray();
        foreach (var destination in destinations)
        {
            await destination.Connection.SendAsync(
                MessageType.InboundNetZFrame,
                payload,
                cancellationToken).ConfigureAwait(false);
        }
    }

    private async Task HeartbeatLoopAsync(CancellationToken cancellationToken)
    {
        using var timer = new PeriodicTimer(HeartbeatPeriod);
        while (await timer.WaitForNextTickAsync(cancellationToken).ConfigureAwait(false))
        {
            var now = Environment.TickCount64;
            foreach (var state in _clients.Values)
            {
                if (now - state.LastSeenTicks > PeerTimeout.TotalMilliseconds)
                {
                    state.Connection.Client.Dispose();
                    continue;
                }

                try
                {
                    await state.Connection.SendAsync(
                        MessageType.Heartbeat,
                        ReadOnlyMemory<byte>.Empty,
                        cancellationToken).ConfigureAwait(false);
                }
                catch (Exception exception) when (
                    exception is IOException or SocketException or ObjectDisposedException)
                {
                    state.Connection.Client.Dispose();
                }
            }
        }
    }

    private string? FindIncompatibility(CompatibilityInfo candidate)
    {
        var expected = _options.Compatibility;
        if (candidate.ProtocolVersion != ProtocolConstants.Version)
        {
            return "The protocol version is incompatible.";
        }

        // SHA and ProfileId remain diagnostic values. While opaque relay and native
        // replication are disabled, they do not form a lobby allowlist. Future
        // replication must compare a validated ABI and capability contract, not
        // only file identity.
        if (!string.Equals(candidate.DllVersion, expected.DllVersion, StringComparison.Ordinal)
            || !string.Equals(candidate.LauncherVersion, expected.LauncherVersion,
                StringComparison.Ordinal)
            || !string.Equals(candidate.CompatibilityHash, expected.CompatibilityHash,
                StringComparison.OrdinalIgnoreCase))
        {
            return "Versions or compatibility hashes differ.";
        }

        return null;
    }

    private LobbySnapshotPayload CreateSnapshot()
    {
        string level;
        string state;
        int levelGeneration;
        Guid preparationToken;
        LobbyPreparationPayload[] preparations;
        CampaignLaunchDescriptor? campaignLaunch;
        lock (_stateLock)
        {
            campaignLaunch = _campaignLaunch;
            level = _level;
            state = _state;
            levelGeneration = _levelGeneration;
            preparationToken = _preparationToken;
            preparations = _lobbyPreparations.Values
                .Where(value => value.PeerId == HostPeerId || _clients.ContainsKey(value.PeerId))
                .OrderBy(value => value.PeerId).ToArray();
        }

        var players = new List<LobbyPlayer>(_clients.Count + 1)
        {
            new(HostPeerId, _options.HostLauncherPlayerId, _options.HostDisplayName, true, true)
        };
        players.AddRange(_clients.Values
            .Select(client => client.ToPlayer())
            .OrderBy(player => player.PeerId));
        lock (_stateLock)
        {
            var characters = PlayerCharacterCatalog.Assign(_characterByPeer,
                players.Select(player => player.PeerId).ToArray(), HostPeerId);
            players = players.Select(player => player with
            {
                Character = characters.TryGetValue(player.PeerId, out var name) ? name : null
            }).ToList();
        }
        var snapshot = new LobbySnapshotPayload(
            SessionId,
            _options.ServerName,
            level,
            state,
            _options.Capacity,
            levelGeneration,
            players,
            preparationToken,
            preparations,
            _options.RequireWorldPreparation,
            _options.AlienEnabled,
            _options.AlienVsSurvivors,
            _alienPeerId,
            campaignLaunch);
        ModelValidation.Validate(snapshot);
        return snapshot;
    }

    private void PublishSnapshot()
    {
        // Several client/heartbeat paths may publish concurrently. Build and
        // deliver each local snapshot under one lock so an older snapshot
        // cannot be delivered after a newer state merely because its thread
        // was pre-empted between CreateSnapshot and the event callback.
        lock (_snapshotPublishLock)
        {
            SnapshotChanged?.Invoke(this, CreateSnapshot());
        }
    }

    private async Task BroadcastSnapshotAsync(CancellationToken cancellationToken)
    {
        // Build the payload only after obtaining the gate. If several state
        // changes queue broadcasts concurrently, a late caller therefore
        // captures the current state instead of sending a stale pre-built
        // payload after a newer one.
        await _snapshotBroadcastGate.WaitAsync(cancellationToken).ConfigureAwait(false);
        try
        {
            var payload = JsonPayload.Serialize(CreateSnapshot());
            await BroadcastAsync(MessageType.LobbySnapshot, payload, cancellationToken)
                .ConfigureAwait(false);
        }
        finally
        {
            _snapshotBroadcastGate.Release();
        }
    }

    private async Task BroadcastSnapshotSafelyAsync()
    {
        try
        {
            await BroadcastSnapshotAsync(_shutdown.Token).ConfigureAwait(false);
        }
        catch (OperationCanceledException) when (_shutdown.IsCancellationRequested)
        {
        }
    }

    private async Task WorldMutationBroadcastLoopAsync(CancellationToken cancellationToken)
    {
        try
        {
            await foreach (var queued in _worldMutationBroadcasts.Reader
                .ReadAllAsync(cancellationToken).ConfigureAwait(false))
            {
                var mutation = queued.Mutation;
                if (!IsCurrentWorldAuthorityBroadcastContext(queued.Context)
                    || !_worldAuthority.CanBroadcast(mutation))
                {
                    Log?.Invoke(this,
                        $"WS3 broadcast dropped: stale/unhealthy authority context " +
                        $"generation={mutation.WorldGeneration} sequence={mutation.Mutation.Sequence} " +
                        $"epoch={queued.Context.Epoch} sha256={queued.Context.ManifestSha256}.");
                    continue;
                }

                var authorityToken = GetWorldAuthorityBroadcastToken(queued.Context);
                using var linked = CancellationTokenSource.CreateLinkedTokenSource(
                    cancellationToken, authorityToken);
                try
                {
                    await BroadcastAsync(
                        MessageType.WorldNetworkMutation,
                        WorldReplicationCodec.EncodeNetworkMutation(mutation),
                        linked.Token).ConfigureAwait(false);
                }
                catch (OperationCanceledException) when (
                    authorityToken.IsCancellationRequested && !cancellationToken.IsCancellationRequested)
                {
                    Log?.Invoke(this,
                        $"WS3 broadcast aborted by Host authority context invalidation: " +
                        $"generation={mutation.WorldGeneration} sequence={mutation.Mutation.Sequence} " +
                        $"epoch={queued.Context.Epoch}.");
                }
            }
        }
        catch (OperationCanceledException) when (cancellationToken.IsCancellationRequested)
        {
        }
    }

    private async Task BroadcastAsync(
        MessageType type,
        ReadOnlyMemory<byte> payload,
        CancellationToken cancellationToken)
    {
        foreach (var client in _clients.Values)
        {
            try
            {
                await client.Connection.SendAsync(type, payload, cancellationToken)
                    .ConfigureAwait(false);
            }
            catch (Exception exception) when (
                exception is IOException or SocketException or ObjectDisposedException)
            {
                client.Connection.Client.Dispose();
            }
        }
    }

    private async Task BroadcastExceptAsync(
        MessageType type,
        ReadOnlyMemory<byte> payload,
        ulong excludedPeerId,
        CancellationToken cancellationToken)
    {
        var destinations = _clients.Values.Where(value => value.PeerId != excludedPeerId).ToArray();
        foreach (var client in destinations)
            await SendToClientSafelyAsync(client, type, payload, cancellationToken)
                .ConfigureAwait(false);
    }

    private static async Task SendToClientSafelyAsync(
        ClientState client,
        MessageType type,
        ReadOnlyMemory<byte> payload,
        CancellationToken cancellationToken)
    {
        try { await client.Connection.SendAsync(type, payload, cancellationToken)
            .ConfigureAwait(false); }
        catch (Exception exception) when (exception is IOException or SocketException
            or ObjectDisposedException) { client.Connection.Client.Dispose(); }
    }

    private static Task SendErrorAsync(
        ProtocolConnection connection,
        string code,
        string message,
        CancellationToken cancellationToken) =>
        connection.SendAsync(
            MessageType.Error,
            JsonPayload.Serialize(new ErrorPayload(code, message)),
            cancellationToken).AsTask();

    private ulong NewUniquePeerId()
    {
        for (var attempt = 0; attempt < 16; attempt++)
        {
            var candidate = NewPeerId();
            if (candidate != HostPeerId && !_clients.ContainsKey(candidate))
            {
                return candidate;
            }
        }

        throw new InvalidOperationException("A unique PeerId could not be assigned.");
    }

    private static ulong NewPeerId()
    {
        Span<byte> bytes = stackalloc byte[8];
        do
        {
            RandomNumberGenerator.Fill(bytes);
        }
        while (BinaryPrimitives.ReadUInt64LittleEndian(bytes) == 0);

        return BinaryPrimitives.ReadUInt64LittleEndian(bytes);
    }

    public async ValueTask DisposeAsync()
    {
        if (Interlocked.Exchange(ref _disposed, 1) != 0)
        {
            return;
        }

        _worldMutationBroadcasts.Writer.TryComplete();
        lock (_worldAuthorityBroadcastLock)
        {
            try { _worldAuthorityBroadcastCancellation.Cancel(); }
            catch (ObjectDisposedException) { }
            _worldAuthorityBroadcastCancellation.Dispose();
        }
        _shutdown.Cancel();
        await StopAISyncShadowAsync().ConfigureAwait(false);
        await StopAISyncExecutionAsync().ConfigureAwait(false);
        _listener.Stop();
        foreach (var client in _clients.Values)
        {
            client.Connection.Client.Dispose();
        }

        foreach (var task in new[] { _acceptLoop, _heartbeatLoop, _worldMutationBroadcastLoop, _missionInboundLoop }.OfType<Task>())
        {
            try
            {
                await task.ConfigureAwait(false);
            }
            catch (OperationCanceledException)
            {
            }
        }

        Task[] clientTasks;
        lock (_clientTasks)
        {
            clientTasks = _clientTasks.ToArray();
        }

        try
        {
            await Task.WhenAll(clientTasks).ConfigureAwait(false);
        }
        catch (Exception exception) when (
            exception is OperationCanceledException or IOException or SocketException)
        {
        }

        _shutdown.Dispose();
        _admissionGate.Dispose();
        _spawnPositionGate.Dispose();
        _worldMutationGate.Dispose();
    }

    private sealed class ClientState
    {
        public int CampaignSaveSending;
        public VoiceReceiveWindow VoiceWindow { get; } = new();
        public PlayerPositionReceiveWindow PositionWindow { get; } = new();
        private int _ready;
        private ulong _lastEquipmentSequence;
        private ulong _lastActionSequence;
        private ulong _lastAISyncImpactSequence;
        private int _equipmentGeneration, _actionGeneration, _impactGeneration;
        private long _lastSeenTicks = Environment.TickCount64;

        public ClientState(
            ulong peerId,
            Guid launcherPlayerId,
            string displayName,
            ProtocolConnection connection,
            long connectionGeneration)
        {
            PeerId = peerId;
            LauncherPlayerId = launcherPlayerId;
            DisplayName = displayName;
            Connection = connection;
            ConnectionGeneration = connectionGeneration;
        }

        public ulong PeerId { get; }

        public Guid LauncherPlayerId { get; }

        public string DisplayName { get; }

        public ProtocolConnection Connection { get; }

        public long ConnectionGeneration { get; }

        public long LastSeenTicks => Interlocked.Read(ref _lastSeenTicks);

        public void Touch() => Interlocked.Exchange(ref _lastSeenTicks, Environment.TickCount64);

        public void SetReady(bool ready) => Interlocked.Exchange(ref _ready, ready ? 1 : 0);

        public bool IsReady => Volatile.Read(ref _ready) != 0;

        public bool TryAcceptEquipmentSequence(int generation, ulong sequence) =>
            TryAcceptSequence(generation, sequence, ref _equipmentGeneration, ref _lastEquipmentSequence);

        public bool TryAcceptActionSequence(int generation, ulong sequence) =>
            TryAcceptSequence(generation, sequence, ref _actionGeneration, ref _lastActionSequence);

        public bool TryAcceptAISyncImpactSequence(int generation, uint sequence) =>
            TryAcceptSequence(generation, sequence, ref _impactGeneration, ref _lastAISyncImpactSequence);

        // These windows belong to a launch/world generation, not the TCP
        // connection. Only its ordered reader mutates them; an old packet must
        // never roll a window back after the next round has begun.
        private static bool TryAcceptSequence(int generation, ulong sequence, ref int currentGeneration, ref ulong last)
        {
            if (generation < 1 || generation < currentGeneration || sequence == 0) return false;
            if (generation != currentGeneration) { currentGeneration = generation; last = 0; }
            if (sequence <= last) return false;
            last = sequence;
            return true;
        }

        public LobbyPlayer ToPlayer() =>
            new(PeerId, LauncherPlayerId, DisplayName, false, IsReady);
    }
}
