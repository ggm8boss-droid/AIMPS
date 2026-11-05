using System.Collections.Concurrent;
using System.Globalization;
using System.Net;
using System.Net.Sockets;
using System.Runtime.InteropServices;
using System.Threading.Channels;
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
internal sealed class SteamRelay
{
    internal const string AddressPrefix = "steam:";
    private const uint SpacewarAppId = 480;
    private const int VirtualPort = 7856;
    private const int ChunkSize = 64 * 1024;
    private const int SendBufferBytes = 8 * 1024 * 1024;
    private static readonly TimeSpan ConnectTimeout = TimeSpan.FromSeconds(25);
    private static readonly TimeSpan InitRetryInterval = TimeSpan.FromSeconds(10);

    private readonly LauncherDiagnostics _diagnostics;
    private readonly BlockingCollection<Action> _work = new();
    private readonly object _gate = new();
    private readonly List<RelayTunnel> _tunnels = new();
    private Thread? _pump;
    private HostRelaySocket? _hostSocket;
    private int _hostPort;
    private DateTime _nextInitAttempt = DateTime.MinValue;
    private volatile bool _ready;

    internal SteamRelay(LauncherDiagnostics diagnostics)
    {
        _diagnostics = diagnostics ?? throw new ArgumentNullException(nameof(diagnostics));
    }

    /// <summary>Raised (on a worker thread) when the user accepts a Steam
    /// invite or clicks Join Game on a friend. Argument: "steam:&lt;id&gt;".</summary>
    internal event Action<string, string>? JoinRequested;

    internal bool IsReady => _ready;

    internal SteamId LocalSteamId { get; private set; }

    internal static bool IsSteamAddress(string? host) =>
        host is not null && host.StartsWith(AddressPrefix, StringComparison.OrdinalIgnoreCase);

    internal static bool TryParseAddress(string? host, out ulong steamId)
    {
        steamId = 0;
        if (!IsSteamAddress(host)) return false;
        return ulong.TryParse(host![AddressPrefix.Length..].Trim(), NumberStyles.None,
            CultureInfo.InvariantCulture, out steamId) && (steamId >> 56) == 1; // individual account universe
    }

    internal static string FormatAddress(ulong steamId) =>
        AddressPrefix + steamId.ToString(CultureInfo.InvariantCulture);

    /// <summary>Starts the pump thread and attempts Steam initialization.
    /// Never throws; Steam being closed just leaves the relay unavailable.</summary>
    internal void Start()
    {
        lock (_gate)
        {
            if (_pump is not null) return;
            _pump = new Thread(PumpLoop) { IsBackground = true, Name = "AIMP Steam relay" };
            _pump.Start();
        }
        AppDomain.CurrentDomain.ProcessExit += (_, _) => Shutdown();
    }

    /// <summary>Accepts Steam relay connections and bridges them to the local lobby port.</summary>
    internal Task<bool> StartHostingAsync(int lobbyPort, string statusText) => RunOnPumpAsync(() =>
    {
        if (!EnsureInitialized(force: true)) return false;
        StopHostingOnPump();
        _hostPort = lobbyPort;
        _hostSocket = SteamNetworkingSockets.CreateRelaySocket<HostRelaySocket>(VirtualPort);
        _hostSocket.Owner = this;
        var address = FormatAddress(LocalSteamId);
        SteamFriends.SetRichPresence("connect", address);
        SteamFriends.SetRichPresence("status", statusText);
        _diagnostics.Log("Steam", $"Hosting over Steam relay (Spacewar 480). Friends can join with {address} or via your Steam friends list.");
        return true;
    });

    internal Task StopHostingAsync() => RunOnPumpAsync(() => { StopHostingOnPump(); return true; });

    /// <summary>Connects to a host over Steam relay and returns a loopback port
    /// that LobbyClient can connect to. Throws when the relay cannot connect.</summary>
    internal async Task<int> OpenClientTunnelAsync(ulong hostSteamId, CancellationToken cancellationToken)
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
        if (hostSteamId == LocalSteamId.Value)
            throw new InvalidOperationException("That Steam ID is your own account; Steam cannot relay to itself.");
        var manager = SteamNetworkingSockets.ConnectRelay<ClientRelayConnection>(hostSteamId, VirtualPort);
        var tunnel = new RelayTunnel(this, manager.Connection, $"host {hostSteamId}") { ClientManager = manager };
        manager.Tunnel = tunnel;
        Track(tunnel);
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

    internal void CloseAllTunnels(string reason)
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
        EnsureInitialized(force: true);
        while (true)
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
        while (_work.TryTake(out var action, _ready ? 1 : 250)) action();
        if (!_ready) { EnsureInitialized(force: false); return; }
        SteamClient.RunCallbacks();
        _hostSocket?.Receive(256);
        RelayTunnel[] tunnels;
        lock (_gate) tunnels = _tunnels.ToArray();
        foreach (var tunnel in tunnels) tunnel.ClientManager?.Receive(256);
    }

    private Task<T> RunOnPumpAsync<T>(Func<T> action)
    {
        var completion = new TaskCompletionSource<T>(TaskCreationOptions.RunContinuationsAsynchronously);
        _work.Add(() =>
        {
            try { completion.TrySetResult(action()); }
            catch (Exception exception) { completion.TrySetException(exception); }
        });
        return completion.Task;
    }

    private bool EnsureInitialized(bool force)
    {
        if (_ready) return true;
        if (!force && DateTime.UtcNow < _nextInitAttempt) return false;
        _nextInitAttempt = DateTime.UtcNow + InitRetryInterval;
        try { SteamClient.Init(SpacewarAppId, asyncCallbacks: false); }
        catch (Exception exception)
        {
            if (force) _diagnostics.Log("Steam", $"Steam relay unavailable: {exception.Message}");
            return false;
        }
        ConfigureAfterInit();
        return true;
    }

    private void ConfigureAfterInit()
    {
        LocalSteamId = SteamClient.SteamId;
        SteamNetworkingUtils.SendBufferSize = SendBufferBytes;
        SteamNetworkingUtils.InitRelayNetworkAccess();
        SteamFriends.OnGameRichPresenceJoinRequested += (friend, connect) =>
            RaiseJoin(friend.Name, connect);
        _ready = true;
        _diagnostics.Log("Steam", $"Steam relay ready as {FormatAddress(LocalSteamId)} ({SteamClient.Name}).");
    }

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

    private void StopHostingOnPump()
    {
        if (_hostSocket is null) return;
        RelayTunnel[] hosted;
        lock (_gate) hosted = _tunnels.Where(t => t.ClientManager is null).ToArray();
        foreach (var tunnel in hosted) tunnel.Close("host stopped");
        try { _hostSocket.Close(); } catch (Exception exception) { _diagnostics.Log("Steam", $"Relay socket close: {exception.Message}"); }
        _hostSocket = null;
        if (_ready) SteamFriends.ClearRichPresence();
        _diagnostics.Log("Steam", "Stopped hosting over Steam relay.");
    }

    private void Shutdown()
    {
        if (!_ready) return;
        try
        {
            CloseAllTunnels("launcher exit");
            SteamFriends.ClearRichPresence();
            SteamClient.Shutdown();
        }
        catch
        {
            // Process is exiting; Steam cleans up after us.
        }
        _ready = false;
    }

    private void Track(RelayTunnel tunnel) { lock (_gate) _tunnels.Add(tunnel); }

    private void Untrack(RelayTunnel tunnel) { lock (_gate) _tunnels.Remove(tunnel); }

    // Host side: called on the pump thread from HostRelaySocket callbacks.
    private void OnHostConnected(Connection connection, ConnectionInfo info)
    {
        var tunnel = new RelayTunnel(this, connection, $"peer {info.Identity.SteamId.Value}");
        connection.UserData = tunnel.Id;
        Track(tunnel);
        _diagnostics.Log("Steam", $"Steam relay peer {info.Identity.SteamId.Value} connected; bridging to lobby port {_hostPort}.");
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
            Owner?._diagnostics.Log("Steam", $"Steam relay peer {info.Identity.SteamId.Value} connecting.");
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
            Tunnel?.SteamConnected.TrySetException(new IOException($"Steam relay refused: {info.EndReason}"));
            Tunnel?.Close($"Steam host left ({info.EndReason})", closeSteam: false);
            base.OnDisconnected(info);
        }

        public override void OnMessage(IntPtr data, int size, long messageNum, long recvTime, int channel) =>
            Tunnel?.DeliverFromSteam(data, size);
    }

    /// <summary>One Steam connection bridged to one loopback TCP connection.</summary>
    private sealed class RelayTunnel
    {
        private static long _nextId;
        private readonly SteamRelay _owner;
        private readonly Connection _connection;
        private readonly string _label;
        private readonly Channel<byte[]> _toTcp = Channel.CreateUnbounded<byte[]>(
            new UnboundedChannelOptions { SingleReader = true, SingleWriter = true });
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
            if (Volatile.Read(ref _isClosed) != 0) { tcp.Dispose(); return; }
            tcp.NoDelay = true;
            _tcp = tcp;
            var stream = tcp.GetStream();
            _ = PumpTcpToSteamAsync(stream);
            _ = PumpSteamToTcpAsync(stream);
        }

        // Pump thread: copy out of Steam's buffer before the callback returns.
        internal void DeliverFromSteam(IntPtr data, int size)
        {
            if (size <= 0 || Volatile.Read(ref _isClosed) != 0) return;
            var bytes = new byte[size];
            Marshal.Copy(data, bytes, 0, size);
            _toTcp.Writer.TryWrite(bytes);
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
                await foreach (var bytes in _toTcp.Reader.ReadAllAsync(_closed.Token).ConfigureAwait(false))
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
            _toTcp.Writer.TryComplete();
            _closed.Cancel();
            try { _tcp?.Dispose(); } catch (ObjectDisposedException) { }
            SteamConnected.TrySetException(new IOException(reason));
            _ = SteamConnected.Task.Exception; // observed
            if (closeSteam) _owner._work.Add(() => CloseSteamOnPump(reason));
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
