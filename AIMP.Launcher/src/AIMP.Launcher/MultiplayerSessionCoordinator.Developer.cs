namespace AIMP.Launcher;

internal sealed partial class MultiplayerSessionCoordinator
{
    private async Task SetDeveloperModeAsync(ScaleformBridgeMessage message,
        Func<string, IReadOnlyDictionary<string, string>?, Task<bool>>? reply = null)
    {
        var value = message.Get("enabled");
        if (value is not ("0" or "1")) throw new InvalidOperationException("Invalid developer mode.");
        _settingsStore.SetDeveloperMode(value == "1");
        await SendVoiceSettingsAsync(reply).ConfigureAwait(false);
        await SendConfigAsync().ConfigureAwait(false);
    }

    private static void ValidateDeveloperLobby(LauncherSettings settings, ScaleformBridgeMessage message)
    {
        var level = ResolveLevel(message);
        var mode = ReadLobbyMode(message);
        // Campaign is a Developer mode game mode (its levels are only prepared there).
        if (mode.Equals("campaign", StringComparison.OrdinalIgnoreCase))
        {
            if (!settings.DeveloperMode)
                throw new InvalidOperationException(CampaignDisabledMessage);
            return;
        }
        if (!mode.Equals("survival", StringComparison.OrdinalIgnoreCase))
            throw new InvalidOperationException("Invalid game mode.");
        if (settings.DeveloperMode)
            WorldManifestAutoResolver.ResolveLevelSource(settings.GameDirectory, level, includeCampaign: true);
    }

    // The menu's Game Mode stepper has no initial selection, so until the player
    // steps it the GFX sends String(undefined) = "undefined". Unset values mean the
    // stepper's first (displayed) entry, Survival; anything else is still rejected.
    internal static string ReadLobbyMode(ScaleformBridgeMessage message)
    {
        var mode = message.Get("mode").Trim();
        return mode.Length == 0 || mode is "undefined" or "null" ? "survival" : mode;
    }

    internal const string CampaignDisabledMessage =
        "Campaign is only available in Developer mode. Enable it, then restart the game to prepare the campaign levels.";

    private static string LevelGameMode(string level) =>
        level.StartsWith("Challenge", StringComparison.OrdinalIgnoreCase)
        || level.StartsWith("BSPNOSTROMO_", StringComparison.OrdinalIgnoreCase)
        || level.StartsWith("Salvage", StringComparison.OrdinalIgnoreCase) ? "Survival" : "Campaign";

    private Dictionary<string, string> AddDeveloperLevels(
        LauncherSettings settings, Dictionary<string, string> values)
    {
        values["developerModeAvailable"] = _settingsStore.DeveloperModeAvailable ? "1" : "0";
        values["developerMode"] = settings.DeveloperMode ? "1" : "0";
        var levels = settings.DeveloperMode
            ? WorldManifestLevelDiscovery.Discover(settings.GameDirectory, includeCampaign: true)
                .Where(l => l.CanCompile && !l.LevelDirectory.EndsWith("_PATCH", StringComparison.OrdinalIgnoreCase))
                .Select(l => Path.GetFileName(l.LevelDirectory)).Distinct(StringComparer.OrdinalIgnoreCase).ToArray()
            : Array.Empty<string>();
        values["developerLevelCount"] = levels.Length.ToString(System.Globalization.CultureInfo.InvariantCulture);
        for (var i = 0; i < levels.Length; i++) values[$"developerLevel{i}"] = levels[i];
        return values;
    }
}
