namespace AIMP.Protocol.Tests;

internal static class Program
{
    private static readonly TestCase[] Tests =
    {
        new("Steam relay bounded receive stream", SteamRelayTests.ReceiveBoundsAsync),
        new("Steam relay game process environment isolation", SteamRelayTests.EnvironmentAsync),
        new("Steam relay address validation and shutdown", SteamRelayTests.AddressesAndDisposalAsync),
        new("Steam relay invitation automatically joins lobby", SteamRelayTests.AutoJoinAsync),
        new("Steam relay pending invite, failure and independent IP connection", SteamRelayTests.PendingAndFailureAsync),
        new("Steam relay concurrent invitation handshakes", SteamRelayTests.ConcurrentInvitesAsync),
        new("Steam relay callback cancellation and shutdown", SteamRelayTests.CallbackShutdownAsync),
        new("Steam relay Direct Connection rejects Steam addresses", SteamRelayTests.DirectAddressGuardAsync),
        new("Steam relay waits for ready menu and reactivates after game restart", SteamRelayTests.MenuActivationAsync),
        new("Steam relay gameplay does not activate Spacewar", SteamRelayTests.GameplayDoesNotActivateAsync),
        new("Steam relay worker waits for menu activation", SteamRelayTests.WorkerWaitsForMenuAsync),
        new("Local API origin and loopback policy", LocalApiSecurityTests.AccessPolicyAsync),
        new("Local API HTTP security boundary", LocalApiSecurityTests.HttpBoundaryAsync),
        new("WorldSync bounded dispatch pipeline", WorldDispatchWindowTests.CapacityAndOrderAsync),
        new("WorldSync failed transaction skip and continuity", WorldTransactionSkipTests.SkipAndContinueAsync),
        new("MissionSync authored completion frontier", MissionFrontierTests.SelectionAsync),
        new("MissionSync survival objective frontiers", MissionFrontierTests.SurvivalAsync),
        new("MissionSync proxied composite milestone frontiers", MissionFrontierTests.ProxyAsync),
        new("MissionSync survival NPC spawn frontiers", MissionFrontierTests.NpcSpawnAsync),
        new("MissionSync nested mission start frontiers", MissionFrontierTests.MissionStartAsync),
        new("MissionSync generic retail graph coverage", MissionFrontierTests.RetailMissionGraphAsync),
        new("MissionSync canonical native configuration", MissionFrontierTests.BinaryAsync),
        new("MissionSync catalog loading barrier", PreparationTests.MissionCatalogBarrierAsync),
        new("MissionSync signal model", MissionSyncTests.ModelAsync),
        new("MissionSync ordered Host commits", MissionSyncTests.LobbyAsync),
        new("Protocol frames", ProtocolTests.FrameRoundTripAndValidationAsync),
        new("Protocol models", ProtocolTests.ModelValidationAsync),
        new("PlayerSync posture wire and revisions", PlayerPostureTests.WireAsync),
        new("PlayerSync posture stationary and edges", PlayerPostureTests.MovementAsync),
        new("PlayerSync posture coherent memory capture", PlayerPostureTests.CaptureAsync),
        new("PlayerSync retail crouch animation coverage", RemoteCrouchAnimationTests.RetailAsync),
        new("PlayerSync crouch animation independent preparation", RemoteCrouchAnimationTests.PreparationReceiptAsync),
        new("PlayerSync torch wire", PlayerTorchTests.WireAsync),
        new("PlayerSync hiding wire and validation", PlayerHidingTests.WireAsync),
        new("PlayerSync hiding coherent capture and lifecycle", PlayerHidingTests.CaptureAsync),
        new("PlayerSync hiding movement and ordered edges", PlayerHidingTests.MovementAsync),
        new("Crew characters catalog and animation profiles", PlayerCharacterTests.CatalogAsync),
        new("Crew characters assigned before automatic spawn", PlayerCharacterTests.LobbyAssignmentAsync),
        new("Crew characters Host assignment", PlayerCharacterTests.AssignmentAsync),
        new("Crew characters roster and spawn command", PlayerCharacterTests.WireAsync),
        new("PlayerSync torch effective capture", PlayerTorchTests.CaptureAsync),
        new("PlayerSync torch IPC validation", PlayerTorchTests.CommandValidationAsync),
        new("PlayerSync torch stationary and moving", PlayerTorchTests.MovementAsync),
        new("PlayerSync torch edges and binding", PlayerTorchTests.EdgesAndBindingAsync),
        new("PlayerSync torch local coalescing and lifecycle", PlayerTorchTests.CoordinatorAsync),
        new("PlayerSync torch isolated graph", PlayerTorchGraphTests.IsolationAsync),
        new("PlayerSync torch retail graph isolation and serialization", PlayerTorchGraphTests.RetailAsync),
        new("Survival equipment selection isolation", RemoteEquipmentSelectionTests.SelectionAsync),
        new("Survival flare public activation admission", RemoteEquipmentSelectionTests.FlareActivationAsync),
        new("Survival flare retail graph", RemoteEquipmentSelectionTests.RetailFlareGraphAsync),
        new("Throwable carrier validation", ThrowableSyncTests.CodecAsync),
        new("Throwable ownership, native admission and generation lifetime", ThrowableSyncTests.LobbyAsync),
        new("AISync authority codec", AISyncCodecTests.AuthorityStateRoundTripAsync),
        new("AISync hybrid motion carrier", AISyncCodecTests.HybridMotionAsync),
        new("AISync transaction shared canonical fixture", AISyncTransactionTests.CanonicalFixtureAsync),
        new("AISync transaction malformed records", AISyncTransactionTests.MalformedRecordsAsync),
        new("AISync transaction validation limits", AISyncTransactionTests.LimitsAsync),
        new("AISync transaction generated round trips", AISyncTransactionTests.GeneratedRoundTripsAsync),
        new("AISync marker identity codec", AISyncMarkerCodecTests.RunAllAsync),
        new("AISync native intention codec", AISyncNativeIntentCodecTests.RunAllAsync),
        new("AISync scheduler control codec", AISyncExecutionCodecTests.SchedulerControlAsync),
        new("AISync execution shared canonical fixture", AISyncExecutionCodecTests.CanonicalFixtureAsync),
        new("AISync movement complete wire and shared fixture", AISyncMovementTests.WireAsync),
        new("AISync movement full payload guards", AISyncMovementTests.GuardsAsync),
        new("AISync movement journal full evidence", AISyncMovementTests.JournalAsync),
        new("AISync native queue admission and retention directives", AISyncMovementTests.DirectivesAsync),
        new("AISync execution transport controls", AISyncExecutionTransportTests.ControlsAsync),
        new("AISync execution transport journal", AISyncExecutionTransportTests.JournalAsync),
        new("AISync settled prefix checkpoint journal", AISyncCheckpointTests.JournalAsync),
        new("AISync local character incarnation codec", CharacterIncarnationTests.CodecAsync),
        new("AISync local character incarnation IPC", IpcTests.CharacterIncarnationAsync),
        new("AISync local character incarnation connection and generation", CharacterIncarnationTests.BindingAsync),
        new("AISync remote construction ticket capture", CharacterIncarnationTests.RemoteCreationAsync),
        new("AISync portable player incarnation allocation", PlayerIncarnationTests.AllocationAsync),
        new("AISync portable player incarnation stale lifetime refusal", PlayerIncarnationTests.WindowAsync),
        new("AISync portable player incarnation authenticated relay", PlayerIncarnationTests.TransportAsync),
        new("AISync participant configuration v3 shared fixture", AISyncParticipantBindingTests.ConfigurationAsync),
        new("AISync participant captured creation evidence", AISyncParticipantBindingTests.EvidenceAsync),
        new("AISync participant attack incarnation v13 carrier", AISyncParticipantBindingTests.PairCarrierAsync),
        new("AISync pair registration full authority seal", AISyncParticipantBindingTests.PairAuthorityAsync),
        new("AISync settled prefix checkpoint loopback", AISyncCheckpointTests.TransportAsync),
        new("AISync checkpoint waits for every native consumer", AISyncCheckpointTests.HostBridgeAsync),
        new("AISync Host bounded ingress loss fences source", AISyncHostBridgeTests.IngressOverflowAsync),
        new("AISync execution transport delivery and retry", AISyncExecutionTransportTests.DeliveryAsync),
        new("AISync FIFO congestion telemetry preserves packet order", AISyncExecutionTransportTests.SenderQueueTelemetryAsync),
        new("AISync execution transport peer isolation", AISyncExecutionTransportTests.IsolationAsync),
        new("AISync execution transport direction authority", AISyncExecutionTransportTests.DirectionAsync),
        new("AISync execution transport pending writer shutdown", AISyncExecutionTransportTests.PendingWriterShutdownAsync),
        new("AISync execution IPC bridge readiness and receipts", AISyncExecutionBridgeTests.DeliveryAsync),
        new("AISync execution IPC bridge native incarnations", AISyncExecutionBridgeTests.IncarnationsAsync),
        new("AISync configuration reconnect same process", IpcTests.AISyncReconfigureOnReconnectAsync),
        new("AISync Host producer fanout and native readiness", AISyncHostBridgeTests.FanoutAsync),
        new("AISync Host producer recovery and incarnations", AISyncHostBridgeTests.RecoveryAndIncarnationAsync),
        new("AISync Host producer cancellation supersedes waiting retries", AISyncHostBridgeTests.CancelWaitingAsync),
        new("AISync latest Host intent retry priority and native ordering", AISyncHostBridgeTests.CurrentIntentRetryPriorityAsync),
        new("AISync retry congestion bounded actor fairness", AISyncHostBridgeTests.RetryCongestionPlannerAsync),
        new("AISync retry causal constructor bypass and protected barriers", AISyncRetryRegressionTests.CausalBypassAsync),
        new("AISync retry planner full actor identity", AISyncRetryRegressionTests.FullIdentityAsync),
        new("AISync retry planner bounded allocations under backlog", AISyncRetryRegressionTests.AllocationBudgetAsync),
        new("AISync retry real Host tick fairness for stable 16 and 32 actor groups", AISyncRetryRegressionTests.HostLoopFairnessAsync),
        new("AISync retry local constructor unblocks protected action", AISyncRetryRegressionTests.LocalConstructorProgressAsync),
        new("AISync native repair classification, timeouts and bounded retries", AISyncHostBridgeTests.NativeRecoveryPolicyAsync),
        new("AISync Host rejected operation recovers through a fresh authority epoch", AISyncHostBridgeTests.UpdateRejectedIsolationAsync),
        new("AISync Host unknown native result triggers safe scope retirement", AISyncHostBridgeTests.UnknownIsolationAsync),
        new("AISync repeated native faults exhaust the automatic repair budget", AISyncHostBridgeTests.NativeRepairBudgetAsync),
        new("AISync Host refusal exact receipt validation", AISyncHostBridgeTests.RefusalEchoValidationAsync),
        new("AISync Host forged refusal recovery", AISyncHostBridgeTests.ForgedRefusalRecoveryAsync),
        new("AISync solved route shared fixture", AISyncRouteTests.FixtureAsync),
        new("AISync solved route bounds and strict domain", AISyncRouteTests.BoundsAsync),
        new("AISync solved route generated roundtrips", AISyncRouteTests.GeneratedAsync),
        new("AISync action reservation correlation", AISyncActionCorrelationTests.ContractAsync),
        new("AISync route command journal", AISyncRouteTransportTests.JournalAsync),
        new("AISync strict route capability negotiation", AISyncRouteTransportTests.NegotiationAsync),
        new("AISync route loopback", AISyncRouteTransportTests.LoopbackAsync),
        new("AISync route source and native bridges", AISyncRouteTransportTests.BridgesAsync),
        new("AISync route direction", AISyncRouteTransportTests.DirectionAsync),
        new("AISync route native IPC", IpcTests.RouteDeliveryAsync),
        new("AISync execution round trips", AISyncExecutionCodecTests.RoundTripsAsync),
        new("AISync execution malformed packets", AISyncExecutionCodecTests.MalformedPacketsAsync),
        new("AISync execution truncation boundaries", AISyncExecutionCodecTests.AllTruncationsAsync),
        new("AISync execution scope and identity", AISyncExecutionCodecTests.ScopeAndIdentityAsync),
        new("AISync execution observation refusal and profiles", AISyncExecutionCodecTests.ObservationAndProfilesAsync),
        new("AISync execution numeric validation", AISyncExecutionCodecTests.NumericValidationAsync),
        new("AISync execution receipt identity", AISyncExecutionCodecTests.ReceiptsAsync),
        new("AISync shadow codec round trips", AISyncShadowTests.CodecRoundTripsAsync),
        new("AISync shadow malformed packets", AISyncShadowTests.MalformedPacketsAsync),
        new("AISync shadow journal bounds and receipts", AISyncShadowTests.JournalBoundsAndReceiptsAsync),
        new("AISync shadow receiver recovery", AISyncShadowTests.ReceiverRecoveryAsync),
        new("AISync shadow reconnect and epochs", AISyncShadowTests.ReconnectAndEpochsAsync),
        new("AISync shadow generated observations", AISyncShadowTests.GeneratedObservationsAsync),
        new("AISync shadow lobby delivery", AISyncShadowLobbyTests.DeliveryAsync),
        new("AISync shadow lobby peer isolation", AISyncShadowLobbyTests.IsolationAsync),
        new("AISync shadow lobby scope refusal", AISyncShadowLobbyTests.RefusalAsync),
        new("AISync shadow lobby restart", AISyncShadowLobbyTests.RestartAsync),
        new("AISync shadow lobby timeout and retention", AISyncShadowLobbyTests.TimeoutAndRetentionAsync),
        new("AISync shadow client validation", AISyncShadowLobbyTests.ClientValidationAsync),
        new("AISync shadow native bridge connection fences", AISyncShadowBridgeTests.ConnectionAndGenerationFencesAsync),
        new("Alien option configuration", AlienOptionTests.ConfigurationAsync),
        new("Alien ventilation selection wire and catalogue",AlienVentTests.TransportAsync),
        new("Alien ventilation owner stage after replica timeout",AlienVentTests.OwnerStageRecoveryAsync),
        new("Alien ventilation late native entry after cancellation",AlienVentTests.LateCancelledEntryAsync),
        new("Alien option payload defaults", AlienOptionTests.PayloadDefaultsAsync),
        new("Alien option lobby and restart", AlienOptionTests.LobbyRoundRestartAsync),
        new("Voice codec", VoiceTests.CodecAsync),
        new("Voice bounded receive window", VoiceTests.ReceiveWindowAsync),
        new("Voice lobby ownership and restart", VoiceTests.LobbyAsync),
        new("WS3 wire, authority and manifest", ProtocolTests.WorldPublicActionContractAsync),
        new("WS3 local effect boundary", WorldPublicActionTests.LocalEffectBoundaryAsync),
        new("WS3 retail Door event carriers", WorldPublicActionTests.RetailDoorEventCarriersAsync),
        new("Lobby lifecycle", ProtocolTests.LobbyLifecycleAsync),
        new("Lobby manifest preflight", PreparationTests.LobbyManifestPreflightAsync),
        new("Session loading barrier", PreparationTests.SessionReleaseBarrierAsync),
        new("Round authority generations", RoundRestartTests.AuthorityGenerationsAsync),
        new("Round gameplay sequence restart", RoundRestartTests.GameplaySequencesAsync),
        new("Mission complete native terminal endpoint", MissionCompleteTests.NativeEndpointAsync),
        new("Mission complete models and wait message", MissionCompleteTests.ModelAsync),
        new("Mission complete when every participant finishes", MissionCompleteTests.AllFinishedAsync),
        new("Mission complete with dead and finished participants", MissionCompleteTests.MixedOutcomeAsync),
        new("Game over when nobody finishes", MissionCompleteTests.AllDeadAsync),
        new("Launcher reset data scope", LauncherResetTests.GeneratedDataScopeAsync),
        new("Launcher reset path boundary", LauncherResetTests.RefusesOutsideSettingsPathAsync),
        new("AI process selection", LauncherTests.ProcessSelectionAsync),
        new("Launcher native build version agreement", LauncherTests.NativeBuildVersionAgreementAsync),
        new("WorldManifest native compiler version agreement", LauncherTests.NativeManifestVersionAgreementAsync),
        new("Native DLL priority", LauncherTests.NativeDllPriorityAsync),
        new("Native module matching", LauncherTests.NativeModuleMatchingAsync),
        new("Diagnostics export", LauncherTests.DiagnosticsExportAsync),
        new("Log retention compresses losslessly and keeps newest per kind", LogRetentionTests.CompressesAndKeepsNewestAsync),
        new("Log retention leaves open and active logs alone", LogRetentionTests.LeavesOpenLogsAloneAsync),
        new("Launcher diagnostics applies log retention at startup", LogRetentionTests.DiagnosticsRunsRetentionAsync),
        new("Command result diagnostics", LauncherTests.CommandResultDiagnosticsAsync),
        new("Character movement tools", LauncherTests.CharacterMovementToolsAsync),
        new("Local player position tools", LauncherTests.LocalPlayerPositionToolsAsync),
        new("Registered local player resolution", LocalPlayerResolverTests.ResolvesRegisteredPlayerAsync),
        new("Local player bootstrap rejection", LocalPlayerResolverTests.RejectsBootstrapPoolSlotAsync),
        new("Launcher integration tools", LauncherTests.LauncherIntegrationToolsAsync),
        new("Developer mode startup arguments", DeveloperModeTests.StartupArgumentsAsync),
        new("Developer mode session gate", DeveloperModeTests.SessionGateAsync),
        new("Steam persona selection", SteamPersonaTests.SelectionAsync),
        new("Steam persona imposed name", SteamPersonaTests.ImposedNameAsync),
        new("UI bridge rejects browser traffic", BridgeSecurityTests.RejectsBrowserTrafficAsync),
        new("UI bridge accepts game traffic", BridgeSecurityTests.AcceptsGameTrafficAsync),
        new("Solo lobby starts alone", SoloLobbyTests.StartsAloneAsync),
        new("Solo lobby rejects joins", SoloLobbyTests.RejectsJoinAsync),
        new("Multiplayer lobby needs a client", SoloLobbyTests.MultiplayerNeedsClientAsync),
        new("Private lobby starts alone and accepts joins", SoloLobbyTests.PrivateLobbyAcceptsJoinAsync),
        new("Steam helper protocol round-trips lobby data", SteamHostTests.RoundTripsAsync),
        new("Steam helper protocol keeps error kinds", SteamHostTests.KeepsErrorKindsAsync),
        new("Steam helper mode needs its exact arguments", SteamHostTests.IgnoresNormalArgumentsAsync),
        new("Campaign start stuck at the menu is detected", StartWatchdogTests.DetectsCampaignStuckAtMenuAsync),
        new("Start watchdog ignores Survival and loaded gameplay", StartWatchdogTests.IgnoresSurvivalAndGameplayAsync),
        new("Generic native IPC", IpcTests.GenericHandshakeAndQueuedCommandAsync),
        new("AISync rejection preserves IPC", IpcTests.AISyncRejectionPreservesConnectionAsync),
        new("Manifest cache identity casing", LauncherTests.ManifestCacheIdentityAsync),
        new("Lobby preparation status", LauncherTests.LobbyReadinessStatusAsync),
        new("Launcher body length", ArchitectureTests.LauncherBodiesAreShortAsync)
    };

    [STAThread]
    private static async Task<int> Main(string[] args)
    {
        var tests = SelectedTests();
        if (args.Length != 0)
        {
            if (args.Length != 2 || args[0] != "--filter")
                throw new ArgumentException("Usage: --filter <test name>");
            var filters = args[1].Split(',', StringSplitOptions.TrimEntries | StringSplitOptions.RemoveEmptyEntries);
            tests = tests.Where(test => filters.Any(filter => test.Name.Contains(filter, StringComparison.OrdinalIgnoreCase))).ToArray();
            if (tests.Length == 0) throw new ArgumentException("No tests matched the filter.");
        }
        var failures = new List<string>();
        foreach (var test in tests)
        {
            await RunAsync(test, failures).ConfigureAwait(false);
        }

        Console.WriteLine($"{tests.Length - failures.Count}/{tests.Length} tests passed.");
        return failures.Count == 0 ? 0 : 1;
    }

    private static TestCase[] SelectedTests()
    {
        if (!string.Equals(Environment.GetEnvironmentVariable(
                "AIMP_REQUIRE_DEPLOYMENT_TEST"), "1", StringComparison.Ordinal))
        {
            return Tests;
        }

        var tests = Tests.ToList();
        tests.Insert(5, new TestCase(
            "Native DLL deployment", LauncherTests.NativeDllDeploymentAsync));
        return tests.ToArray();
    }

    private static async Task RunAsync(TestCase test, ICollection<string> failures)
    {
        try
        {
            await test.Run().ConfigureAwait(false);
            Console.WriteLine($"PASS {test.Name}");
        }
        catch (Exception exception)
        {
            failures.Add(test.Name);
            Console.Error.WriteLine($"FAIL {test.Name}: {exception}");
        }
    }
}

internal sealed record TestCase(string Name, Func<Task> Run);

internal static class AssertEx
{
    internal static void True(bool condition, string? message = null)
    {
        if (!condition)
        {
            throw new InvalidOperationException(message ?? "Expected condition to be true.");
        }
    }

    internal static void False(bool condition, string? message = null) =>
        True(!condition, message ?? "Expected condition to be false.");

    internal static void Equal<T>(T expected, T actual, string? message = null)
        where T : notnull
    {
        if (!EqualityComparer<T>.Default.Equals(expected, actual))
        {
            throw new InvalidOperationException(
                message ?? $"Expected '{expected}', received '{actual}'.");
        }
    }

    internal static void SequenceEqual<T>(
        IEnumerable<T> expected,
        IEnumerable<T> actual,
        string? message = null)
    {
        if (!expected.SequenceEqual(actual))
        {
            throw new InvalidOperationException(message ?? "Sequences differ.");
        }
    }

    internal static TException Throws<TException>(Action action)
        where TException : Exception
    {
        try
        {
            action();
        }
        catch (TException exception)
        {
            return exception;
        }

        throw new InvalidOperationException($"Expected {typeof(TException).Name}.");
    }

    internal static async Task<TException> ThrowsAsync<TException>(Func<Task> action)
        where TException : Exception
    {
        try
        {
            await action().ConfigureAwait(false);
        }
        catch (TException exception)
        {
            return exception;
        }

        throw new InvalidOperationException($"Expected {typeof(TException).Name}.");
    }
}

internal sealed class TemporaryDirectory : IDisposable
{
    internal TemporaryDirectory()
    {
        DirectoryPath = System.IO.Path.Combine(
            System.IO.Path.GetTempPath(),
            "AIMP.Protocol.Tests",
            Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(DirectoryPath);
    }

    internal string DirectoryPath { get; }

    internal string Combine(params string[] parts)
    {
        var values = new[] { DirectoryPath }.Concat(parts).ToArray();
        return System.IO.Path.Combine(values);
    }

    public void Dispose()
    {
        if (Directory.Exists(DirectoryPath))
        {
            Directory.Delete(DirectoryPath, true);
        }
    }
}
