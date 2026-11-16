using System.Collections.Concurrent;
using System.Globalization;
using System.Net;
using System.Net.Sockets;
using System.Runtime.InteropServices;
using Steamworks;
using Steamworks.Data;
using ConnectionInfo = Steamworks.Data.ConnectionInfo;

namespace AIMP.Launcher;

/// <summary>
/// Carries the existing AIMP TCP protocol over Steam Datagram Relay (app 480,
/// Spacewar) so players can host without port forwarding.
///
/// Nothing in the lobby protocol changes. The host keeps its normal TCP
/// listener; every incoming Steam connection is bridged to 127.0.0.1:port.
/// A client that joins "steam:&lt;SteamID64&gt;" gets a one-shot loopback TCP
/// listener that is bridged to the host's Steam connection, and LobbyClient
/// connects to that listener as if it were a direct host.
///
/// All Steam API calls except Connection.SendMessage (thread-safe in
/// SteamNetworkingSockets) run on one dedicated pump thread.
/// </summary>
internal sealed partial class SteamRelay : ISteamRelay, ISteamLobbies
{
    internal const string AddressPrefix = "steam:";
    private const uint SpacewarAppId = 480;
    private const int VirtualPort = 7856;
    private const int ChunkSize = 64 * 1024;
    private const int SendBufferBytes = 8 * 1024 * 1024;
    private const int MaxHostTunnels = 16;
    private static readonly TimeSpan ConnectTimeout = TimeSpan.FromSeconds(25);
    private static readonly TimeSpan InitRetryInterval = TimeSpan.FromSeconds(10);

    private readonly SteamRelayLog _diagnostics;
    private readonly BlockingCollection<Action> _work = new();
    private readonly object _gate = new();
    private readonly List<RelayTunnel> _tunnels = new();
    private Thread? _pump;
    private HostRelaySocket? _hostSocket;
    private int _hostPort;
    private DateTime _nextInitAttempt = DateTime.MinValue;
    private volatile bool _ready;
    private bool _stopping;
    private volatile bool _pumpExit;
    private bool _menuEnabled;
    private int _menuProcessId;

    internal SteamRelay(LauncherDiagnostics diagnostics)
        : this(new SteamRelayLog(diagnostics is null ? throw new ArgumentNullException(nameof(diagnostics)) : diagnostics.Log))
    {
    }

    // The Steam helper process forwards its log lines to the launcher instead of writing a launcher log.
    internal SteamRelay(SteamRelayLog log)
    {
        _diagnostics = log ?? throw new ArgumentNullException(nameof(log));
    }

    /// <summary>Raised (on a worker thread) when the user accepts a Steam
    /// invite or clicks Join Game on a friend. Argument: "steam:&lt;id&gt;".</summary>
    public event Action<string, string>? JoinRequested;

    internal bool IsReady => _ready;

    public ulong LocalSteamId { get; private set; }

    internal static bool IsSteamAddress(string? host) =>
        host is not null && host.StartsWith(AddressPrefix, StringComparison.OrdinalIgnoreCase);

    internal static bool TryParseAddress(string? host, out ulong steamId)
    {
        steamId = 0;
        if (!IsSteamAddress(host)) return false;
        return ulong.TryParse(host![AddressPrefix.Length..].Trim(), NumberStyles.None,
            CultureInfo.InvariantCulture, out steamId)
            && (steamId >> 56) == 1 // public universe
            && ((steamId >> 52) & 15) == 1 // individual account
            && ((steamId >> 32) & 0xfffff) == 1 // desktop instance
            && (uint)steamId != 0;
    }

    internal static string FormatAddress(ulong steamId) =>
        AddressPrefix + steamId.ToString(CultureInfo.InvariantCulture);

    /// <summary>Starts the worker without initializing Steam before the game menu is ready.</summary>
    internal void Start()
    {
        lock (_gate)
        {
            ObjectDisposedException.ThrowIf(_stopping, this);
            if (_pump is not null) return;
            _pump = new Thread(PumpLoop) { IsBackground = true, Name = "AIMP Steam relay" };
            _pump.Start();
        }
        AppDomain.CurrentDomain.ProcessExit += OnProcessExit;
    }

    public Task<bool> ActivateForMenuAsync(int gameProcessId)
    {
        if (gameProcessId <= 0) throw new ArgumentOutOfRangeException(nameof(gameProcessId));
        Start();
        return RunOnPumpAsync(() =>
        {
            if (_menuEnabled && _menuProcessId == gameProcessId) return _ready;
            if (_menuEnabled) ShutdownOnPump();
            _menuEnabled = true;
            _menuProcessId = gameProcessId;
            _diagnostics.Log("Steam", $"Game menu ready for PID={gameProcessId}; activating Spacewar relay after Alien: Isolation startup.");
            return EnsureInitialized(force: true);
        });
    }

    /// <summary>Accepts Steam relay connections and bridges them to the local lobby port.</summary>
    // Without a visibility (no Steam lobby advert) the relay accepts anyone, as before.
    public Task<bool> StartHostingAsync(int lobbyPort, string statusText) => RunOnPumpAsync(() =>
    {
        if (!EnsureInitialized(force: true)) return false;
        StartHostingOnPump(lobbyPort, SteamLobbyVisibility.Public);
        var address = FormatAddress(LocalSteamId);
        SteamFriends.SetRichPresence("connect", address);
        SteamFriends.SetRichPresence("status", statusText);
        return true;
    });

    private void StartHostingOnPump(int lobbyPort, SteamLobbyVisibility visibility)
    {
        StopHostingOnPump();
        _lobbyVisibility = visibility; // relay access checks use it from the first connection
        _hostPort = lobbyPort;
        _hostSocket = SteamNetworkingSockets.CreateRelaySocket<HostRelaySocket>(VirtualPort);
        _hostSocket.Owner = this;
        var address = FormatAddress(LocalSteamId);
        _diagnostics.Log("Steam", $"Hosting over Steam relay (Spacewar 480). Friends can join with {address} or via your Steam friends list.");
    }

    public Task StopHostingAsync() => RunOnPumpAsync(() => { StopHostingOnPump(); return true; });

    public Task ReleaseAfterGameExitAsync() => RunOnPumpAsync(() =>
    {
        if (!_menuEnabled) return false;
        _diagnostics.Log("Steam", $"Alien: Isolation PID={_menuProcessId} exited; releasing Spacewar until the next game menu.");
        ShutdownOnPump();
        _menuProcessId = 0;
        return true;
    });

    /// <summary>Connects to a host over Steam relay and returns a loopback port
    /// that LobbyClient can connect to. Throws when the relay cannot connect.</summary>
    public async Task<int> OpenClientTunnelAsync(ulong hostSteamId, CancellationToken cancellationToken)
    {
        var tunnel = await RunOnPumpAsync(() => ConnectOnPump(hostSteamId)).ConfigureAwait(false);
        var listener = new TcpListener(IPAddress.Loopback, 0);
        try
        {
            listener.Start(1);
            await WaitForSteamAsync(tunnel, hostSteamId, cancellationToken).ConfigureAwait(false);
            _ = AcceptClientAsync(listener, tunnel);
            return ((IPEndPoint)listener.LocalEndpoint).Port;
        }
        catch
        {
            listener.Stop();
            tunnel.Close("connect failed");
            throw;
        }
    }

    private RelayTunnel ConnectOnPump(ulong hostSteamId)
    {
        if (!EnsureInitialized(force: true))
            throw new InvalidOperationException("Steam is not running or not signed in. Start Steam and try again.");
        if (hostSteamId == LocalSteamId)
            throw new InvalidOperationException("That Steam ID is your own account; Steam cannot relay to itself.");
        var manager = SteamNetworkingSockets.ConnectRelay<ClientRelayConnection>(hostSteamId, VirtualPort);
        var tunnel = new RelayTunnel(this, manager.Connection, $"host {hostSteamId}") { ClientManager = manager };
        manager.Tunnel = tunnel;
        Track(tunnel);
        _clientHostSteamId = hostSteamId; // lets a joined player invite friends to this host
        return tunnel;
    }

    private async Task WaitForSteamAsync(RelayTunnel tunnel, ulong hostSteamId, CancellationToken cancellationToken)
    {
        _diagnostics.Log("Steam", $"Connecting to steam:{hostSteamId} through Steam relay...");
        using var timeout = CancellationTokenSource.CreateLinkedTokenSource(cancellationToken);
        timeout.CancelAfter(ConnectTimeout);
        try { await tunnel.SteamConnected.Task.WaitAsync(timeout.Token).ConfigureAwait(false); }
        catch (OperationCanceledException) when (!cancellationToken.IsCancellationRequested)
        {
            throw new TimeoutException($"Could not reach steam:{hostSteamId} through Steam relay. "
                + "Check the host has a lobby open in the modified launcher and Steam is running.");
        }
        _diagnostics.Log("Steam", $"Steam relay connected to steam:{hostSteamId}.");
    }

    public void CloseAllTunnels(string reason)
    {
        RelayTunnel[] tunnels;
        lock (_gate) tunnels = _tunnels.ToArray();
        foreach (var tunnel in tunnels) tunnel.Close(reason);
    }

    private async Task AcceptClientAsync(TcpListener listener, RelayTunnel tunnel)
    {
        try
        {
            using var timeout = new CancellationTokenSource(TimeSpan.FromSeconds(15));
            var client = await listener.AcceptTcpClientAsync(timeout.Token).ConfigureAwait(false);
            tunnel.AttachTcp(client);
        }
        catch (Exception exception)
        {
            tunnel.Close($"local accept failed: {exception.Message}");
        }
        finally
        {
            listener.Stop();
        }
    }

    // ---- pump thread ---------------------------------------------------

    private void PumpLoop()
    {
        // Continuations of async Steam calls (lobby search/join/create) resume here.
        SynchronizationContext.SetSynchronizationContext(new PumpSynchronizationContext(_work));
        while (!_pumpExit)
        {
            try { PumpOnce(); }
            catch (Exception exception)
            {
                _diagnostics.Log("Steam", $"Relay pump error: {exception.Message}");
                Thread.Sleep(50);
            }
        }
    }

    private void PumpOnce()
    {
        while (!_pumpExit && _work.TryTake(out var action, _ready ? 1 : 250)) action();
        if (_pumpExit) return;
        if (!_ready) { if (_menuEnabled) EnsureInitialized(force: false); return; }
        SteamClient.RunCallbacks();
        _hostSocket?.Receive(256);
        RelayTunnel[] tunnels;
        lock (_gate) tunnels = _tunnels.ToArray();
        foreach (var tunnel in tunnels) tunnel.ClientManager?.Receive(256);
    }

    private Task<T> RunOnPumpAsync<T>(Func<T> action)
    {
        var completion = new TaskCompletionSource<T>(TaskCreationOptions.RunContinuationsAsynchronously);
        lock (_gate)
        {
            if (_stopping) return Task.FromException<T>(new ObjectDisposedException(nameof(SteamRelay)));
            if (_pump is null) return Task.FromException<T>(new InvalidOperationException("Steam relay is waiting for the game menu to be ready."));
            _work.Add(() =>
            {
                try { completion.TrySetResult(action()); }
                catch (Exception exception) { completion.TrySetException(exception); }
            });
        }
        return completion.Task;
    }

    // Starts an async operation on the pump thread; its awaits resume on the pump.
    private Task<T> RunOnPumpAwaitAsync<T>(Func<Task<T>> action)
    {
        var completion = new TaskCompletionSource<T>(TaskCreationOptions.RunContinuationsAsynchronously);
        lock (_gate)
        {
            if (_stopping) return Task.FromException<T>(new ObjectDisposedException(nameof(SteamRelay)));
            if (_pump is null) return Task.FromException<T>(new InvalidOperationException("Steam relay is waiting for the game menu to be ready."));
            _work.Add(async () =>
            {
                try { completion.TrySetResult(await action()); }
                catch (Exception exception) { completion.TrySetException(exception); }
            });
        }
        return completion.Task;
    }

    private sealed class PumpSynchronizationContext(BlockingCollection<Action> work) : SynchronizationContext
    {
        public override void Post(SendOrPostCallback callback, object? state)
        {
            // After shutdown the queue is closed; late continuations are dropped.
            try { work.Add(() => callback(state)); }
            catch (InvalidOperationException) { }
        }

        public override SynchronizationContext CreateCopy() => this;
    }

    private bool EnsureInitialized(bool force)
    {
        if (!_menuEnabled) return false;
        if (_ready) return true;
        if (!force && DateTime.UtcNow < _nextInitAttempt) return false;
        _nextInitAttempt = DateTime.UtcNow + InitRetryInterval;
        try
        {
            SteamAppEnvironment.Initialize(() => SteamClient.Init(SpacewarAppId, asyncCallbacks: false));
            ConfigureAfterInit();
        }
        catch (Exception exception)
        {
            if (force) _diagnostics.Log("Steam", $"Steam relay unavailable: {exception.Message} ({DescribeInitEnvironment()})");
            if (SteamClient.IsValid)
            {
                UnhookSteamCallbacks();
                SteamClient.Shutdown();
            }
            _ready = false;
            return false;
        }
        return true;
    }

    // Why SteamAPI_Init can fail: no Steam process for this Windows user, or an admin/non-admin mismatch
    // between Steam and this process (Steam IPC refuses it).
    private static string DescribeInitEnvironment()
    {
        try
        {
            var steamRunning = System.Diagnostics.Process.GetProcessesByName("steam").Length > 0;
            using var identity = System.Security.Principal.WindowsIdentity.GetCurrent();
            var elevated = new System.Security.Principal.WindowsPrincipal(identity)
                .IsInRole(System.Security.Principal.WindowsBuiltInRole.Administrator);
            return $"steam.exe running={steamRunning}, this process elevated={elevated}, pid={Environment.ProcessId}";
        }
        catch (Exception exception) { return $"environment check failed: {exception.Message}"; }
    }

    private void ConfigureAfterInit()
    {
        LocalSteamId = SteamClient.SteamId.Value;
        SteamNetworkingUtils.SendBufferSize = SendBufferBytes;
        SteamNetworkingUtils.InitRelayNetworkAccess();
        SteamFriends.OnGameRichPresenceJoinRequested += OnRichPresenceJoinRequested;
        HookLobbyCallbacks();
        _ready = true;
        ApplyPresence();
        _diagnostics.Log("Steam", $"Steam relay ready as {FormatAddress(LocalSteamId)} ({SteamClient.Name}).");
    }

    private void OnRichPresenceJoinRequested(Friend friend, string connect) => RaiseJoin(friend.Name, connect);

    private void RaiseJoin(string friendName, string connect)
    {
        var address = connect?.Trim() ?? string.Empty;
        if (!TryParseAddress(address, out _))
        {
            _diagnostics.Log("Steam", $"Ignored Steam join request with unknown address '{address}'.");
            return;
        }
        var handler = JoinRequested;
        if (handler is not null) Task.Run(() => handler(friendName, address));
    }

    // Also leaves the Steam lobby (hosted or joined) and resets rich presence.
    private void StopHostingOnPump()
    {
        LeaveLobbyOnPump();
        if (_hostSocket is null) return;
        RelayTunnel[] hosted;
        lock (_gate) hosted = _tunnels.Where(t => t.ClientManager is null).ToArray();
        foreach (var tunnel in hosted) tunnel.Close("host stopped");
        try { _hostSocket.Close(); } catch (Exception exception) { _diagnostics.Log("Steam", $"Relay socket close: {exception.Message}"); }
        _hostSocket = null;
        if (_ready) ApplyPresence();
        _diagnostics.Log("Steam", "Stopped hosting over Steam relay.");
    }

    private void OnProcessExit(object? sender, EventArgs args) => Dispose();

    public void Dispose()
    {
        Thread? pump;
        lock (_gate)
        {
            if (_stopping) return;
            _stopping = true;
            pump = _pump;
            if (pump is not null) _work.Add(() =>
            {
                try { ShutdownOnPump(); }
                finally { _pumpExit = true; }
            });
        }
        AppDomain.CurrentDomain.ProcessExit -= OnProcessExit;
        if (pump is not null && pump != Thread.CurrentThread && !pump.Join(TimeSpan.FromSeconds(5)))
            _diagnostics.Log("Steam", "Relay pump did not stop within five seconds.");
    }

    private void ShutdownOnPump()
    {
        try
        {
            CloseAllTunnels("launcher exit");
            StopHostingOnPump();
            if (SteamClient.IsValid)
            {
                UnhookSteamCallbacks();
                SteamFriends.ClearRichPresence();
                SteamClient.Shutdown();
            }
        }
        catch
        {
            // Process is exiting; Steam cleans up after us.
        }
        _ready = false;
        _menuEnabled = false;
    }

    private void Track(RelayTunnel tunnel) { lock (_gate) _tunnels.Add(tunnel); }

    private void Untrack(RelayTunnel tunnel) { lock (_gate) _tunnels.Remove(tunnel); }

    // Host side: called on the pump thread from HostRelaySocket callbacks.
    private void OnHostConnected(Connection connection, ConnectionInfo info)
    {
        var peer = SteamConnectionInfo.RemoteSteamId(info).Value;
        var tunnel = new RelayTunnel(this, connection, $"peer {peer}");
        connection.UserData = tunnel.Id;
        Track(tunnel);
        _diagnostics.Log("Steam", $"Steam relay peer {peer} connected; bridging to lobby port {_hostPort}.");
        tunnel.SteamConnected.TrySetResult();
        var port = _hostPort;
        _ = Task.Run(() => BridgeToLobbyAsync(tunnel, port));
    }

    private static async Task BridgeToLobbyAsync(RelayTunnel tunnel, int port)
    {
        var client = new TcpClient(AddressFamily.InterNetwork);
        try
        {
            await client.ConnectAsync(IPAddress.Loopback, port).ConfigureAwait(false);
            tunnel.AttachTcp(client);
        }
        catch (Exception exception)
        {
            client.Dispose();
            tunnel.Close($"lobby port {port} refused: {exception.Message}");
        }
    }

    private RelayTunnel? FindHostTunnel(Connection connection)
    {
        lock (_gate) return _tunnels.FirstOrDefault(t => t.ClientManager is null && t.Id == connection.UserData);
    }

    private sealed class HostRelaySocket : SocketManager
    {
        internal SteamRelay? Owner { get; set; }

        public override void OnConnecting(Connection connection, ConnectionInfo info)
        {
            // Not info.Identity: Facepunch 2.3.3 returns the address there (always SteamID 0).
            var peer = SteamConnectionInfo.RemoteSteamId(info);
            if (Owner?.RejectPeer(peer) is { } reason)
            {
                Owner._diagnostics.Log("Steam", $"Rejected Steam relay peer {peer.Value}: {reason}.");
                connection.Close(false, 0, reason);
                return;
            }
            Owner?._diagnostics.Log("Steam", $"Steam relay peer {peer.Value} connecting.");
            base.OnConnecting(connection, info);
        }

        public override void OnConnected(Connection connection, ConnectionInfo info)
        {
            base.OnConnected(connection, info);
            Owner?.OnHostConnected(connection, info);
        }

        public override void OnDisconnected(Connection connection, ConnectionInfo info)
        {
            Owner?.FindHostTunnel(connection)?.Close($"Steam peer left ({info.EndReason})", closeSteam: false);
            base.OnDisconnected(connection, info);
        }

        public override void OnMessage(Connection connection, NetIdentity identity, IntPtr data,
            int size, long messageNum, long recvTime, int channel) =>
            Owner?.FindHostTunnel(connection)?.DeliverFromSteam(data, size);
    }

    private sealed class ClientRelayConnection : ConnectionManager
    {
        internal RelayTunnel? Tunnel { get; set; }

        public override void OnConnected(ConnectionInfo info)
        {
            base.OnConnected(info);
            Tunnel?.SteamConnected.TrySetResult();
        }

        public override void OnDisconnected(ConnectionInfo info)
        {
            var reason = DescribeHostClose(info);
            Tunnel?.SteamConnected.TrySetException(new IOException($"Steam relay refused: {reason}"));
            Tunnel?.Close($"Steam host left ({reason})", closeSteam: false);
            base.OnDisconnected(info);
        }

        public override void OnMessage(IntPtr data, int size, long messageNum, long recvTime, int channel) =>
            Tunnel?.DeliverFromSteam(data, size);
    }

    // An application close (App_Min..App_Max) carries the host's own reason text,
    // e.g. "not a Steam friend and the lobby is Friends"; show it instead of "App_Min".
    internal static string DescribeHostClose(ConnectionInfo info)
    {
        var code = (int)info.EndReason;
        var debug = SteamConnectionInfo.EndDebug(info);
        return code is >= 1000 and <= 1999 && debug.Length > 0 ? $"host refused the connection: {debug}" : info.EndReason.ToString();
    }

    /// <summary>One Steam connection bridged to one loopback TCP connection.</summary>
    private sealed class RelayTunnel
    {
        private static long _nextId;
        private readonly SteamRelay _owner;
        private readonly Connection _connection;
        private readonly string _label;
        private readonly SteamRelayReceiveQueue _toTcp = new();
        private readonly object _tcpGate = new();
        private readonly CancellationTokenSource _closed = new();
        private TcpClient? _tcp;
        private int _isClosed;

        internal RelayTunnel(SteamRelay owner, Connection connection, string label)
        {
            _owner = owner;
            _connection = connection;
            _label = label;
            Id = Interlocked.Increment(ref _nextId);
        }

        internal long Id { get; }

        internal ClientRelayConnection? ClientManager { get; set; }

        internal TaskCompletionSource SteamConnected { get; } =
            new(TaskCreationOptions.RunContinuationsAsynchronously);

        internal void AttachTcp(TcpClient tcp)
        {
            NetworkStream stream;
            lock (_tcpGate)
            {
                if (Volatile.Read(ref _isClosed) != 0) { tcp.Dispose(); return; }
                tcp.NoDelay = true;
                _tcp = tcp;
                stream = tcp.GetStream();
            }
            _ = PumpTcpToSteamAsync(stream);
            _ = PumpSteamToTcpAsync(stream);
        }

        // Pump thread: copy out of Steam's buffer before the callback returns.
        internal void DeliverFromSteam(IntPtr data, int size)
        {
            if (size <= 0 || Volatile.Read(ref _isClosed) != 0) return;
            if (size > SteamRelayReceiveQueue.MaximumMessageBytes)
            {
                Close("oversized Steam message");
                return;
            }
            var bytes = new byte[size];
            Marshal.Copy(data, bytes, 0, size);
            if (!_toTcp.TryWrite(bytes)) Close("Steam receive queue exceeded capacity");
        }

        private async Task PumpTcpToSteamAsync(NetworkStream stream)
        {
            var buffer = new byte[ChunkSize];
            var token = _closed.Token;
            try
            {
                int read;
                while ((read = await stream.ReadAsync(buffer, token).ConfigureAwait(false)) > 0)
                    await SendReliableAsync(buffer, read, token).ConfigureAwait(false);
            }
            catch (Exception exception) when (exception is IOException or SocketException
                or ObjectDisposedException or OperationCanceledException)
            {
                // Fall through: either side ending the stream closes the tunnel.
            }
            Close("local connection closed");
        }

        // The lobby protocol is a reliable byte stream: a full Steam send queue
        // waits instead of dropping (voice already drops before reaching TCP).
        private async Task SendReliableAsync(byte[] buffer, int count, CancellationToken token)
        {
            while (true)
            {
                var result = _connection.SendMessage(buffer, 0, count, SendType.Reliable | SendType.NoNagle);
                if (result == Result.OK) return;
                if (result != Result.LimitExceeded)
                    throw new IOException($"Steam relay send failed: {result}");
                await Task.Delay(2, token).ConfigureAwait(false);
            }
        }

        private async Task PumpSteamToTcpAsync(NetworkStream stream)
        {
            try
            {
                await foreach (var bytes in _toTcp.ReadAllAsync(_closed.Token).ConfigureAwait(false))
                    await stream.WriteAsync(bytes, _closed.Token).ConfigureAwait(false);
            }
            catch (Exception exception) when (exception is IOException or SocketException
                or ObjectDisposedException or OperationCanceledException)
            {
                Close("local write failed");
            }
        }

        internal void Close(string reason, bool closeSteam = true)
        {
            if (Interlocked.Exchange(ref _isClosed, 1) != 0) return;
            _owner.Untrack(this);
            _owner._diagnostics.Log("Steam", $"Relay tunnel to {_label} closed: {reason}.");
            _toTcp.Complete();
            _closed.Cancel();
            lock (_tcpGate)
                try { _tcp?.Dispose(); } catch (ObjectDisposedException) { }
            SteamConnected.TrySetException(new IOException(reason));
            _ = SteamConnected.Task.Exception; // observed
            if (closeSteam)
            {
                if (Thread.CurrentThread == _owner._pump) CloseSteamOnPump(reason);
                else lock (_owner._gate)
                    if (!_owner._stopping) _owner._work.Add(() => CloseSteamOnPump(reason));
            }
        }

        private void CloseSteamOnPump(string reason)
        {
            try
            {
                if (ClientManager is not null) ClientManager.Close();
                else _connection.Close(false, 0, reason);
            }
            catch (Exception exception)
            {
                _owner._diagnostics.Log("Steam", $"Relay close: {exception.Message}");
            }
        }
    }
}

internal sealed class SteamRelayLog(Action<string, string> write)
{
    internal void Log(string category, string message) => write(category, message);
}
