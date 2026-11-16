using System.Reflection;
using Steamworks;
using Steamworks.Data;
using ConnectionInfo = Steamworks.Data.ConnectionInfo;

namespace AIMP.Launcher;

/// <summary>
/// Reads the remote peer and close reason from a Steam connection.
///
/// Facepunch.Steamworks 2.3.3 has a bug: <c>ConnectionInfo.Identity</c> is
/// implemented as <c>=&gt; address</c>, so it returns the remote IP address
/// (as an IP identity) instead of the remote identity. On a Steam relay
/// connection that address is empty, so <c>Identity.SteamId</c> is always 0.
/// Friends-only checks then rejected every Steam friend. The real fields
/// (<c>identity</c>, <c>endDebug</c>) are internal, so they are read here.
/// </summary>
internal static class SteamConnectionInfo
{
    private const BindingFlags InstanceFields = BindingFlags.Instance | BindingFlags.NonPublic | BindingFlags.Public;
    private static readonly FieldInfo? IdentityField = typeof(ConnectionInfo).GetField("identity", InstanceFields);
    private static readonly FieldInfo? EndDebugField = typeof(ConnectionInfo).GetField("endDebug", InstanceFields);

    /// <summary>True when the Facepunch build has the fields this reader needs.</summary>
    internal static bool IsSupported => IdentityField?.FieldType == typeof(NetIdentity);

    /// <summary>The remote SteamID, or 0 when Steam does not report a SteamID identity.</summary>
    internal static SteamId RemoteSteamId(ConnectionInfo info) =>
        IdentityField?.GetValue(info) is NetIdentity identity && identity.IsSteamId ? identity.SteamId : default;

    /// <summary>The closing side's reason text (sent by the peer that closed), or "".</summary>
    internal static string EndDebug(ConnectionInfo info) =>
        (EndDebugField?.GetValue(info) as string)?.Trim() ?? string.Empty;
}
