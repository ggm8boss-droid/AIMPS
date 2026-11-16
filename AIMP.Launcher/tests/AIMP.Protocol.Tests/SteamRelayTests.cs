using System.Diagnostics;
using System.Reflection;
using AIMP.Launcher;

namespace AIMP.Protocol.Tests;

internal static class SteamRelayTests
{
    private const ulong HostId = 76561198000000001;
    private const BindingFlags Private = BindingFlags.Instance | BindingFlags.NonPublic;

    internal static async Task ReceiveBoundsAsync()
    {
        var queue = new SteamRelayReceiveQueue();
        AssertEx.False(queue.TryWrite(Array.Empty<byte>()));
        AssertEx.False(queue.TryWrite(new byte[SteamRelayReceiveQueue.MaximumMessageBytes + 1]));
        for (var i = 0; i < SteamRelayReceiveQueue.Capacity; i++)
            AssertEx.True(queue.TryWrite(new byte[] { (byte)i }));
        AssertEx.False(queue.TryWrite(new byte[] { 255 }), "A saturated reliable stream must reject, not drop old bytes.");
        queue.Complete();
        var received = new List<byte>();
        using var timeout = new CancellationTokenSource(TimeSpan.FromSeconds(5));
        await foreach (var value in queue.ReadAllAsync(timeout.Token)) received.Add(value[0]);
        AssertEx.SequenceEqual(Enumerable.Range(0, SteamRelayReceiveQueue.Capacity).Select(i => (byte)i), received);
        AssertEx.False(queue.TryWrite(new byte[] { 1 }));
        var empty = new SteamRelayReceiveQueue();
        using var cancelled = new CancellationTokenSource();
        cancelled.Cancel();
        await AssertEx.ThrowsAsync<OperationCanceledException>(async () =>
        {
            await foreach (var value in empty.ReadAllAsync(cancelled.Token)) { }
        });
    }

    internal static Task EnvironmentAsync()
    {
        var originalApp = Environment.GetEnvironmentVariable("SteamAppId");
        var originalGame = Environment.GetEnvironmentVariable("SteamGameId");
        try
        {
            Environment.SetEnvironmentVariable("SteamAppId", "214490");
            Environment.SetEnvironmentVariable("SteamGameId", null);
            AssertEx.Throws<IOException>(() => SteamAppEnvironment.Initialize(() =>
            {
                Environment.SetEnvironmentVariable("SteamAppId", "480");
                Environment.SetEnvironmentVariable("SteamGameId", "480");
                throw new IOException("Steam not running");
            }));
            AssertEx.Equal("214490", Environment.GetEnvironmentVariable("SteamAppId")!);
            AssertEx.True(Environment.GetEnvironmentVariable("SteamGameId") is null);
            SteamAppEnvironment.Initialize(() => Environment.SetEnvironmentVariable("SteamAppId", "480"));
            AssertEx.Equal("214490", Environment.GetEnvironmentVariable("SteamAppId")!);
            var info = new ProcessStartInfo("AI.exe");
            info.Environment["SteamAppId"] = "480";
            info.Environment["SteamGameId"] = "480";
            info.Environment["AIMP_TEST"] = "keep";
            SteamAppEnvironment.PrepareGameProcess(info);
            AssertEx.False(info.Environment.ContainsKey("SteamAppId"));
            AssertEx.False(info.Environment.ContainsKey("SteamGameId"));
            AssertEx.Equal("keep", info.Environment["AIMP_TEST"]!);
            info.Environment["SteamAppId"] = "214490";
            SteamAppEnvironment.PrepareGameProcess(info);
            AssertEx.Equal("214490", info.Environment["SteamAppId"]!);
        }
        finally
        {
            Environment.SetEnvironmentVariable("SteamAppId", originalApp);
            Environment.SetEnvironmentVariable("SteamGameId", originalGame);
        }
        return Task.CompletedTask;
    }

    internal static Task AddressesAndDisposalAsync()
    {
        AssertEx.True(SteamRelay.TryParseAddress($"STEAM:{HostId}", out var id));
        AssertEx.Equal(HostId, id);
        foreach (var invalid in new[] { "steam:0", "steam:-1", "steam:abc", "steam:72057594037927937", "steam:103582791429521412", "127.0.0.1" })
            AssertEx.False(SteamRelay.TryParseAddress(invalid, out _), invalid);
        using var directory = new TemporaryDirectory();
        using var diagnostics = new LauncherDiagnostics(directory.Combine("logs"));
        var relay = new SteamRelay(diagnostics);
        relay.Dispose();
        relay.Dispose();
        AssertEx.Throws<ObjectDisposedException>(relay.Start);
        return AssertEx.ThrowsAsync<ObjectDisposedException>(() => relay.StartHostingAsync(28015, "test"));
    }

    internal static async Task AutoJoinAsync()
    {
        using var fixture = new Fixture();
        await using var host = fixture.Host();
        host.Start();
        fixture.Relay.Port = host.BoundPort;
        fixture.Ready();
        await fixture.Coordinator.AcceptSteamInviteAsync("Ripley", SteamRelay.FormatAddress(HostId));
        var client = fixture.Get<LobbyClient>("_client");
        AssertEx.Equal(host.SessionId, client.SessionId, "Accepting the invite must finish the lobby handshake without JOIN_DIRECT_IP.");
        AssertEx.Equal(1, fixture.Relay.OpenCount);
        AssertEx.Equal(HostId, fixture.Relay.LastHost);
        AssertEx.Equal("192.0.2.42", fixture.Settings.Load().HostAddress);
        AssertEx.Equal(29000, fixture.Settings.Load().Port);
        await AssertEx.ThrowsAsync<InvalidOperationException>(() => fixture.Coordinator.AcceptSteamInviteAsync("Other", SteamRelay.FormatAddress(HostId + 1)));
        AssertEx.Equal(1, fixture.Relay.OpenCount);
        AssertEx.Equal(host.SessionId, client.SessionId);
    }

    internal static async Task PendingAndFailureAsync()
    {
        using var fixture = new Fixture();
        await using var host = fixture.Host();
        host.Start();
        fixture.Relay.Port = host.BoundPort;
        await fixture.Coordinator.AcceptSteamInviteAsync("Ripley", SteamRelay.FormatAddress(HostId));
        AssertEx.Equal(0, fixture.Relay.OpenCount, "Do not connect before the frontend/native runtime is ready.");
        fixture.Ready();
        await fixture.CallAsync("ResumeSteamInvitesFromMenuAsync");
        AssertEx.Equal(host.SessionId, fixture.Get<LobbyClient>("_client").SessionId);

        using var failed = new Fixture();
        failed.Ready();
        failed.Relay.Failure = new IOException("Steam unavailable");
        await AssertEx.ThrowsAsync<IOException>(() => failed.Coordinator.AcceptSteamInviteAsync("Ripley", SteamRelay.FormatAddress(HostId)));
        AssertEx.Equal(1, failed.Relay.CloseCount);
        AssertEx.Equal("192.0.2.42", failed.Settings.Load().HostAddress);
        await AssertEx.ThrowsAsync<ArgumentException>(() => failed.Coordinator.AcceptSteamInviteAsync("Ripley", "steam:invalid"));
        AssertEx.Equal(1, failed.Relay.OpenCount);
        // A failed invite must release the command gate so Direct Connection still works.
        failed.Settings.Save(failed.Settings.Load() with { HostAddress = "127.0.0.1", Port = host.BoundPort });
        await failed.CallAsync("JoinDirectAsync");
        AssertEx.Equal(host.SessionId, failed.Get<LobbyClient>("_client").SessionId);
        AssertEx.Equal(1, failed.Relay.OpenCount, "Direct Connection must not call Steam.");
    }

    internal static async Task ConcurrentInvitesAsync()
    {
        using var fixture = new Fixture();
        await using var host = fixture.Host();
        host.Start();
        fixture.Ready();
        fixture.Relay.Port = host.BoundPort;
        fixture.Relay.Delay = new(TaskCreationOptions.RunContinuationsAsynchronously);
        var first = fixture.Coordinator.AcceptSteamInviteAsync("First", SteamRelay.FormatAddress(HostId));
        await fixture.Relay.Entered.Task.WaitAsync(TimeSpan.FromSeconds(5));
        var second = fixture.Coordinator.AcceptSteamInviteAsync("Second", SteamRelay.FormatAddress(HostId + 1));
        AssertEx.False(second.IsCompleted, "The second invitation must wait for the first handshake.");
        AssertEx.Equal(1, fixture.Relay.OpenCount);
        fixture.Relay.Delay.SetResult();
        await first;
        await AssertEx.ThrowsAsync<InvalidOperationException>(() => second);
        AssertEx.Equal(1, fixture.Relay.OpenCount);
        AssertEx.Equal(host.SessionId, fixture.Get<LobbyClient>("_client").SessionId);
    }

    internal static async Task CallbackShutdownAsync()
    {
        using var fixture = new Fixture();
        fixture.Ready();
        fixture.Relay.Delay = new(TaskCreationOptions.RunContinuationsAsynchronously);
        fixture.Relay.Raise("Ripley", SteamRelay.FormatAddress(HostId));
        await fixture.Relay.Entered.Task.WaitAsync(TimeSpan.FromSeconds(5));
        fixture.Coordinator.Dispose();
        AssertEx.True(fixture.Relay.Disposed);
        AssertEx.Equal(1, fixture.Relay.CloseCount);
        AssertEx.True(fixture.Get<LobbyClient?>("_client") is null);
        fixture.Relay.Raise("Late", SteamRelay.FormatAddress(HostId));
        AssertEx.Equal(1, fixture.Relay.OpenCount);
    }

    internal static async Task DirectAddressGuardAsync()
    {
        using var fixture = new Fixture();
        fixture.Ready();
        fixture.Settings.Save(fixture.Settings.Load() with { HostAddress = SteamRelay.FormatAddress(HostId) });
        await AssertEx.ThrowsAsync<InvalidOperationException>(() => fixture.CallAsync("JoinDirectAsync"));
        AssertEx.Equal(0, fixture.Relay.OpenCount);
        AssertEx.True(fixture.Get<LobbyClient?>("_client") is null);
    }

    internal static async Task MenuActivationAsync()
    {
        using var fixture = new Fixture();
        var hello = new ScaleformBridgeMessage("HELLO", new Dictionary<string, string>());
        AssertEx.Equal(0, fixture.Relay.MenuProcesses.Count, "Opening the launcher must not activate Spacewar.");
        await fixture.CallAsync("HandleHelloAsync", hello);
        AssertEx.Equal(0, fixture.Relay.MenuProcesses.Count, "An unready native runtime must not activate Spacewar.");
        fixture.Ready(101, false);
        await fixture.CallAsync("HandleHelloAsync", hello);
        AssertEx.SequenceEqual(new[] { 101 }, fixture.Relay.MenuProcesses);
        await fixture.CallAsync("ResumeSteamInvitesFromMenuAsync");
        await fixture.CallAsync("HandleHelloAsync", hello);
        AssertEx.SequenceEqual(new[] { 101 }, fixture.Relay.MenuProcesses, "Menu heartbeats must not restart Steam.");
        fixture.Ready(202, false);
        await fixture.CallAsync("ResumeSteamInvitesFromMenuAsync");
        AssertEx.SequenceEqual(new[] { 101, 202 }, fixture.Relay.MenuProcesses,
            "A new game process must reactivate Spacewar after its menu is ready.");
    }

    internal static async Task GameplayDoesNotActivateAsync()
    {
        using var fixture = new Fixture();
        fixture.Ready(101, false);
        typeof(MultiplayerSessionCoordinator).GetField("_uiGameplayLoaded", Private)!.SetValue(fixture.Coordinator, true);
        await fixture.CallAsync("ResumeSteamInvitesFromMenuAsync");
        AssertEx.Equal(0, fixture.Relay.MenuProcesses.Count, "Gameplay must not trigger a new Steam activation.");
    }

    internal static async Task WorkerWaitsForMenuAsync()
    {
        using var directory = new TemporaryDirectory();
        using var diagnostics = new LauncherDiagnostics(directory.Combine("logs"));
        using var relay = new SteamRelay(diagnostics);
        await AssertEx.ThrowsAsync<InvalidOperationException>(() => relay.StartHostingAsync(28015, "too early").WaitAsync(TimeSpan.FromSeconds(2)));
        relay.Start();
        AssertEx.False(await relay.StartHostingAsync(28015, "still too early").WaitAsync(TimeSpan.FromSeconds(2)));
        AssertEx.False(relay.IsReady);
        AssertEx.Equal(0UL, relay.LocalSteamId);
    }

    // Facepunch 2.3.3 ConnectionInfo.Identity returns the address, so Friends Only
    // lobbies saw every joiner as SteamID 0 and refused them (client: "App_Min").
    internal static Task ConnectionInfoIdentityAsync()
    {
        const ulong friend = 76561198000000002;
        const BindingFlags fields = BindingFlags.Instance | BindingFlags.NonPublic | BindingFlags.Public;
        AssertEx.True(SteamConnectionInfo.IsSupported, "Facepunch ConnectionInfo layout changed; review SteamConnectionInfo.");
        object boxed = default(Steamworks.Data.ConnectionInfo);
        var type = typeof(Steamworks.Data.ConnectionInfo);
        type.GetField("identity", fields)!.SetValue(boxed, (Steamworks.Data.NetIdentity)new Steamworks.SteamId { Value = friend });
        type.GetField("endReason", fields)!.SetValue(boxed, 1000);
        type.GetField("endDebug", fields)!.SetValue(boxed, "not a Steam friend and the lobby is Friends");
        var info = (Steamworks.Data.ConnectionInfo)boxed;

        AssertEx.Equal(friend, SteamConnectionInfo.RemoteSteamId(info).Value, "remote SteamID read from the identity field");
        AssertEx.Equal("host refused the connection: not a Steam friend and the lobby is Friends", SteamRelay.DescribeHostClose(info));
        AssertEx.Equal(0UL, SteamConnectionInfo.RemoteSteamId(default).Value);
        AssertEx.Equal("Invalid", SteamRelay.DescribeHostClose(default));
        return Task.CompletedTask;
    }

    private sealed class FakeRelay : ISteamRelay
    {
        public event Action<string, string>? JoinRequested;
        public ulong LocalSteamId => HostId + 10;
        internal int Port, OpenCount, CloseCount;
        internal ulong LastHost;
        internal Exception? Failure;
        internal TaskCompletionSource? Delay;
        internal readonly TaskCompletionSource Entered = new(TaskCreationOptions.RunContinuationsAsynchronously);
        internal bool Disposed;
        internal readonly List<int> MenuProcesses = new();
        internal void Raise(string friend, string address) => JoinRequested?.Invoke(friend, address);
        public Task<bool> ActivateForMenuAsync(int gameProcessId)
        {
            MenuProcesses.Add(gameProcessId);
            return Task.FromResult(true);
        }
        public Task<bool> StartHostingAsync(int port, string status) => Task.FromResult(true);
        public Task StopHostingAsync() => Task.CompletedTask;
        public async Task<int> OpenClientTunnelAsync(ulong host, CancellationToken token)
        {
            token.ThrowIfCancellationRequested();
            OpenCount++;
            LastHost = host;
            Entered.TrySetResult();
            if (Delay is not null) await Delay.Task.WaitAsync(token);
            if (Failure is not null) throw Failure;
            return Port;
        }
        public void CloseAllTunnels(string reason) => CloseCount++;
        public void Dispose() => Disposed = true;
    }

    private sealed class Fixture : IDisposable
    {
        private readonly TemporaryDirectory _directory = new();
        private readonly LauncherDiagnostics _diagnostics;
        private readonly NativeRuntimeController _controller;
        private readonly ScaleformBridgeListener _bridge;
        internal readonly LauncherSettingsStore Settings;
        internal readonly FakeRelay Relay = new();
        internal readonly MultiplayerSessionCoordinator Coordinator;

        internal Fixture()
        {
            _diagnostics = new LauncherDiagnostics(_directory.Combine("logs"));
            Settings = new LauncherSettingsStore(_directory.Combine("settings.json"));
            Settings.Save(new LauncherSettings(string.Empty, "Dallas", "192.0.2.42", 29000));
            _controller = new NativeRuntimeController(_diagnostics, Settings);
            _bridge = new ScaleformBridgeListener(_diagnostics);
            Coordinator = new MultiplayerSessionCoordinator(_controller, Settings, _bridge, new PerformanceIsolationState(), steamRelay: Relay);
        }

        internal LobbyHost Host() => new(new LobbyHostOptions("Steam test", "Ripley", Guid.NewGuid(),
            0, 4, "TestLevel", LobbyCompatibility.Create(Settings.Load())));
        internal void Ready(int processId = 123, bool frontendReady = true)
        {
            typeof(NativeRuntimeController).GetField("_snapshot", Private)!.SetValue(_controller,
                NativeRuntimeSnapshot.Empty with { IpcConnected = true, ProcessId = processId });
            typeof(MultiplayerSessionCoordinator).GetField("_steamFrontendReady", Private)!.SetValue(Coordinator, frontendReady);
        }
        internal T Get<T>(string field) => (T)typeof(MultiplayerSessionCoordinator).GetField(field, Private)!.GetValue(Coordinator)!;
        internal Task CallAsync(string method, params object[] arguments) => (Task)typeof(MultiplayerSessionCoordinator).GetMethod(method, Private)!.Invoke(Coordinator, arguments)!;
        public void Dispose()
        {
            Coordinator.Dispose();
            _bridge.Dispose();
            _controller.Dispose();
            _diagnostics.Dispose();
            _directory.Dispose();
        }
    }
}
