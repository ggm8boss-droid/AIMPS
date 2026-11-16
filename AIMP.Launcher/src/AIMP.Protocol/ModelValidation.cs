namespace AIMP.Protocol;

using System.Globalization;
using System.Text;

public static class ModelValidation
{
    private static readonly UTF8Encoding StrictUtf8 = new(false, true);
    private const float MaximumLocomotionDirectionMagnitude = 8.0F;

    public static void Validate(CompatibilityInfo? value)
    {
        if (value is null
            || value.ProtocolVersion != ProtocolConstants.Version
            || !Bounded(value.LauncherVersion, 1, 64)
            || !Bounded(value.DllVersion, 1, 64)
            || !HexDigest(value.AiSha256)
            || !Bounded(value.ProfileId, 1, 128)
            || !HexDigest(value.CompatibilityHash))
        {
            throw new ProtocolException("Compatibility metadata is invalid.");
        }
    }

    public static void Validate(DiscoveryAnnouncement? value)
    {
        if (value is null
            || value.ProtocolVersion != ProtocolConstants.Version
            || value.SessionId == Guid.Empty
            || value.TcpPort is < 1 or > 65535
            || value.Capacity is < 1 or > 32
            || value.PlayerCount is < 0
            || value.PlayerCount > value.Capacity
            || !Bounded(value.ServerName, 1, ProtocolConstants.MaximumServerNameLength)
            || !PlayerName.TryNormalize(value.HostName, out _, out _)
            || !Bounded(value.Level, 1, 128)
            || !Bounded(value.State, 1, 32)
            || !HexDigest(value.CompatibilityHash))
        {
            throw new ProtocolException("Discovery announcement fields are out of bounds.");
        }
    }

    public static void Validate(LobbyJoinPayload? value)
    {
        if (value is null
            || value.LauncherPlayerId == Guid.Empty
            || !PlayerName.TryNormalize(value.DisplayName, out _, out _)
            || value.ConnectionNonce <= 0)
        {
            throw new ProtocolException("The lobby join request is invalid.");
        }

        Validate(value.Compatibility);
    }

    public static void Validate(LobbyJoinAcceptedPayload? value)
    {
        if (value is null
            || value.SessionId == Guid.Empty
            || value.PeerId == 0
            || value.Capacity is < 1 or > 32
            || value.ConnectionGeneration <= 0)
        {
            throw new ProtocolException("The lobby acceptance is invalid.");
        }
    }

    public static void Validate(GameplayReadyPayload? value)
    {
        if (value is null
            || value.SessionId == Guid.Empty
            || value.PeerId == 0
            || value.LevelGeneration < 1)
        {
            throw new ProtocolException("The gameplay-ready payload is invalid.");
        }
    }

    public static void Validate(PlayerDeathPayload? value)
    {
        if (value is null
            || value.SessionId == Guid.Empty
            || value.PeerId == 0
            || value.LevelGeneration < 1)
        {
            throw new ProtocolException("The player-death payload is invalid.");
        }
    }

    public static void Validate(PlayerMissionCompletePayload? value)
    {
        if (value is null
            || value.SessionId == Guid.Empty
            || value.PeerId == 0
            || value.LevelGeneration < 1)
        {
            throw new ProtocolException("The player mission-complete payload is invalid.");
        }
    }

    public static void Validate(RoundProgressPayload? value)
    {
        if (value is null
            || value.SessionId == Guid.Empty
            || value.LevelGeneration < 1
            || value.Participants is < 1 or > ProtocolConstants.MaximumLobbyCapacity
            || value.Finished < 0 || value.Dead < 0
            || value.Finished + value.Dead > value.Participants)
        {
            throw new ProtocolException("The round-progress payload is invalid.");
        }
    }

    public static void Validate(RoundEndedPayload? value)
    {
        if (value is null
            || value.SessionId == Guid.Empty
            || value.LevelGeneration < 1
            || string.IsNullOrWhiteSpace(value.Reason)
            || value.Reason.Length > 32
            || value.ReturnDelayMilliseconds is < 0 or > 60000)
        {
            throw new ProtocolException("The round-ended payload is invalid.");
        }
    }

    public static void Validate(NativePlayerLifecycleStatePayload? value)
    {
        // Schema 2 adds the monotonic local level-exit edge. A state must carry
        // at least one terminal edge (death or level exit). Schema 3 adds the
        // spectated participant, which only exists after a local death.
        if (value is null
            || value.SchemaVersion is not (1 or 2 or 3)
            || (value.SchemaVersion == 1 && (value.DeathSequence == 0 || !value.LocalPlayerDead))
            || (value.LocalPlayerDead != (value.DeathSequence != 0))
            || (value.DeathSequence == 0 && value.LevelExitSequence == 0)
            || (value.SchemaVersion < 3 && (value.SpectatePeer != 0 || value.SpectateCandidates != 0))
            || value.SpectateCandidates is < 0 or > 32
            || (value.SpectatePeer != 0 && !value.LocalPlayerDead))
        {
            throw new ProtocolException("The native player-lifecycle state is invalid.");
        }
    }

    public static void Validate(ItemPickupRequestPayload? value)
    {
        if (value is null
            || value.SessionId == Guid.Empty
            || value.PeerId == 0
            || value.LevelGeneration < 1
            || !IsWorldEntityIdText(value.EntityId))
        {
            throw new ProtocolException("The item-pickup request is invalid.");
        }
    }

    public static void Validate(ItemCollectedPayload? value)
    {
        if (value is null
            || value.SessionId == Guid.Empty
            || value.LevelGeneration < 1
            || value.CollectorPeerId == 0
            || !IsWorldEntityIdText(value.EntityId))
        {
            throw new ProtocolException("The item-collected payload is invalid.");
        }
    }

    public static void Validate(NativeItemSyncPickupPayload? value)
    {
        if (value is null
            || value.SchemaVersion != 1
            || value.Sequence == 0
            || !IsWorldEntityIdText(value.EntityId))
        {
            throw new ProtocolException("The native ItemSync pickup is invalid.");
        }
    }

    public static void Validate(ItemSpawnPayload? value)
    {
        if (value is null
            || value.SessionId == Guid.Empty
            || value.PeerId == 0
            || value.LevelGeneration < 1
            || !IsItemSpawn(value.Kind, value.OwnerKey, value.Spawn, value.Path,
                value.Quantity, value.Transform))
        {
            throw new ProtocolException("The item-spawn payload is invalid.");
        }
    }

    public static void Validate(NativeItemSyncSpawnPayload? value)
    {
        if (value is null
            || value.SchemaVersion != 1
            || value.Sequence == 0
            || !IsItemSpawn(value.Kind, value.OwnerKey, value.Spawn, value.Path,
                value.Quantity, value.Transform))
        {
            throw new ProtocolException("The native ItemSync spawn is invalid.");
        }
    }

    public const uint ItemDropOwnerInstanceTag = 0xA1D00000U;
    public const uint ItemDropOwnerInstanceMask = 0xFFF00000U;

    private static bool IsItemSpawn(
        string? kind, string? ownerKey, bool spawn, string? path, int quantity, float[]? transform)
    {
        if (ownerKey is not { Length: 16 }
            || !ulong.TryParse(ownerKey, NumberStyles.AllowHexSpecifier,
                CultureInfo.InvariantCulture, out var owner)
            || owner == 0
            || path is null
            || transform is not { Length: 12 })
            return false;
        var drop = string.Equals(kind, "drop", StringComparison.Ordinal);
        if (!drop && !string.Equals(kind, "owner", StringComparison.Ordinal)) return false;
        if (drop && (!spawn || ((uint)owner & ItemDropOwnerInstanceMask) != ItemDropOwnerInstanceTag))
            return false;
        if (!spawn) return path.Length == 0 && quantity == 0;
        return IsItemPath(path)
            && quantity is >= 0 and <= 100000
            && transform.All(value => float.IsFinite(value) && Math.Abs(value) <= 100000F);
    }

    public static string FormatItemTransform(IReadOnlyList<float> transform) =>
        string.Join(",", transform.Select(value => value.ToString("R", CultureInfo.InvariantCulture)));

    public static bool TryParseItemTransform(string? value, out float[] transform)
    {
        transform = Array.Empty<float>();
        var parts = value?.Split(',');
        if (parts is not { Length: 12 }) return false;
        var parsed = new float[12];
        for (var index = 0; index < parts.Length; index++)
        {
            if (!float.TryParse(parts[index], NumberStyles.Float, CultureInfo.InvariantCulture,
                    out parsed[index]))
                return false;
        }
        transform = parsed;
        return true;
    }

    // Item definition composite path ("Pickups\..."), printable ASCII.
    public static bool IsItemPath(string? value) =>
        value is { Length: > 0 and < 96 }
        && value.All(character => character is >= ' ' and <= '~');

    // 32 uppercase or lowercase hex digits (WorldEntityId High:X16 + Low:X16),
    // never the all-zero id.
    public static bool IsWorldEntityIdText(string? value) =>
        value is { Length: 32 }
        && value.All(Uri.IsHexDigit)
        && value.Any(character => character != '0');

    public static void Validate(PlayerSpawnPositionPayload? value)
    {
        if (value is null || value.PeerId == 0 || !ValidSpawnCoordinate(value.X)
            || !ValidSpawnCoordinate(value.Y) || !ValidSpawnCoordinate(value.Z)
            || (value.Character is not null && !PlayerCharacterCatalog.IsKnown(value.Character)))
            throw new ProtocolException("The player spawn position is invalid.");
    }

    public static void Validate(SpawnPositionRequestPayload? value)
    {
        if (value is null || value.SessionId == Guid.Empty || value.RequestId == Guid.Empty
            || value.HostPeerId == 0 || value.LevelGeneration < 1)
            throw new ProtocolException("The spawn-position request is invalid.");
    }

    public static void Validate(SpawnPositionResponsePayload? value)
    {
        if (value is null || value.SessionId == Guid.Empty || value.RequestId == Guid.Empty
            || value.PeerId == 0 || value.LevelGeneration < 1)
            throw new ProtocolException("The spawn-position response is invalid.");
        Validate(new PlayerSpawnPositionPayload(value.PeerId, value.X, value.Y, value.Z));
    }

    public static void Validate(SpawnRemotePlayersPayload? value)
    {
        if (value is null || value.SessionId == Guid.Empty || value.RequestId == Guid.Empty
            || value.HostPeerId == 0 || value.LevelGeneration < 1 || value.Players is null
            || value.Players.Count is < 2 or > 32)
            throw new ProtocolException("The remote-player spawn request is invalid.");
        foreach (var player in value.Players) Validate(player);
        var characters = value.Players.Where(player => player.Character is not null).Select(player => player.Character);
        if (value.Players.Select(player => player.PeerId).Distinct().Count() != value.Players.Count
            || value.Players.All(player => player.PeerId != value.HostPeerId)
            || characters.Distinct(StringComparer.Ordinal).Count() != characters.Count())
            throw new ProtocolException("The remote-player spawn roster is invalid.");
    }

    public static void Validate(PlayerPositionSnapshotPayload? value)
    {
        if (value is null || value.SessionId == Guid.Empty || value.PeerId == 0
            || value.LevelGeneration < 1 || value.Sequence == 0
            || value.SentAtUnixMilliseconds <= 0 || !ValidSpawnCoordinate(value.X)
            || !ValidSpawnCoordinate(value.Y) || !ValidSpawnCoordinate(value.Z)
            || !ValidYaw(value.Yaw)
            || (int)value.MoveType is < 0 or > 3
            || !float.IsFinite(value.NativeMoveSpeed)
            || value.NativeMoveSpeed is < 0.05F or > 8.0F)
            throw new ProtocolException("The player-position snapshot is invalid.");
        if (value.Posture is not null) Validate(value.Posture);
        if (value.Torch is not null) Validate(value.Torch);
        if (value.Hiding is not null) Validate(value.Hiding);
        if (value.Animation is not null && !PlayerAnimationCodec.IsValid(value.Animation))
            throw new ProtocolException("The player-animation snapshot is invalid.");
    }

    public static void Validate(PlayerHidingSnapshot? value)
    {
        if (value is null || !PlayerHidingCodec.IsValid(value))
            throw new ProtocolException("The player-hiding snapshot is invalid.");
    }

    public static void Validate(PlayerTorchSnapshot? value)
    {
        if (value is null || !PlayerTorchCodec.IsValid(value))
            throw new ProtocolException("The player-torch snapshot is invalid.");
    }

    public static void Validate(PlayerPostureSnapshot? value)
    {
        if (value is null || !PlayerPostureCodec.IsValid(value))
            throw new ProtocolException("The player-posture snapshot is invalid.");
    }

    public static void Validate(NativePlayerEquipmentStatePayload? value)
    {
        if (value is null || value.SchemaVersion != 2 || value.Revision == 0
            || value.CurrentEquipmentIndex is < -1 or > 64
            || value.PreviousEquipmentIndex is < -1 or > 64
            || value.PresentationState is < -1 or > 64)
            throw new ProtocolException("The native player-equipment state is invalid.");
    }

    public static void Validate(PlayerEquipmentSnapshotPayload? value)
    {
        if (value is null || value.SessionId == Guid.Empty || value.PeerId == 0
            || value.LevelGeneration < 1 || value.Sequence == 0
            || value.SentAtUnixMilliseconds <= 0
            || value.CurrentEquipmentIndex is < -1 or > 64
            || value.PreviousEquipmentIndex is < -1 or > 64
            || value.PresentationState is < 0 or > 64)
            throw new ProtocolException("The player-equipment snapshot is invalid.");
    }

    public static void Validate(NativePlayerActionEventPayload? value)
    {
        if (value is null || value.SchemaVersion != 1 || value.EventSequence == 0
            || (int)value.Action is < 0 or > 2
            || (value.Action == PlayerActionKind.Reload && !value.Active)
            || value.CurrentEquipmentIndex is < -1 or > 64
            || value.PresentationState is < -1 or > 64)
            throw new ProtocolException("The native player-action event is invalid.");
    }

    public static void Validate(PlayerActionEventPayload? value)
    {
        if (value is null || value.SessionId == Guid.Empty || value.PeerId == 0
            || value.LevelGeneration < 1 || value.Sequence == 0
            || value.SentAtUnixMilliseconds <= 0
            || (int)value.Action is < 0 or > 2
            || (value.Action == PlayerActionKind.Reload && !value.Active)
            || value.CurrentEquipmentIndex is < -1 or > 64
            || value.PresentationState is < 0 or > 64)
            throw new ProtocolException("The player-action event is invalid.");
    }

    public static void Validate(CinematicSkipPayload? value)
    {
        if (value is null || value.SessionId==Guid.Empty
            || value.LevelGeneration<1 || value.Sequence==0UL)
            throw new ProtocolException("The Host cinematic-skip decision is invalid.");
    }

    public static void Validate(LobbyPreparationPayload? value)
    {
        if (value is null || value.SessionId == Guid.Empty || value.PeerId == 0
            || value.PreparationToken == Guid.Empty || !Bounded(value.Level, 1, 128)
            || value.Error is null || value.Error.Length > 2048
            || value.ManifestSha256 is null || value.ManifestSha256.Length > 64
            || value.CampaignSaveSha256 is null
            || (value.CampaignSaveSha256.Length != 0 && !HexDigest(value.CampaignSaveSha256)))
            throw new ProtocolException("Lobby preparation is invalid.");
        if (value.Ready)
        {
            if (value.Manifest is not { IsValid: true } || !HexDigest(value.ManifestSha256)
                || value.Error.Length != 0)
                throw new ProtocolException("Ready lobby preparation must provide valid manifest identity and digest.");
        }
        else if (value.Manifest is { } manifest && !manifest.IsValid)
            throw new ProtocolException("Lobby preparation manifest identity is invalid.");
    }

    public static void Validate(SessionPreparedPayload? value)
    {
        if (value is null || value.SessionId == Guid.Empty || value.PeerId == 0
            || value.LevelGeneration < 1 || value.MissionSha256 is null
            || (value.MissionSha256.Length != 0 && !HexDigest(value.MissionSha256)))
            throw new ProtocolException("Session preparation is invalid.");
    }

    public static void Validate(SessionReleasePayload? value)
    {
        if (value is null || value.SessionId == Guid.Empty || value.LevelGeneration < 1)
            throw new ProtocolException("Session release is invalid.");
    }

    public static void Validate(LobbyStartPayload? value)
    {
        if (value?.Campaign is { } campaignLaunch) CampaignSaveValidation.Validate(campaignLaunch);
        if (value is null
            || value.SessionId == Guid.Empty
            || !Bounded(value.Level, 1, 128)
            || value.LevelGeneration < 1
            || value.StartAtUnixMilliseconds <= 0
            || (value.AlienVsSurvivors && (!value.AlienEnabled || value.AlienPeerId == 0))
            || (!value.AlienVsSurvivors && value.AlienPeerId != 0)
            || value.PreviousMissionSequence > MissionSignalValidation.MaximumSignalsPerGeneration
            || (!value.CampaignContinuation && value.PreviousMissionSequence != 0)
            || (value.CampaignContinuation && (value.LevelGeneration < 2 || value.AlienVsSurvivors)))
        {
            throw new ProtocolException("The launch command is invalid.");
        }
    }

    public static void Validate(LobbySnapshotPayload? value)
    {
        if (value?.Campaign is { } campaignLaunch) CampaignSaveValidation.Validate(campaignLaunch);
        if (value is null
            || value.SessionId == Guid.Empty
            || !Bounded(value.ServerName, 1, ProtocolConstants.MaximumServerNameLength)
            || !Bounded(value.Level, 1, 128)
            || !Bounded(value.State, 1, 32)
            || value.Capacity is < 1 or > 32
            || value.LevelGeneration < 0
            || value.Players is null
            || value.Players.Count is < 1
            || value.Players.Count > value.Capacity
            || (value.AlienVsSurvivors && !value.AlienEnabled)
            || (!value.AlienVsSurvivors && value.AlienPeerId != 0))
        {
            throw new ProtocolException("The lobby snapshot is invalid.");
        }

        if (value.WorldPreparationRequired && value.PreparationToken == Guid.Empty)
            throw new ProtocolException("The lobby preparation token is missing.");
        if (value.Preparations is { } preparations)
        {
            if (preparations.Count > value.Capacity)
                throw new ProtocolException("Too many lobby preparation reports.");
            var reportedPeers = new HashSet<ulong>();
            foreach (var preparation in preparations)
            {
                Validate(preparation);
                if (preparation.SessionId != value.SessionId
                    || preparation.PreparationToken != value.PreparationToken
                    || !string.Equals(preparation.Level, value.Level, StringComparison.OrdinalIgnoreCase)
                    || !reportedPeers.Add(preparation.PeerId))
                    throw new ProtocolException("Lobby preparation report is stale or duplicated.");
            }
        }

        var peerIds = new HashSet<ulong>();
        var launcherIds = new HashSet<Guid>();
        var characters = new HashSet<string>(StringComparer.Ordinal);
        var hostCount = 0;
        foreach (var player in value.Players)
        {
            if (player is null
                || player.PeerId == 0
                || player.LauncherPlayerId == Guid.Empty
                || !PlayerName.TryNormalize(player.DisplayName, out _, out _)
                || !peerIds.Add(player.PeerId)
                || !launcherIds.Add(player.LauncherPlayerId))
            {
                throw new ProtocolException("A lobby player is invalid or duplicated.");
            }
            if (player.Character is not null
                && (!PlayerCharacterCatalog.IsKnown(player.Character) || !characters.Add(player.Character)))
                throw new ProtocolException("A lobby character is invalid or duplicated.");

            hostCount += player.IsHost ? 1 : 0;
        }

        if (value.AlienPeerId != 0 && !peerIds.Contains(value.AlienPeerId) && value.State == "Lobby")
            throw new ProtocolException("Alien player is not a lobby participant.");
        if (value.AlienVsSurvivors && value.State == "Starting" && value.AlienPeerId == 0)
            throw new ProtocolException("PvP round cannot start without an assigned Alien.");

        if (hostCount != 1)
        {
            throw new ProtocolException("The snapshot must contain exactly one host.");
        }
    }

    public static void Validate(ErrorPayload? value)
    {
        if (value is null
            || !Bounded(value.Code, 1, 64)
            || !Bounded(value.Message, 1, 1024))
        {
            throw new ProtocolException("The error payload is invalid.");
        }
    }

    public static void Validate(WorldTransactionRecoveryRequest? value)
    {
        if (value is null || value.SessionId == Guid.Empty || value.PeerId == 0
            || value.WorldGeneration <= 0)
            throw new ProtocolException("The world transaction recovery request is invalid.");
    }

    public static void Validate(WorldTransactionRecoveryStatus? value)
    {
        if (value is null || value.SessionId == Guid.Empty || value.PeerId == 0
            || value.WorldGeneration <= 0
            || !Enum.IsDefined(value.Status)
            || (value.Status == WorldTransactionRecoveryStatusCode.Complete
                && value.HostThroughSequence < value.RequestedAfterSequence)
            || !Bounded(value.Reason, 1, 256))
            throw new ProtocolException("The world transaction recovery status is invalid.");
    }

    public static void Validate(SessionConfigPayload? value)
    {
        if (value is null
            || value.SessionId == Guid.Empty
            || value.PeerId == 0
            || value.LauncherPlayerId == Guid.Empty
            || !PlayerName.TryNormalize(value.DisplayName, out _, out _)
            || !ValidUtf8Text(value.HostAddress, 255, 255)
            || value.HostPort is < 1 or > 65535
            || value.LevelGeneration < 0)
        {
            throw new ProtocolException("The session configuration is invalid.");
        }
    }

    public static void Validate(WorldSyncLifecyclePayload? value)
    {
        if (value is null
            || value.SessionId == Guid.Empty
            || value.PeerId == 0
            || value.LevelGeneration < 0
            || (value.WorldBusActive && !value.AuthorityBound)
            || !Bounded(value.Reason, 1, 128))
        {
            throw new ProtocolException("The WorldSync lifecycle binding is invalid.");
        }
    }

    public static void Validate(TestModeConfigurePayload? value)
    {
        ValidateExperimentalEnvelope(
            value?.SchemaVersion ?? 0,
            value?.RequestId ?? Guid.Empty,
            value?.LevelGeneration ?? -1,
            value?.PortableDummyId ?? Guid.Empty);
    }

    public static void Validate(CapabilityQueryPayload? value)
    {
        ValidateExperimentalEnvelope(
            value?.SchemaVersion ?? 0,
            value?.RequestId ?? Guid.Empty,
            value?.LevelGeneration ?? -1,
            value?.RuntimeIdentity ?? Guid.Empty);
    }

    public static void Validate(CapabilityStatePayload? value)
    {
        ValidateExperimentalEnvelope(
            value?.SchemaVersion ?? 0,
            value?.RequestId ?? Guid.Empty,
            value?.LevelGeneration ?? -1,
            value?.RuntimeIdentity ?? Guid.Empty);
        if (value is null
            || !Bounded(value.ProfileId, 0, 128)
            || !BuildResolutionModes.All.Contains(value.ResolutionMode)
            || !Bounded(value.NextAction, 1, ProtocolConstants.MaximumExperimentalTextLength)
            || value.Capabilities is null
            || value.Capabilities.Count > NativeCapabilityNames.All.Count
            || value.BlockedGates is null)
        {
            throw new ProtocolException("The experimental capability state is invalid.");
        }

        var capabilityNames = new HashSet<string>(StringComparer.Ordinal);
        foreach (var capability in value.Capabilities)
        {
            if (capability is null
                || !NativeCapabilityNames.All.Contains(capability.Name)
                || !CapabilityStateNames.All.Contains(capability.State)
                || !Bounded(capability.Reason, 0, 512)
                || !capabilityNames.Add(capability.Name))
            {
                throw new ProtocolException("A native capability is invalid or duplicated.");
            }
        }

        ValidateBoundedUniqueList(value.BlockedGates, "blocked gates");
    }

    public static void Validate(CaptureStatePayload? value)
    {
        ValidateExperimentalEnvelope(
            value?.SchemaVersion ?? 0,
            value?.RequestId ?? Guid.Empty,
            value?.LevelGeneration ?? -1,
            value?.RuntimeIdentity ?? Guid.Empty);
        if (value is null
            || value.ParticipantCount is < 0 or > 8
            || value.CaptureCatalogCount is < 0 or > 256
            || value.GameThreadCandidateId < 0
            || value.GameThreadSourceCount is < 0 or > 32
            || value.EngineThreadId < 0
            || value.PendingCommandCount is < 0 or > 128
            || !Bounded(value.EngineThreadSource, 1, 64)
            || !Bounded(value.LastOperation, 0, 512)
            || !ValidRemoteCharacterState(value))
        {
            throw new ProtocolException("The experimental capture state is invalid.");
        }
    }

    private static bool ValidRemoteCharacterState(CaptureStatePayload value)
    {
        if (!RemoteCharacterStateNames.All.Contains(value.RemoteCharacterState)) return false;
        if ((value.RemoteCharacterCreationSerial == 0) != (value.RemoteCharacterRuntimeId == 0)
            || value.RemoteCharacterCreationSerial != 0 &&
                (value.RemoteCharacterState != RemoteCharacterStateNames.Ready || value.LevelGeneration <= 0)) return false;
        var hasKey = value.RemoteCharacterSpawnKey != 0;
        var hasCharacter = ValidCharacterAddress(value.RemoteCharacterAddress);
        var hasRecord = ValidNativeAddress(value.RemoteCharacterRecordAddress);
        return value.RemoteCharacterState switch
        {
            RemoteCharacterStateNames.None => !hasKey && !hasCharacter && !hasRecord,
            RemoteCharacterStateNames.Pending => hasKey && !hasCharacter && !hasRecord,
            RemoteCharacterStateNames.Ready => hasKey && hasCharacter && hasRecord,
            RemoteCharacterStateNames.Failed => hasKey && !hasCharacter && !hasRecord,
            _ => false
        };
    }

    private static bool ValidCharacterAddress(ulong value) =>
        ValidNativeAddress(value) && (value & 0x0FUL) == 0;

    private static bool ValidNativeAddress(ulong value) =>
        value is >= 0x10000 and <= uint.MaxValue;

    public static void Validate(DummyIdentityPayload? value)
    {
        if (value is null
            || value.PortableDummyId == Guid.Empty
            || value.PeerId == 0
            || value.LauncherPlayerId == Guid.Empty
            || value.PortableBindingId == Guid.Empty
            || !PlayerName.TryNormalize(value.DisplayName, out var normalized, out _)
            || !string.Equals(value.DisplayName, normalized, StringComparison.Ordinal))
        {
            throw new ProtocolException("The portable Dummy identity is invalid.");
        }
    }

    public static void Validate(DummyCreatePayload? value)
    {
        ValidateExperimentalEnvelope(
            value?.SchemaVersion ?? 0,
            value?.RequestId ?? Guid.Empty,
            value?.LevelGeneration ?? -1,
            value?.PortableDummyId ?? Guid.Empty);
        if (value is null || value.Identity is null || value.SpawnOffset is null)
        {
            throw new ProtocolException("The Dummy creation command is incomplete.");
        }

        Validate(value.Identity);
        if (value.Identity.PortableDummyId != value.PortableDummyId
            || !FiniteAndBounded(value.SpawnOffset.X, 10.0F)
            || !FiniteAndBounded(value.SpawnOffset.Y, 10.0F)
            || !FiniteAndBounded(value.SpawnOffset.Z, 10.0F))
        {
            throw new ProtocolException("The Dummy creation offset or identity is invalid.");
        }
    }

    public static void Validate(DummyRemovePayload? value)
    {
        ValidateExperimentalEnvelope(
            value?.SchemaVersion ?? 0,
            value?.RequestId ?? Guid.Empty,
            value?.LevelGeneration ?? -1,
            value?.PortableDummyId ?? Guid.Empty);
    }

    public static void Validate(DummyOperationResultPayload? value)
    {
        ValidateExperimentalEnvelope(
            value?.SchemaVersion ?? 0,
            value?.RequestId ?? Guid.Empty,
            value?.LevelGeneration ?? -1,
            value?.PortableDummyId ?? Guid.Empty);
        if (value is null
            || !DummyStateNames.All.Contains(value.State)
            || !DummyAchievementLevels.All.Contains(value.AchievedLevel)
            || !Identifier(value.StatusCode)
            || !Bounded(value.Message, 1, ProtocolConstants.MaximumExperimentalTextLength)
            || value.CompletedSteps is null
            || value.BlockedGates is null)
        {
            throw new ProtocolException("The Dummy operation result is invalid.");
        }

        ValidateBoundedUniqueList(value.CompletedSteps, "Dummy steps");
        ValidateBoundedUniqueList(value.BlockedGates, "Dummy gates");

        if ((string.Equals(value.State, DummyStateNames.Active, StringComparison.Ordinal)
                && string.Equals(
                    value.AchievedLevel,
                    DummyAchievementLevels.None,
                    StringComparison.Ordinal))
            || (string.Equals(value.State, DummyStateNames.Absent, StringComparison.Ordinal)
                && !string.Equals(
                    value.AchievedLevel,
                    DummyAchievementLevels.None,
                    StringComparison.Ordinal)))
        {
            throw new ProtocolException(
                "Dummy state and achieved level are inconsistent.");
        }
    }

    public static void Validate(DummyMovePayload? value)
    {
        ValidateExperimentalEnvelope(
            value?.SchemaVersion ?? 0,
            value?.RequestId ?? Guid.Empty,
            value?.LevelGeneration ?? -1,
            value?.PortableDummyId ?? Guid.Empty);
        if (value is null
            || !DummyMoveCommands.All.Contains(value.Command)
            || !FiniteAndBounded(value.Amount, 180.0F)
            || value.DurationMilliseconds is < 0 or > 10_000)
        {
            throw new ProtocolException("The Dummy movement command is invalid.");
        }
    }

    public static void Validate(DummyMoveResultPayload? value)
    {
        ValidateExperimentalEnvelope(
            value?.SchemaVersion ?? 0,
            value?.RequestId ?? Guid.Empty,
            value?.LevelGeneration ?? -1,
            value?.PortableDummyId ?? Guid.Empty);
        if (value is null
            || !Identifier(value.StatusCode)
            || !Bounded(value.Message, 1, ProtocolConstants.MaximumExperimentalTextLength)
            || !DummyMovementMethods.All.Contains(value.Method)
            || (value.Applied
                == string.Equals(
                    value.Method,
                    DummyMovementMethods.NotApplied,
                    StringComparison.Ordinal)))
        {
            throw new ProtocolException("The Dummy movement result is invalid.");
        }
    }

    public static void Validate(DummyBindingStatePayload? value)
    {
        ValidateExperimentalEnvelope(
            value?.SchemaVersion ?? 0,
            value?.RequestId ?? Guid.Empty,
            value?.LevelGeneration ?? -1,
            value?.PortableDummyId ?? Guid.Empty);
        if (value is null
            || !DummyAchievementLevels.All.Contains(value.AchievedLevel)
            || !Bounded(value.LastError, 0, ProtocolConstants.MaximumExperimentalTextLength))
        {
            throw new ProtocolException("The Dummy binding state is invalid.");
        }

        var anyBindingState = value.ParticipantBound
            || value.CharacterPresent
            || value.CharacterRecordPresent
            || value.ChannelActive
            || value.DescriptorsPresent
            || value.ReplicationContextPresent;
        var completeBinding = value.ParticipantBound
            && value.CharacterPresent
            && value.CharacterRecordPresent
            && value.ChannelActive
            && value.DescriptorsPresent
            && value.ReplicationContextPresent;
        var isNone = string.Equals(
            value.AchievedLevel,
            DummyAchievementLevels.None,
            StringComparison.Ordinal);
        var isNativeParticipant = string.Equals(
                value.AchievedLevel,
                DummyAchievementLevels.C,
                StringComparison.Ordinal)
            || string.Equals(
                value.AchievedLevel,
                DummyAchievementLevels.D,
                StringComparison.Ordinal);
        if ((isNone && anyBindingState)
            || (isNativeParticipant && !completeBinding))
        {
            throw new ProtocolException(
                "Achieved level and Dummy binding state are inconsistent.");
        }
    }

    public static void Validate(DiagnosticSnapshotRequestPayload? value)
    {
        ValidateExperimentalEnvelope(
            value?.SchemaVersion ?? 0,
            value?.RequestId ?? Guid.Empty,
            value?.LevelGeneration ?? -1,
            value?.RuntimeIdentity ?? Guid.Empty);
    }

    public static void Validate(DiagnosticSnapshotResultPayload? value)
    {
        ValidateExperimentalEnvelope(
            value?.SchemaVersion ?? 0,
            value?.RequestId ?? Guid.Empty,
            value?.LevelGeneration ?? -1,
            value?.RuntimeIdentity ?? Guid.Empty);
        if (value is null
            || !Identifier(value.StatusCode)
            || !Bounded(value.Message, 1, ProtocolConstants.MaximumExperimentalTextLength)
            || !Bounded(value.RunId, 0, 128)
            || !PathText(value.NativeLogPath)
            || !PathText(value.ProfilePath)
            || !PathText(value.CaptureDirectory)
            || !PathText(value.DumpPath)
            || value.ArtifactPaths is null
            || value.ArtifactPaths.Count > ProtocolConstants.MaximumExperimentalListItems
            || value.ArtifactPaths.Any(path => !PathText(path)))
        {
            throw new ProtocolException("The diagnostic snapshot is invalid.");
        }
    }

    public static void Validate(ExperimentalErrorPayload? value)
    {
        ValidateExperimentalEnvelope(
            value?.SchemaVersion ?? 0,
            value?.RequestId ?? Guid.Empty,
            value?.LevelGeneration ?? -1,
            value?.RuntimeIdentity ?? Guid.Empty);
        if (value is null
            || !Identifier(value.Code)
            || !Bounded(value.Message, 1, ProtocolConstants.MaximumExperimentalTextLength)
            || value.BlockedGates is null)
        {
            throw new ProtocolException("The experimental error is invalid.");
        }

        ValidateBoundedUniqueList(value.BlockedGates, "error gates");
    }

    public static void Validate(LevelStatePayload? value)
    {
        ValidateExperimentalEnvelope(
            value?.SchemaVersion ?? 0,
            value?.RequestId ?? Guid.Empty,
            value?.LevelGeneration ?? -1,
            value?.RuntimeIdentity ?? Guid.Empty);
        if (value is null || !Bounded(value.LevelName, 0, 256))
        {
            throw new ProtocolException("The level state is invalid.");
        }
    }

    public static void Validate(GameCommandExecutePayload? value)
    {
        ValidateExperimentalEnvelope(
            value?.SchemaVersion ?? 0,
            value?.RequestId ?? Guid.Empty,
            value?.LevelGeneration ?? -1,
            value?.RuntimeIdentity ?? Guid.Empty);
        if (value is null
            || !GameCommandNames.All.Contains(value.Command)
            || value.Arguments is null
            || value.Arguments.Count > 17)
        {
            throw new ProtocolException("The game command is invalid.");
        }


        if (string.Equals(
                value.Command,
                GameCommandNames.WorldInteractionStats,
                StringComparison.Ordinal)
            || string.Equals(
                value.Command,
                GameCommandNames.WorldCompositeInputStats,
                StringComparison.Ordinal)
            || string.Equals(
                value.Command,
                GameCommandNames.WorldStateDiscoveryBaseline,
                StringComparison.Ordinal)
            || string.Equals(
                value.Command,
                GameCommandNames.WorldStateDiscoveryAfter,
                StringComparison.Ordinal)
            || string.Equals(
                value.Command,
                GameCommandNames.WorldStateDiscoveryStatus,
                StringComparison.Ordinal)
            || string.Equals(
                value.Command,
                GameCommandNames.AlienPvPProbe,
                StringComparison.Ordinal)
            || string.Equals(
                value.Command,
                GameCommandNames.AlienPvPRelease,
                StringComparison.Ordinal)
            || string.Equals(
                value.Command,
                GameCommandNames.ReturnToFrontend,
                StringComparison.Ordinal)
            || value.Command is GameCommandNames.CinematicSyncStatus
                or GameCommandNames.CinematicSyncSkip)
        {
            if (value.Arguments.Count != 0)
                throw new ProtocolException($"{value.Command} parameters are invalid.");
            return;
        }

        if(value.Command==GameCommandNames.AlienPvPVents) {
            if(value.LevelGeneration<1 || value.Arguments.Count!=1 ||
                !value.Arguments.TryGetValue("page",out var pageText) ||
                !uint.TryParse(pageText,NumberStyles.None,CultureInfo.InvariantCulture,out var page) || page>=64)
                throw new ProtocolException("Invalid Alien vent catalogue page.");
            return;
        }
        if (string.Equals(value.Command, GameCommandNames.AlienPvPBind,
                StringComparison.Ordinal))
        {
            if (value.LevelGeneration < 1 || value.Arguments.Count != 1 ||
                !value.Arguments.TryGetValue("ownerPeer", out var peerText) ||
                !ulong.TryParse(peerText, NumberStyles.None, CultureInfo.InvariantCulture,
                    out var ownerPeer) || ownerPeer == 0UL)
                throw new ProtocolException("ALIEN_PVP_BIND parameters are invalid.");
            return;
        }

        if (string.Equals(value.Command, GameCommandNames.SessionRoundBoundary,
                StringComparison.Ordinal))
        {
            if (value.LevelGeneration != 0 || value.Arguments.Count != 2
                || !value.Arguments.TryGetValue("sessionId", out var boundaryText)
                || !Guid.TryParse(boundaryText, out var boundarySession) || boundarySession == Guid.Empty
                || !value.Arguments.TryGetValue("generation", out var boundaryGenerationText)
                || !int.TryParse(boundaryGenerationText, NumberStyles.None, CultureInfo.InvariantCulture,
                    out var boundaryGeneration) || boundaryGeneration < 1)
                throw new ProtocolException("SESSION_ROUND_BOUNDARY parameters are invalid.");
            return;
        }

        if (value.Command == GameCommandNames.CampaignProfile)
        {
            if (value.LevelGeneration != 0 || value.Arguments.Count != 3
                || !value.Arguments.TryGetValue("action", out var profileAction)
                || profileAction is not ("configure" or "status" or "clear")
                || !value.Arguments.TryGetValue("sessionId", out var profileSessionText)
                || !Guid.TryParse(profileSessionText, out var profileSession) || profileSession == Guid.Empty
                || !value.Arguments.TryGetValue("path", out var profilePath)
                || profilePath.Length > 1024 || (profileAction == "configure" && string.IsNullOrWhiteSpace(profilePath)))
                throw new ProtocolException("CAMPAIGN_PROFILE parameters are invalid.");
            return;
        }

        if (string.Equals(value.Command, GameCommandNames.SessionLoadingHold,
                StringComparison.Ordinal))
        {
            if (value.LevelGeneration != 0 || value.Arguments.Count != 3
                || !value.Arguments.TryGetValue("active", out var active) || active is not ("0" or "1")
                || !value.Arguments.TryGetValue("sessionId", out var sessionText)
                || !Guid.TryParse(sessionText, out var sessionId) || sessionId == Guid.Empty
                || !value.Arguments.TryGetValue("generation", out var generationText)
                || !int.TryParse(generationText, NumberStyles.None,
                    CultureInfo.InvariantCulture, out var generation) || generation < 1)
                throw new ProtocolException("SESSION_LOADING_HOLD parameters are invalid.");
            return;
        }

        if (string.Equals(value.Command, GameCommandNames.WorldWs3EmptyCallbackSet,
                StringComparison.Ordinal))
        {
            if (value.LevelGeneration != 0 || value.Arguments.Count != 1
                || !value.Arguments.TryGetValue("enabled", out var enabled)
                || (enabled != "0" && enabled != "1"))
                throw new ProtocolException("WORLD_WS3_EMPTY_CALLBACK_SET parameters are invalid.");
            return;
        }

        if (string.Equals(value.Command, GameCommandNames.PerformanceIsolationSet,
                StringComparison.Ordinal))
        {
            if (value.LevelGeneration != 0 || value.Arguments.Count != 2
                || !value.Arguments.TryGetValue("module", out var module)
                || !PerformanceIsolationNativeModules.All.Contains(module)
                || !value.Arguments.TryGetValue("enabled", out var enabled)
                || (enabled != "0" && enabled != "1"))
                throw new ProtocolException("PERFORMANCE_ISOLATION_SET parameters are invalid.");
            return;
        }



        if (value.Arguments.Count == 0)
            throw new ProtocolException("The game command is invalid.");

        if (string.Equals(
                value.Command,
                GameCommandNames.WorldManifestLoad,
                StringComparison.Ordinal))
        {
            if (value.Arguments.Count != 1
                || !value.Arguments.TryGetValue("manifestPath", out var manifestPath)
                || string.IsNullOrWhiteSpace(manifestPath)
                || manifestPath.Length > 1024
                || manifestPath.IndexOfAny(new[] { '\0', '\r', '\n' }) >= 0)
                throw new ProtocolException("WORLD_MANIFEST_LOAD parameters are invalid.");
            return;
        }

        if (string.Equals(
                value.Command,
                GameCommandNames.WorldNetProbe,
                StringComparison.Ordinal))
        {
            if (value.Arguments.Count != 1
                || !value.Arguments.TryGetValue("entityId", out var entityId)
                || entityId.Length != 32
                || !entityId.All(Uri.IsHexDigit)
                || entityId.All(value => value == '0'))
                throw new ProtocolException("WORLD_NET_PROBE parameters are invalid.");
            return;
        }

        if (value.Command == GameCommandNames.MissionSyncConfigure)
        {
            if (value.Arguments.Count != 2 || !value.Arguments.TryGetValue("role", out var role)
                || role is not ("host" or "client" or "off")
                || !value.Arguments.TryGetValue("path", out var path) || path.Length > 1024
                || path.Contains('\0') || (role != "off" && string.IsNullOrWhiteSpace(path)))
                throw new ProtocolException("MISSION_SYNC_CONFIG parameters are invalid.");
            return;
        }
        if (value.Command == GameCommandNames.MissionSyncApply)
        {
            // Must match AIMP.Native ExperimentalProtocol: ordered occurrences
            // added a fifth argument and selectors 3/4 (scheduled inputs).
            if (value.Arguments.Count != 5 || !value.Arguments.TryGetValue("entityId", out var entity)
                || !IsWorldEntityIdText(entity) || !value.Arguments.TryGetValue("signal", out var port)
                || !uint.TryParse(port, NumberStyles.None, CultureInfo.InvariantCulture, out var signal) || signal == 0
                || !value.Arguments.TryGetValue("sequence", out var text)
                || !ulong.TryParse(text, NumberStyles.None, CultureInfo.InvariantCulture, out var sequence) || sequence == 0
                || !value.Arguments.TryGetValue("occurrence", out var occurrenceText)
                || !uint.TryParse(occurrenceText, NumberStyles.None, CultureInfo.InvariantCulture, out var occurrence)
                || occurrence > 1_000_000U
                || !value.Arguments.TryGetValue("selector", out var selector) || selector is not ("1" or "2" or "3" or "4"))
                throw new ProtocolException("MISSION_SYNC_APPLY parameters are invalid.");
            return;
        }
        if (string.Equals(
                value.Command,
                GameCommandNames.ItemSyncSpawn,
                StringComparison.Ordinal))
        {
            var expected = new HashSet<string>(
                new[] { "kind", "ownerKey", "spawn", "path", "quantity", "transform" },
                StringComparer.Ordinal);
            if (value.Arguments.Count != expected.Count
                || value.Arguments.Keys.Any(key => !expected.Contains(key))
                || !value.Arguments.TryGetValue("spawn", out var spawnText)
                || spawnText is not ("0" or "1")
                || !int.TryParse(value.Arguments["quantity"], NumberStyles.None,
                    CultureInfo.InvariantCulture, out var quantity)
                || !TryParseItemTransform(value.Arguments["transform"], out var transform)
                || !IsItemSpawn(value.Arguments["kind"], value.Arguments["ownerKey"],
                    spawnText == "1", value.Arguments["path"], quantity, transform))
                throw new ProtocolException("ITEM_SYNC_SPAWN parameters are invalid.");
            return;
        }

        if (string.Equals(
                value.Command,
                GameCommandNames.ItemSyncApply,
                StringComparison.Ordinal))
        {
            if (value.Arguments.Count != 1
                || !value.Arguments.TryGetValue("entityId", out var entityId)
                || !IsWorldEntityIdText(entityId))
                throw new ProtocolException("ITEM_SYNC_APPLY parameters are invalid.");
            return;
        }

        if (string.Equals(
                value.Command,
                GameCommandNames.WorldRegistryProbe,
                StringComparison.Ordinal))
        {
            if (value.Arguments.Count != 1
                || !value.Arguments.TryGetValue("manifestPath", out var manifestPath)
                || string.IsNullOrWhiteSpace(manifestPath)
                || manifestPath.Length > 1024
                || manifestPath.IndexOfAny(new[] { '\0', '\r', '\n' }) >= 0)
            {
                throw new ProtocolException("WORLD_REGISTRY_PROBE parameters are invalid.");
            }
            return;
        }

        if (string.Equals(
                value.Command,
                GameCommandNames.PopupMessage,
                StringComparison.Ordinal))
        {
            var expected = new HashSet<string>(
                new[] { "headerText", "mainText", "duration", "icon", "soundEvent" },
                StringComparer.Ordinal);
            if (value.Arguments.Count != expected.Count
                || value.Arguments.Keys.Any(key => !expected.Contains(key))
                || !value.Arguments.TryGetValue("headerText", out var header)
                || !value.Arguments.TryGetValue("mainText", out var main)
                || !value.Arguments.TryGetValue("duration", out var durationText)
                || !value.Arguments.TryGetValue("icon", out var icon)
                || !value.Arguments.TryGetValue("soundEvent", out var soundEvent)
                || (!ValidCommandText(header, allowLineBreaks: false)
                    && !string.IsNullOrEmpty(header))
                || (!ValidCommandText(main, allowLineBreaks: true)
                    && !string.IsNullOrEmpty(main))
                || (string.IsNullOrEmpty(header) && string.IsNullOrEmpty(main))
                || !float.TryParse(
                    durationText,
                    NumberStyles.Float,
                    CultureInfo.InvariantCulture,
                    out var duration)
                || !float.IsFinite(duration)
                || duration is < 0.5F or > 120.0F
                || !PopupMessageIcons.All.Contains(icon)
                || !PopupMessageSoundEvents.All.Contains(soundEvent))
            {
                throw new ProtocolException("PopupMessage parameters are invalid.");
            }

            return;
        }

        if (string.Equals(
                value.Command,
                GameCommandNames.SpawnCharacter,
                StringComparison.Ordinal))
        {
            var expected = new HashSet<string>(
                new[] { "x", "y", "z" },
                StringComparer.Ordinal);
            // Optional crew representation; omitted means E_RIPLEY.
            var hasCharacter = value.Arguments.TryGetValue("character", out var characterText);
            if (value.Arguments.Count != expected.Count + (hasCharacter ? 1 : 0)
                || value.Arguments.Keys.Any(key => !expected.Contains(key) && key != "character")
                || (hasCharacter && !PlayerCharacterCatalog.IsKnown(characterText))
                || !value.Arguments.TryGetValue("x", out var xText)
                || !value.Arguments.TryGetValue("y", out var yText)
                || !value.Arguments.TryGetValue("z", out var zText)
                || !ValidCoordinate(xText)
                || !ValidCoordinate(yText)
                || !ValidCoordinate(zText))
            {
                throw new ProtocolException("SpawnCharacter parameters are invalid.");
            }

            return;
        }

        if (string.Equals(
                value.Command,
                GameCommandNames.CharacterDirectMove,
                StringComparison.Ordinal))
        {
            var required = new HashSet<string>(
                new[]
                {
                    "characterAddress", "directionX", "directionZ",
                    "intensity", "frameCount"
                },
                StringComparer.Ordinal);
            var allowed = new HashSet<string>(required, StringComparer.Ordinal)
            {
                "directionSpace", "replicationPositionX", "replicationPositionY",
                "replicationPositionZ",
                "replicationYaw",
                "moveType", "playerPosture", "playerTorch", "playerTorchDirection", "playerHiding", "playerHidingPresentation",
                "playerAnimation"
            };
            var hasDirectionSpace = value.Arguments.TryGetValue(
                "directionSpace", out var directionSpaceText);
            var hasReplicationPositionX = value.Arguments.TryGetValue(
                "replicationPositionX", out var replicationPositionXText);
            var hasReplicationPositionY = value.Arguments.TryGetValue(
                "replicationPositionY", out var replicationPositionYText);
            var hasReplicationPositionZ = value.Arguments.TryGetValue(
                "replicationPositionZ", out var replicationPositionZText);
            var hasReplicationYaw = value.Arguments.TryGetValue(
                "replicationYaw", out var replicationYawText);
            var hasMoveType = value.Arguments.TryGetValue(
                "moveType", out var moveTypeText);
            var hasReplicationPosition = hasReplicationPositionX
                && hasReplicationPositionY && hasReplicationPositionZ;
            var hasPosture = value.Arguments.TryGetValue("playerPosture", out var postureText);
            var hasTorch = value.Arguments.TryGetValue("playerTorch", out var torchText);
            var hasTorchDirection = value.Arguments.TryGetValue("playerTorchDirection", out var torchDirection);
            var hasHiding = value.Arguments.TryGetValue("playerHiding", out var hidingText);
            var hasHidingPresentation = value.Arguments.TryGetValue("playerHidingPresentation", out var presentationText);
            var hasAnimation = value.Arguments.TryGetValue("playerAnimation", out var animationText);
            var validCount = value.Arguments.Count is >= 5 and <= 17;
            if (!validCount
                || value.Arguments.Keys.Any(key => !allowed.Contains(key))
                || required.Any(key => !value.Arguments.ContainsKey(key))
                || hasReplicationPositionX != hasReplicationPositionY
                || hasReplicationPositionX != hasReplicationPositionZ
                || (hasReplicationYaw && !hasDirectionSpace)
                || (hasDirectionSpace
                    && !string.Equals(directionSpaceText, "local", StringComparison.Ordinal)
                    && !string.Equals(directionSpaceText, "world", StringComparison.Ordinal))
                || !value.Arguments.TryGetValue("characterAddress", out var addressText)
                || !value.Arguments.TryGetValue("directionX", out var directionXText)
                || !value.Arguments.TryGetValue("directionZ", out var directionZText)
                || !value.Arguments.TryGetValue("intensity", out var intensityText)
                || !value.Arguments.TryGetValue("frameCount", out var frameCountText)
                || !ValidCharacterAddress(addressText)
                || !float.TryParse(
                    directionXText,
                    NumberStyles.Float,
                    CultureInfo.InvariantCulture,
                    out var directionX)
                || !float.IsFinite(directionX)
                || Math.Abs(directionX) > MaximumLocomotionDirectionMagnitude
                || !float.TryParse(
                    directionZText,
                    NumberStyles.Float,
                    CultureInfo.InvariantCulture,
                    out var directionZ)
                || !float.IsFinite(directionZ)
                || Math.Abs(directionZ) > MaximumLocomotionDirectionMagnitude
                || !float.TryParse(
                    intensityText,
                    NumberStyles.Float,
                    CultureInfo.InvariantCulture,
                    out var intensity)
                || !float.IsFinite(intensity)
                || intensity is < 1.0F or > 4.0F
                || directionX * directionX + directionZ * directionZ >
                    MaximumLocomotionDirectionMagnitude *
                    MaximumLocomotionDirectionMagnitude
                || !uint.TryParse(
                    frameCountText,
                    NumberStyles.None,
                    CultureInfo.InvariantCulture,
                    out var frameCount)
                || frameCount > 3600U
                || (frameCount != 0U && directionX == 0.0F && directionZ == 0.0F)
                || (hasMoveType
                    && (!int.TryParse(moveTypeText, NumberStyles.Integer,
                            CultureInfo.InvariantCulture, out var moveType)
                        || moveType is < 0 or > 3))
                || (hasReplicationPosition && !hasMoveType)
                || (hasPosture && (!hasReplicationPosition
                    || !PlayerPostureCodec.TryDecode(postureText, out _)))
                || (hasTorch && (!hasReplicationPosition || torchText is not ("0" or "1")))
                || (hasTorchDirection && (!hasTorch || torchText != "1"
                    || !PlayerTorchCodec.TryDecodeDirection(torchDirection, out _)))
                || (hasHiding && (!hasReplicationPosition
                    || !PlayerHidingCodec.TryDecode(hidingText, out _)))
                || (hasHidingPresentation && (!hasHiding || (presentationText != "0"
                    && !PlayerHidingPresentationCodec.TryDecode(presentationText, out _, out _))))
                || (hasAnimation && (!hasReplicationPosition
                    || !PlayerAnimationCodec.TryDecode(animationText, out _)))
                || (hasReplicationYaw
                    && (!string.Equals(directionSpaceText, "world", StringComparison.Ordinal)
                        || !float.TryParse(replicationYawText, NumberStyles.Float,
                            CultureInfo.InvariantCulture, out var replicationYaw)
                        || !ValidYaw(replicationYaw)))
                || (hasReplicationPosition
                    && (!hasReplicationYaw
                        || !string.Equals(directionSpaceText, "world", StringComparison.Ordinal)
                        || !ValidCoordinate(replicationPositionXText!)
                        || !ValidCoordinate(replicationPositionYText!)
                        || !ValidCoordinate(replicationPositionZText!))))
            {
                throw new ProtocolException(
                    "CHARACTER_DIRECT_MOVE parameters are invalid.");
            }

            return;
        }

        if (string.Equals(value.Command, GameCommandNames.CharacterAmmoRefill,
                StringComparison.Ordinal))
        {
            if (value.Arguments.Count != 1
                || !value.Arguments.TryGetValue("characterAddress", out var addressText)
                || !ValidCharacterAddress(addressText))
            {
                throw new ProtocolException("CHARACTER_AMMO_REFILL parameters are invalid.");
            }
            return;
        }

        if (string.Equals(
                value.Command,
                GameCommandNames.CharacterActionTest,
                StringComparison.Ordinal))
        {
            var expected = new HashSet<string>(
                new[] { "characterAddress", "action" },
                StringComparer.Ordinal);
            if (value.Arguments.Count != expected.Count
                || value.Arguments.Keys.Any(key => !expected.Contains(key))
                || !value.Arguments.TryGetValue("characterAddress", out var addressText)
                || !value.Arguments.TryGetValue("action", out var action)
                || !ValidCharacterAddress(addressText)
                || action is not ("aim-start" or "aim-stop" or
                    "primary-start" or "primary-stop"))
            {
                throw new ProtocolException(
                    "CHARACTER_ACTION_TEST parameters are invalid.");
            }

            return;
        }

        if (string.Equals(
                value.Command,
                GameCommandNames.RemoteEquipmentProvision,
                StringComparison.Ordinal))
        {
            var activateHeld = value.Arguments.TryGetValue("activateHeld", out var activation);
            var releaseHeld = value.Arguments.TryGetValue("releaseHeld", out var release);
            var expected = new HashSet<string>(
                new[]
                {
                    "characterAddress",
                    "compositeName",
                    "equipmentEntityShortGuid",
                    "equipmentOutputShortGuid",
                    "spawnSchedulerParameterShortGuid",
                    "spawnTargetInputShortGuid",
                    "equipmentSlot", "activateHeld", "releaseHeld"
                },
                StringComparer.Ordinal);
            if (value.Arguments.Count != (activateHeld || releaseHeld ? 8 : 7)
                || value.Arguments.Keys.Any(key => !expected.Contains(key))
                || !value.Arguments.TryGetValue("characterAddress", out var addressText)
                || !value.Arguments.TryGetValue("compositeName", out var compositeName)
                || !value.Arguments.TryGetValue(
                    "equipmentEntityShortGuid", out var equipmentEntityText)
                || !value.Arguments.TryGetValue(
                    "equipmentOutputShortGuid", out var equipmentOutputText)
                || !value.Arguments.TryGetValue(
                    "spawnSchedulerParameterShortGuid", out var spawnSchedulerText)
                || !value.Arguments.TryGetValue(
                    "spawnTargetInputShortGuid", out var spawnTargetInputText)
                || !value.Arguments.TryGetValue("equipmentSlot", out var equipmentSlotText)
                || !ValidCharacterAddress(addressText)
                || string.IsNullOrWhiteSpace(compositeName)
                || Encoding.UTF8.GetByteCount(compositeName) > 255
                || !ValidNonZeroHexUInt32(equipmentEntityText)
                || !ValidNonZeroHexUInt32(equipmentOutputText)
                || !ValidNonZeroHexUInt32(spawnSchedulerText)
                || !ValidNonZeroHexUInt32(spawnTargetInputText)
                || !uint.TryParse(
                    equipmentSlotText,
                    NumberStyles.None,
                    CultureInfo.InvariantCulture,
                    out var equipmentSlot)
                || equipmentSlot > 10U
                || (activateHeld && releaseHeld)
                || (activateHeld && activation != "true")
                || (releaseHeld && release != "true")
                || ((activateHeld || releaseHeld) && (equipmentSlot != 9U || compositeName !=
                    @"Required_Assets\Archetypes\Equipment\Flare_Held")))
            {
                throw new ProtocolException(
                    "REMOTE_EQUIPMENT_PROVISION parameters are invalid.");
            }

            return;
        }

        if (string.Equals(
                value.Command,
                GameCommandNames.CharacterDirectStep,
                StringComparison.Ordinal))
        {
            var expected = new HashSet<string>(
                new[] { "characterAddress", "deltaX", "deltaY", "deltaZ", "deltaYaw", "frameCount" },
                StringComparer.Ordinal);
            if (value.Arguments.Count != expected.Count
                || value.Arguments.Keys.Any(key => !expected.Contains(key))
                || !value.Arguments.TryGetValue("characterAddress", out var addressText)
                || !value.Arguments.TryGetValue("deltaX", out var deltaXText)
                || !value.Arguments.TryGetValue("deltaY", out var deltaYText)
                || !value.Arguments.TryGetValue("deltaZ", out var deltaZText)
                || !value.Arguments.TryGetValue("deltaYaw", out var deltaYawText)
                || !value.Arguments.TryGetValue("frameCount", out var frameCountText)
                || !ValidCharacterAddress(addressText)
                || !ValidCharacterStep(deltaXText, 1.0F)
                || !ValidCharacterStep(deltaYText, 1.0F)
                || !ValidCharacterStep(deltaZText, 1.0F)
                || !ValidCharacterStep(deltaYawText, 1.0F)
                || !uint.TryParse(
                    frameCountText,
                    NumberStyles.None,
                    CultureInfo.InvariantCulture,
                    out var frameCount)
                || frameCount > 600U)
            {
                throw new ProtocolException(
                    "CHARACTER_DIRECT_STEP parameters are invalid.");
            }

            return;
        }

        if (string.Equals(
                value.Command,
                GameCommandNames.SpawnNetZCharacter,
                StringComparison.Ordinal))
        {
            var expected = new HashSet<string>(
                new[] { "x", "y", "z", "participantKey", "displayName" },
                StringComparer.Ordinal);
            if (value.Arguments.Count != expected.Count
                || value.Arguments.Keys.Any(key => !expected.Contains(key))
                || !value.Arguments.TryGetValue("x", out var xText)
                || !value.Arguments.TryGetValue("y", out var yText)
                || !value.Arguments.TryGetValue("z", out var zText)
                || !value.Arguments.TryGetValue("participantKey", out var participantKeyText)
                || !value.Arguments.TryGetValue("displayName", out var displayName)
                || !ValidCoordinate(xText)
                || !ValidCoordinate(yText)
                || !ValidCoordinate(zText)
                || !uint.TryParse(
                    participantKeyText,
                    NumberStyles.None,
                    CultureInfo.InvariantCulture,
                    out var participantKey)
                || participantKey == 0
                || participantKey > int.MaxValue
                || string.IsNullOrWhiteSpace(displayName)
                || !ValidCommandText(displayName, allowLineBreaks: false))
            {
                throw new ProtocolException("SpawnNetZCharacter parameters are invalid.");
            }

            return;
        }

        throw new ProtocolException("The game command is not supported.");
    }

    private static bool ValidCharacterAddress(string text)
    {
        if (text.StartsWith("0x", StringComparison.OrdinalIgnoreCase))
            text = text[2..];
        return uint.TryParse(
                text,
                NumberStyles.HexNumber,
                CultureInfo.InvariantCulture,
                out var address)
            && address >= 0x10000U
            && (address & 0x0FU) == 0U;
    }

    private static bool ValidNonZeroHexUInt32(string text)
    {
        if (string.IsNullOrWhiteSpace(text)) return false;
        if (text.StartsWith("0x", StringComparison.OrdinalIgnoreCase))
            text = text[2..];
        return text.Length is > 0 and <= 8
            && uint.TryParse(
                text,
                NumberStyles.HexNumber,
                CultureInfo.InvariantCulture,
                out var value)
            && value != 0U;
    }

    private static bool ValidCoordinate(string text)
    {
        return float.TryParse(
                text,
                NumberStyles.Float,
                CultureInfo.InvariantCulture,
                out var value)
            && float.IsFinite(value)
            && Math.Abs(value) <= 100000.0F;
    }

    private static bool ValidCharacterStep(
        string text,
        float maximumMagnitude)
    {
        return float.TryParse(
                text,
                NumberStyles.Float,
                CultureInfo.InvariantCulture,
                out var value)
            && float.IsFinite(value)
            && Math.Abs(value) <= maximumMagnitude;
    }

    public static void Validate(GameCommandResultPayload? value)
    {
        ValidateExperimentalEnvelope(
            value?.SchemaVersion ?? 0,
            value?.RequestId ?? Guid.Empty,
            value?.LevelGeneration ?? -1,
            value?.RuntimeIdentity ?? Guid.Empty);
        if (value is null
            || !GameCommandNames.All.Contains(value.Command)
            || !Identifier(value.StatusCode)
            || !Bounded(value.Message, 1, value.Command==GameCommandNames.AlienPvPVents?16384:ProtocolConstants.MaximumExperimentalTextLength))
        {
            throw new ProtocolException("The game command result is invalid.");
        }
    }

    private static void ValidateExperimentalEnvelope(
        int schemaVersion,
        Guid requestId,
        int levelGeneration,
        Guid portableDummyId)
    {
        if (schemaVersion != ProtocolConstants.ExperimentalSchemaVersion
            || requestId == Guid.Empty
            || levelGeneration < 0
            || portableDummyId == Guid.Empty)
        {
            throw new ProtocolException("The experimental envelope is invalid.");
        }
    }

    private static void ValidateBoundedUniqueList(
        IReadOnlyList<string> values,
        string label)
    {
        if (values.Count > ProtocolConstants.MaximumExperimentalListItems)
        {
            throw new ProtocolException($"Too many {label}.");
        }

        var unique = new HashSet<string>(StringComparer.Ordinal);
        foreach (var value in values)
        {
            if (!Bounded(value, 1, 128) || !unique.Add(value))
            {
                throw new ProtocolException($"{label} are invalid or duplicated.");
            }
        }
    }

    private static bool FiniteAndBounded(float value, float absoluteMaximum) =>
        float.IsFinite(value) && MathF.Abs(value) <= absoluteMaximum;

    private static bool Identifier(string? value) =>
        Bounded(value, 1, 64)
        && value!.All(character =>
            char.IsAsciiLetterOrDigit(character)
            || character is '_' or '-' or '.');

    private static bool PathText(string? value) =>
        value is not null
        && value.Length <= 32_767
        && value.All(character => !char.IsControl(character));

    private static bool Bounded(string? value, int minimum, int maximum) =>
        value is not null && value.Length >= minimum && value.Length <= maximum;

    private static bool HexDigest(string? value) =>
        value is { Length: 64 } && value.All(Uri.IsHexDigit);

    private static bool ValidUtf8Text(
        string? value,
        int maximumCodePoints,
        int maximumUtf8Bytes)
    {
        if (string.IsNullOrEmpty(value))
        {
            return false;
        }

        int byteCount;
        try
        {
            byteCount = StrictUtf8.GetByteCount(value);
        }
        catch (EncoderFallbackException)
        {
            return false;
        }

        if (byteCount > maximumUtf8Bytes)
        {
            return false;
        }

        var codePoints = 0;
        foreach (var rune in value.EnumerateRunes())
        {
            if (rune.Value < 0x20 || rune.Value == 0x7F
                || ++codePoints > maximumCodePoints)
            {
                return false;
            }
        }

        return codePoints != 0;
    }

    private static bool ValidCommandText(string value, bool allowLineBreaks)
    {
        int byteCount;
        try
        {
            byteCount = StrictUtf8.GetByteCount(value);
        }
        catch (EncoderFallbackException)
        {
            return false;
        }

        if (byteCount > 63)
        {
            return false;
        }

        foreach (var rune in value.EnumerateRunes())
        {
            if (rune.Value == 0
                || (rune.Value < 0x20
                    && !(allowLineBreaks
                        && (rune.Value == '\r'
                            || rune.Value == '\n'
                            || rune.Value == '\t')))
                || rune.Value == 0x7F)
            {
                return false;
            }
        }

        return true;
    }
    private static bool ValidSpawnCoordinate(float value) =>
        float.IsFinite(value) && MathF.Abs(value) <= 1_000_000F;

    private static bool ValidYaw(float value) =>
        float.IsFinite(value) && value is >= -3.3F and <= 3.3F;

}
