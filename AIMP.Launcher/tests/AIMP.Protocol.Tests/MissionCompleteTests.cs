using AIMP.Launcher;
using CATHODE.Scripting;
using CATHODE.Scripting.Internal;

namespace AIMP.Protocol.Tests;

internal static class MissionCompleteTests
{
    internal static Task NativeEndpointAsync()
    {
        var endpoint = new FunctionEntity { function = ShortGuidUtils.Generate("FlashInvoke") };
        void Argument(string name, string value) => endpoint.parameters.Add(
            new Parameter(name, new cString { value = value }, ParameterVariant.PARAMETER));
        Argument("method", "challengemapOpenedResults");
        Argument("layer_name", "ChallengeMapOverlay");
        AssertEx.True(WorldManifestCompiler.IsPlayerMissionCompleteEndpoint(endpoint));
        endpoint.function = ShortGuidUtils.Generate("DebugText");
        AssertEx.False(WorldManifestCompiler.IsPlayerMissionCompleteEndpoint(endpoint));
        endpoint.function = ShortGuidUtils.Generate("FlashInvoke");
        ((cString)endpoint.parameters[0].content).value = "dead";
        AssertEx.False(WorldManifestCompiler.IsPlayerMissionCompleteEndpoint(endpoint));
        ((cString)endpoint.parameters[0].content).value = "challengemapOpenedResults";
        ((cString)endpoint.parameters[1].content).value = "OtherOverlay";
        AssertEx.False(WorldManifestCompiler.IsPlayerMissionCompleteEndpoint(endpoint));
        endpoint.parameters.Clear();
        AssertEx.False(WorldManifestCompiler.IsPlayerMissionCompleteEndpoint(endpoint));
        return Task.CompletedTask;
    }

    private sealed record Session(LobbyHost Host, LobbyClient Client, int Generation) : IAsyncDisposable
    {
        public async ValueTask DisposeAsync()
        {
            await Client.DisposeAsync().ConfigureAwait(false);
            await Host.DisposeAsync().ConfigureAwait(false);
        }
    }

    private static async Task<Session> StartAsync(CancellationToken token)
    {
        var compatibility = new CompatibilityInfo(ProtocolConstants.Version,
            "test-launcher", "test-dll", new string('A', 64), "test-profile", new string('B', 64));
        var host = new LobbyHost(new LobbyHostOptions("Mission test", "Ripley", Guid.NewGuid(),
            0, 2, "TestLevel", compatibility));
        host.Start();
        var client = await LobbyClient.ConnectAsync(new LobbyClientOptions("127.0.0.1",
            host.BoundPort, Guid.NewGuid(), "Dallas", compatibility), token).ConfigureAwait(false);
        client.Start();
        await client.SetReadyAsync(true, token).ConfigureAwait(false);
        await WaitUntilAsync(() => host.CanStart, token).ConfigureAwait(false);
        var start = await host.StartGameAsync(DateTimeOffset.UtcNow, token).ConfigureAwait(false);
        return new Session(host, client, start.LevelGeneration);
    }

    // Every participant finishes: the round ends as a victory, after a visible
    // "1/2 finished" progress for the participant that finished first.
    internal static async Task AllFinishedAsync()
    {
        using var timeout = new CancellationTokenSource(TimeSpan.FromSeconds(20));
        await using var session = await StartAsync(timeout.Token).ConfigureAwait(false);
        RoundProgressPayload? progress = null; RoundEndedPayload? ended = null;
        session.Client.RoundProgressReceived += (_, value) => progress = value;
        session.Client.RoundEnded += (_, value) => ended = value;
        await session.Host.ReportHostMissionCompleteAsync(session.Generation, timeout.Token).ConfigureAwait(false);
        await WaitUntilAsync(() => progress is not null, timeout.Token).ConfigureAwait(false);
        AssertEx.Equal(1, progress!.Finished); AssertEx.Equal(0, progress.Dead); AssertEx.Equal(2, progress.Participants);
        AssertEx.True(ended is null, "One finisher out of two must not end the round.");
        // A duplicate edge is not a second finisher.
        await session.Host.ReportHostMissionCompleteAsync(session.Generation, timeout.Token).ConfigureAwait(false);
        await session.Client.SendMissionCompleteAsync(session.Generation, timeout.Token).ConfigureAwait(false);
        await WaitUntilAsync(() => ended is not null, timeout.Token).ConfigureAwait(false);
        AssertEx.Equal("MissionComplete", ended!.Reason);
        await WaitUntilAsync(() => session.Client.Snapshot?.State == "Lobby", timeout.Token).ConfigureAwait(false);
    }

    // Mixed outcome: one finished, the other died. Still a victory. A dead
    // participant cannot finish afterwards and a finisher's later death edge
    // cannot reclassify it.
    internal static async Task MixedOutcomeAsync()
    {
        using var timeout = new CancellationTokenSource(TimeSpan.FromSeconds(20));
        await using var session = await StartAsync(timeout.Token).ConfigureAwait(false);
        var reports = new List<RoundProgressPayload>(); RoundEndedPayload? ended = null;
        session.Host.RoundProgressChanged += (_, value) => { lock (reports) reports.Add(value); };
        session.Host.RoundEnded += (_, value) => ended = value;
        await session.Client.SendPlayerDeathAsync(session.Generation, timeout.Token).ConfigureAwait(false);
        await session.Client.SendMissionCompleteAsync(session.Generation, timeout.Token).ConfigureAwait(false);
        await WaitUntilAsync(() => { lock (reports) return reports.Count == 1; }, timeout.Token).ConfigureAwait(false);
        AssertEx.Equal(0, reports[0].Finished); AssertEx.Equal(1, reports[0].Dead);
        await session.Host.ReportHostMissionCompleteAsync(session.Generation, timeout.Token).ConfigureAwait(false);
        await WaitUntilAsync(() => ended is not null, timeout.Token).ConfigureAwait(false);
        AssertEx.Equal("MissionComplete", ended!.Reason);
        AssertEx.Equal(1, reports.Count, "The final terminal participant publishes RoundEnded, not progress.");
    }

    // Unchanged game over: nobody finished.
    internal static async Task AllDeadAsync()
    {
        using var timeout = new CancellationTokenSource(TimeSpan.FromSeconds(20));
        await using var session = await StartAsync(timeout.Token).ConfigureAwait(false);
        RoundEndedPayload? ended = null;
        session.Client.RoundEnded += (_, value) => ended = value;
        await session.Host.ReportHostPlayerDeathAsync(session.Generation, timeout.Token).ConfigureAwait(false);
        await session.Host.ReportHostMissionCompleteAsync(session.Generation, timeout.Token).ConfigureAwait(false);
        await session.Client.SendPlayerDeathAsync(session.Generation, timeout.Token).ConfigureAwait(false);
        await WaitUntilAsync(() => ended is not null, timeout.Token).ConfigureAwait(false);
        AssertEx.Equal("AllPlayersDead", ended!.Reason);
    }

    internal static Task ModelAsync()
    {
        var id = Guid.NewGuid();
        ModelValidation.Validate(new RoundProgressPayload(id, 1, 1, 1, 3));
        AssertEx.Throws<ProtocolException>(() => ModelValidation.Validate(new RoundProgressPayload(id, 1, 2, 2, 3)));
        AssertEx.Throws<ProtocolException>(() => ModelValidation.Validate(new RoundProgressPayload(id, 1, 0, 0, 5)));
        AssertEx.Throws<ProtocolException>(() => ModelValidation.Validate(new PlayerMissionCompletePayload(id, 0, 1)));
        // Native lifecycle schema 2: a level exit alone, a death alone, or both.
        ModelValidation.Validate(new NativePlayerLifecycleStatePayload(2, 0, false, 1));
        ModelValidation.Validate(new NativePlayerLifecycleStatePayload(2, 1, true, 0));
        ModelValidation.Validate(new NativePlayerLifecycleStatePayload(1, 1, true));
        AssertEx.Throws<ProtocolException>(() => ModelValidation.Validate(new NativePlayerLifecycleStatePayload(2, 0, false, 0)));
        AssertEx.Throws<ProtocolException>(() => ModelValidation.Validate(new NativePlayerLifecycleStatePayload(2, 1, false, 0)));
        AssertEx.Throws<ProtocolException>(() => ModelValidation.Validate(new NativePlayerLifecycleStatePayload(1, 0, false, 1)));
        // Schema 3: spectated participant, only after a local death.
        ModelValidation.Validate(new NativePlayerLifecycleStatePayload(3, 1, true, 0, 42, 2));
        ModelValidation.Validate(new NativePlayerLifecycleStatePayload(3, 1, true, 0, 0, 0));
        AssertEx.Throws<ProtocolException>(() => ModelValidation.Validate(new NativePlayerLifecycleStatePayload(3, 0, false, 1, 42, 1)));
        AssertEx.Throws<ProtocolException>(() => ModelValidation.Validate(new NativePlayerLifecycleStatePayload(2, 1, true, 0, 42, 1)));
        AssertEx.Throws<ProtocolException>(() => ModelValidation.Validate(new NativePlayerLifecycleStatePayload(3, 1, true, 0, 42, 33)));
        AssertEx.Equal("Waiting for players: 1/3 finished, 1 dead",
            MultiplayerSessionCoordinator.MissionWaitMessage(new RoundProgressPayload(id, 1, 1, 1, 3)));
        AssertEx.Equal("Waiting for the other players...", MultiplayerSessionCoordinator.MissionWaitMessage(null));
        var fields = MultiplayerSessionCoordinator.MissionWaitFields(id, 2, new RoundProgressPayload(id, 1, 1, 0, 2));
        AssertEx.Equal("Waiting for the other players...", fields["message"], "A stale generation's progress is not shown.");
        return Task.CompletedTask;
    }

    private static async Task WaitUntilAsync(Func<bool> condition, CancellationToken token)
    {
        while (!condition()) await Task.Delay(10, token).ConfigureAwait(false);
    }
}
