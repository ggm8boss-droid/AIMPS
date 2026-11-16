using System.Diagnostics;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using AIMP.Launcher.Contracts;
using AIMP.Protocol;

namespace AIMP.Launcher;

// CathodeLib serializers and cache publication must not race debug compilation.
internal static class GamePreparationSynchronization
{
    internal static readonly object Gate = new();
}

public sealed class GamePreparationService
{
    private const int RecipeVersion = 17; // Never import E_RIPLEY into FRONTEND; roll back the build 487 import (game exited at boot).
    private readonly object _sync = new();
    private readonly LauncherSettingsStore _settings;
    private readonly LauncherDiagnostics _diagnostics;
    private readonly CancellationToken _shutdown;
    private Task _job = Task.CompletedTask;
    private CancellationTokenSource? _jobCancel;
    private bool _skipped;
    private int _configurationEpoch;
    private PreparationReceipt? _receipt;
    private GamePreparationDto _status = EmptyStatus(string.Empty);

    internal GamePreparationService(LauncherSettingsStore settings, LauncherDiagnostics diagnostics,
        IHostApplicationLifetime lifetime)
    {
        _settings = settings;
        _diagnostics = diagnostics;
        _shutdown = lifetime.ApplicationStopping;
    }

    internal GamePreparationDto GetStatus()
    {
        var directory = _settings.Load().GameDirectory;
        lock (_sync)
            return SameDirectory(directory, _status.GameDirectory) ? _status : EmptyStatus(directory);
    }

    internal GamePreparationDto Start(bool revalidate = false)
    {
        lock (_sync)
        {
            var settings = _settings.Load();
            var game = GameInstallation.Resolve(settings.GameDirectory);
            if (!_job.IsCompleted)
            {
                if (!SameDirectory(game.GameDirectory, _status.GameDirectory))
                    throw new InvalidOperationException("Another installation is being prepared. Wait for it to finish.");
                if (_skipped)
                    throw new InvalidOperationException("Game preparation is stopping. Retry in a few seconds.");
                return _status;
            }
            _skipped = false;
            if (!revalidate && SameDirectory(game.GameDirectory, _status.GameDirectory) && _status.State == "Ready")
                return _status;
            _receipt = null;
            _status = new(Guid.NewGuid().ToString("D"), game.GameDirectory, "Running", "Checking",
                0, 0, 0, string.Empty, "Checking your installation", string.Empty, DateTimeOffset.UtcNow, null);
            var id = _status.JobId;
            _jobCancel?.Dispose();
            _jobCancel = CancellationTokenSource.CreateLinkedTokenSource(_shutdown);
            var token = _jobCancel.Token;
            var includeCampaign = settings.DeveloperMode;
            _job = Task.Run(() => Run(game.GameDirectory, includeCampaign, id, token), CancellationToken.None);
            return _status;
        }
    }

    // The player may skip preparation and use the launcher immediately. A running
    // job stops at its next level boundary; game assets are replaced atomically.
    internal GamePreparationDto Skip()
    {
        lock (_sync)
        {
            _skipped = true;
            if (!_job.IsCompleted)
            {
                _jobCancel?.Cancel();
                _status = _status with { Message = "Stopping game preparation" };
            }
            _diagnostics.Log("Preparation", "Game preparation skipped by the player.");
            return _status;
        }
    }

    // Launch revalidates a prepared installation, but never forces a skipped one.
    // Multiplayer lobbies still require a prepared manifest (PrepareLevelAsync).
    internal async Task PrepareForLaunchAsync(CancellationToken cancellationToken)
    {
        Task job;
        lock (_sync) job = _job;
        await job.WaitAsync(cancellationToken).ConfigureAwait(false);
        lock (_sync)
            if (_skipped) return;
        await EnsureReadyAsync(cancellationToken).ConfigureAwait(false);
    }

    internal T WithConfigurationLock<T>(Func<T> operation)
    {
        var epoch = Volatile.Read(ref _configurationEpoch);
        lock (_sync)
        {
            if (epoch != _configurationEpoch)
                throw new InvalidOperationException("Launcher settings were reset. Restart setup before saving.");
            return operation();
        }
    }

    internal int Reset(Func<int> deleteData)
    {
        lock (_sync)
            if (!_job.IsCompleted) throw new InvalidOperationException("Wait for game preparation to finish before resetting the launcher.");
        lock (GamePreparationSynchronization.Gate)
        lock (_sync)
        {
            if (!_job.IsCompleted) throw new InvalidOperationException("Wait for game preparation to finish before resetting the launcher.");
            AssertGameStopped();
            Interlocked.Increment(ref _configurationEpoch);
            try
            {
                var deleted = deleteData();
                _receipt = null;
                _status = EmptyStatus(string.Empty);
                return deleted;
            }
            catch
            {
                _receipt = null;
                _status = _status with { State = "Failed", Error = "Launcher reset did not finish. Retry the reset before playing." };
                throw;
            }
        }
    }

    internal async Task EnsureReadyAsync(CancellationToken cancellationToken)
    {
        Start(revalidate: true);
        Task job;
        lock (_sync) job = _job;
        await job.WaitAsync(cancellationToken).ConfigureAwait(false);
        var state = GetStatus();
        if (state.State != "Ready")
            throw new InvalidOperationException(string.IsNullOrEmpty(state.Error) ? "Game preparation is not ready." : state.Error);
    }

    // PvP requires the certified retail behavior. Keep ordinary Coop's existing
    // preparation unchanged; switching modes never rewrites a live behavior tree.
    internal Task PrepareAlienVsSurvivorsAsync(bool enabled, CancellationToken cancellationToken) =>
        Task.Run(() =>
        {
            cancellationToken.ThrowIfCancellationRequested();
            if (!enabled) return;
            var game = _settings.Load().GameDirectory;
            lock (_sync)
            {
                if (_status.State != "Ready" || !SameDirectory(game, _status.GameDirectory) || _receipt is null)
                    throw new InvalidOperationException("Prepare the game installation before selecting Alien vs Survivors.");
            }
            // Both modes use the same retail BML. This preview's possession,
            // locomotion and camera do not depend on donor behavior branches.
            // Read-only verification cannot prove what an already running game
            // loaded: never alter its BML before switching modes in the lobby.
            AlienPvPBehaviorInstaller.VerifyNativeRetail(game);
            _diagnostics.Log("AlienPvP",
                "Retail BML verified. Alien possession, camera, movement, attack and vent adapters await native readiness.");
        }, cancellationToken);

    // This path is intentionally read-only. No imports or cold compilation after launch.
    internal Task<PreparedWorldManifest> PrepareLevelAsync(string level, CancellationToken cancellationToken, bool allowCampaign = false) =>
        Task.Run(() =>
        {
            cancellationToken.ThrowIfCancellationRequested();
            PreparationReceipt receipt;
            var settings = _settings.Load();
            var game = settings.GameDirectory;
            lock (_sync)
            {
                if (_status.State != "Ready" || !SameDirectory(game, _status.GameDirectory) || _receipt == null)
                    throw new InvalidOperationException("Prepare this installation in the Launcher before starting a multiplayer game.");
                receipt = _receipt;
            }
            // Campaign levels are prepared only in Developer mode (see Run).
            if (allowCampaign && !settings.DeveloperMode)
                throw new InvalidOperationException(MultiplayerSessionCoordinator.CampaignDisabledMessage);
            var source = WorldManifestAutoResolver.ResolveLevelSource(game, level, settings.DeveloperMode);
            var entry = receipt.Manifests.SingleOrDefault(m => SameDirectory(source, m.LevelDirectory));
            if (entry == null && settings.DeveloperMode)
                return LoadDeveloperManifest(source, cancellationToken);
            if (entry == null)
                throw new InvalidOperationException("This level has no prepared world manifest. Close the game and prepare this map individually in Debug > WorldManifest, then relaunch.");
            var world = ResolveWorldDirectory(source);
            if (HashIfPresent(Path.Combine(world, "COMMANDS.PAK")) != entry.CommandsPakSha256
                || HashIfPresent(Path.Combine(world, "COMMANDS.BIN")) != entry.CommandsBinSha256)
                throw new InvalidOperationException("Level files changed since preparation. Close the game and prepare the installation again.");
            cancellationToken.ThrowIfCancellationRequested();
            var prepared = WorldManifestSessionLoader.Load(entry.ManifestPath);
            if (!string.Equals(prepared.BinarySha256, entry.BinarySha256, StringComparison.OrdinalIgnoreCase))
                throw new InvalidOperationException("The prepared world manifest changed. Close the game and prepare the installation again.");
            return prepared;
        }, cancellationToken);

    // Older Debug compilers produced the content-addressed cache without a preparation receipt.
    // Resolve it from the current COMMANDS files; never compile or modify game assets here.
    private static PreparedWorldManifest LoadDeveloperManifest(string level, CancellationToken cancellationToken)
    {
        var world = ResolveWorldDirectory(level);
        var pak = Path.Combine(world, "COMMANDS.PAK");
        var bin = Path.Combine(world, "COMMANDS.BIN");
        var pakHash = HashIfPresent(pak);
        var binHash = HashIfPresent(bin);
        var mapId = WorldManifestCompiler.ComputeStableMapContentId(world, File.Exists(pak), File.Exists(bin));
        var path = WorldManifestCompiler.GetDefaultCachePath(mapId);
        if (!File.Exists(path))
            throw new InvalidOperationException($"No current manifest exists for this map at '{path}' (level='{level}', map={mapId}). Prepare it in Debug > WorldManifest before creating the lobby.");
        cancellationToken.ThrowIfCancellationRequested();
        var prepared = WorldManifestSessionLoader.Load(path);
        var manifest = prepared.Manifest;
        if (manifest.MapContentId != mapId || manifest.CompilerVersion != WorldManifestConstants.CompilerVersion
            || manifest.Source.CommandsPakPresent != File.Exists(pak)
            || manifest.Source.CommandsBinPresent != File.Exists(bin)
            || !string.Equals(manifest.Source.CommandsPakSha256, pakHash, StringComparison.OrdinalIgnoreCase)
            || !string.Equals(manifest.Source.CommandsBinSha256, binHash, StringComparison.OrdinalIgnoreCase))
            throw new InvalidOperationException("The cached manifest is outdated for this map. Regenerate it in Debug > WorldManifest.");
        cancellationToken.ThrowIfCancellationRequested();
        return prepared;
    }

    // Survival levels (DLC) are always prepared. Campaign levels are prepared only
    // in Developer mode, the only mode that can host a campaign lobby.
    private void Run(string game, bool includeCampaign, string jobId, CancellationToken token)
    {
        try
        {
            lock (GamePreparationSynchronization.Gate)
            {
                token.ThrowIfCancellationRequested();
                AssertGameStopped();
                var levels = WorldManifestLevelDiscovery.Discover(game, includeCampaign).Where(l => l.CanCompile).ToArray();
                if (levels.Length == 0) throw new InvalidDataException("No playable levels were found.");
                // Global, not level-bound: verified (and re-applied after a game
                // file repair) on every preparation, even with a valid receipt.
                Update("Checking", 1, 0, levels.Length, string.Empty, "Checking multiplayer assets");
                var animationTimer = Stopwatch.StartNew();
                RemoteCrouchAnimationInstaller.Install(game, token);
                _diagnostics.Log("Preparation", $"Remote crouch animations ready in {animationTimer.Elapsed.TotalSeconds:F0}s.");
                token.ThrowIfCancellationRequested();
                var receiptPath = ReceiptPath(game);
                var existing = TryReadReceipt(receiptPath);
                if (existing != null && ReceiptCovers(game, existing, levels))
                {
                    Complete(existing);
                    return;
                }
                var legacyManifests = WorldManifestCompiler.DeleteLegacyCache();
                _diagnostics.Log("Preparation", $"World manifest cache: {WorldManifestCompiler.CacheDirectory.Value}" +
                    (legacyManifests > 0 ? $" ({legacyManifests} old manifests removed from AppData)." : "."));
                Update("Character", 2, 0, levels.Length, string.Empty, "Preparing multiplayer characters");
                var timer = Stopwatch.StartNew();
                // Characters and manifests cover the same levels: a level with a
                // manifest but without E_RIPLEY and its DisplayModel dependencies
                // acknowledges Spawn130 but never materializes a remote Character.
                RipleyCharacterInstaller.Install(game, new InlineProgress<RipleyInstallProgress>(p =>
                    Update("Character", 2 + (int)(38L * p.CompletedLevels / Math.Max(1, p.TotalLevels)),
                        p.CompletedLevels, p.TotalLevels, p.Level, p.Message)), token, includeCampaign);
                // The character step held every level's assets; return that memory
                // before sizing the manifest workers on the free physical memory.
                GC.Collect(GC.MaxGeneration, GCCollectionMode.Aggressive, blocking: true, compacting: true);
                _diagnostics.Log("Preparation", $"Character assets ready for {levels.Length} levels in {timer.Elapsed.TotalSeconds:F0}s.");
                token.ThrowIfCancellationRequested();
                AssertGameStopped();
                timer.Restart();
                _diagnostics.Log("Preparation", $"Compiling {levels.Length} level manifests with up to {WorldManifestWorkerPool.WorkerCount(levels.Length)} workers.");
                var levelProgress = new int[levels.Length];
                var binaries = WorldManifestWorkerPool.CompileAll(levels.Select(l => l.LevelDirectory).ToArray(),
                    (index, percentage) =>
                    {
                        lock (levelProgress)
                        {
                            levelProgress[index] = Math.Max(levelProgress[index], percentage);
                            var total = levelProgress.Sum(value => (long)value);
                            Update("Manifests", 40 + (int)(60L * total / (levels.Length * 100L)),
                                levelProgress.Count(value => value >= 100), levels.Length,
                                levels[index].DisplayName, "Preparing level data");
                        }
                    }, token);
                _diagnostics.Log("Preparation", $"Level manifests compiled in {timer.Elapsed.TotalSeconds:F0}s.");
                var manifests = new List<PreparedManifestReceipt>();
                for (var i = 0; i < levels.Length; i++)
                {
                    token.ThrowIfCancellationRequested();
                    var level = levels[i];
                    // Validate the binary even for non-playable/cinematic levels with no World probe.
                    var manifest = WorldManifestBinaryCodec.Read(binaries[i]);
                    if (manifest.CompilerVersion != WorldManifestConstants.CompilerVersion)
                        throw new InvalidDataException("A world manifest was produced by an incompatible compiler.");
                    var world = ResolveWorldDirectory(level.LevelDirectory);
                    var pakHash = HashIfPresent(Path.Combine(world, "COMMANDS.PAK"));
                    var binHash = HashIfPresent(Path.Combine(world, "COMMANDS.BIN"));
                    if (!string.Equals(manifest.Source.CommandsPakSha256, pakHash, StringComparison.OrdinalIgnoreCase)
                        || !string.Equals(manifest.Source.CommandsBinSha256, binHash, StringComparison.OrdinalIgnoreCase))
                        throw new IOException("Level files changed while preparing a manifest. Retry preparation.");
                    manifests.Add(new(level.LevelDirectory, binaries[i],
                        WorldReplicationCodec.ComputeManifestSha256(binaries[i]),
                        pakHash, binHash));
                }
                var array = manifests.ToArray();
                var receipt = new PreparationReceipt(RecipeVersion, WorldManifestConstants.CompilerVersion,
                    game, CaptureStamps(game, levels, array), array);
                Directory.CreateDirectory(Path.GetDirectoryName(receiptPath)!);
                var temporary = receiptPath + ".tmp";
                File.WriteAllText(temporary, JsonSerializer.Serialize(receipt));
                File.Move(temporary, receiptPath, true);
                Complete(receipt);
            }
        }
        catch (Exception error)
        {
            _diagnostics.Log("Preparation", $"Preparation {jobId} failed: {error}");
            lock (_sync) _status = _status with
            {
                State = error is OperationCanceledException ? "Canceled" : "Failed",
                Message = "Game preparation could not finish", Error = error.Message,
                CompletedAtUtc = DateTimeOffset.UtcNow
            };
        }
    }

    // A receipt may cover more levels than requested: one prepared in Developer
    // mode (with the campaign) stays valid for Survival only. Its stamps are
    // checked over its own levels.
    private static bool ReceiptCovers(string game, PreparationReceipt receipt,
        IReadOnlyList<WorldManifestLevelSource> levels)
    {
        if (receipt.RecipeVersion != RecipeVersion || receipt.CompilerVersion != WorldManifestConstants.CompilerVersion
            || !SameDirectory(receipt.GameDirectory, game)
            || !levels.All(l => receipt.Manifests.Any(m => SameDirectory(l.LevelDirectory, m.LevelDirectory))))
            return false;
        var prepared = levels.Count == receipt.Manifests.Length ? levels
            : WorldManifestLevelDiscovery.Discover(game, includeCampaign: true).Where(l => l.CanCompile)
                .Where(l => receipt.Manifests.Any(m => SameDirectory(l.LevelDirectory, m.LevelDirectory))).ToArray();
        return prepared.Count == receipt.Manifests.Length
            && StampsEqual(receipt.Files, CaptureStamps(game, prepared, receipt.Manifests),
                Path.Combine(game, "DATA", "GLOBAL", "ANIMATION.PAK"));
    }

    private void Complete(PreparationReceipt receipt)
    {
        lock (_sync)
        {
            _receipt = receipt;
            _status = _status with { State = "Ready", Stage = "Ready", Percentage = 100,
                CompletedLevels = receipt.Manifests.Length, TotalLevels = receipt.Manifests.Length,
                CurrentLevel = string.Empty, Message = "Ready to play", Error = string.Empty,
                CompletedAtUtc = DateTimeOffset.UtcNow };
        }
        _diagnostics.Log("Preparation", $"Installation ready: {receipt.Manifests.Length} level manifests verified.");
    }

    private void Update(string stage, int percentage, int completed, int total, string level, string message)
    {
        lock (_sync) _status = _status with { Stage = stage, Percentage = percentage,
            CompletedLevels = completed, TotalLevels = total, CurrentLevel = level, Message = message };
    }

    private static Dictionary<string, FileStamp> CaptureStamps(string game,
        IReadOnlyList<WorldManifestLevelSource> levels, IReadOnlyList<PreparedManifestReceipt> manifests)
    {
        var paths = levels.SelectMany(l => new[] { l.LevelDirectory, l.LevelDirectory + "_PATCH" })
            .Append(Path.Combine(game, "DATA", "ENV", "GLOBAL"))
            .Where(Directory.Exists)
            .SelectMany(root => Directory.EnumerateFiles(root, "*", SearchOption.AllDirectories)
                .Where(p => p.EndsWith(".PAK", StringComparison.OrdinalIgnoreCase)
                    || p.EndsWith(".BIN", StringComparison.OrdinalIgnoreCase)
                    || p.EndsWith(".MTL", StringComparison.OrdinalIgnoreCase)
                    || p.EndsWith(".MAP", StringComparison.OrdinalIgnoreCase)
                    || p.EndsWith(".DAT", StringComparison.OrdinalIgnoreCase)))
            .Concat(manifests.Select(m => m.ManifestPath))
            .Append(Path.Combine(game, "DATA", "GLOBAL", "ANIMATION.PAK"));
        return paths.Distinct(StringComparer.OrdinalIgnoreCase).ToDictionary(p => Path.GetFullPath(p), p =>
        {
            var info = new FileInfo(p);
            if (!info.Exists) throw new FileNotFoundException("A prepared asset is missing.", p);
            return new FileStamp(info.Length, info.LastWriteTimeUtc.Ticks);
        }, StringComparer.OrdinalIgnoreCase);
    }

    // The animation installer above verifies its original/patched SHA-256 on
    // every run. Updating only that independent global archive must not force
    // a rebuild of all prepared level characters/manifests.
    internal static bool StampsEqual(Dictionary<string, FileStamp> a, Dictionary<string, FileStamp> b,
        string verifiedAnimation) => a.Count == b.Count && b.All(pair =>
            a.TryGetValue(pair.Key, out var value) &&
            (pair.Key.Equals(verifiedAnimation, StringComparison.OrdinalIgnoreCase) || value == pair.Value));
    private static PreparationReceipt? TryReadReceipt(string path)
    {
        try
        {
            var receipt = File.Exists(path) ? JsonSerializer.Deserialize<PreparationReceipt>(File.ReadAllText(path)) : null;
            return receipt is { Files: not null, Manifests: not null, GameDirectory: not null }
                && receipt.Manifests.All(m => m is not null && !string.IsNullOrWhiteSpace(m.ManifestPath)
                    && !string.IsNullOrWhiteSpace(m.LevelDirectory)) ? receipt : null;
        }
        catch (Exception e) when (e is IOException or JsonException or UnauthorizedAccessException) { return null; }
    }
    private static string ReceiptPath(string game) => Path.Combine(
        Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "AIMP", "Preparation",
        Convert.ToHexString(SHA256.HashData(Encoding.UTF8.GetBytes(Path.GetFullPath(game).ToUpperInvariant()))) + ".json");
    private static string ResolveWorldDirectory(string level) => WorldManifestCompiler.ResolveWorldDirectory(level);
    private static string HashIfPresent(string path)
    {
        if (!File.Exists(path)) return string.Empty;
        using var stream = File.OpenRead(path);
        return Convert.ToHexString(SHA256.HashData(stream));
    }
    private static bool SameDirectory(string a, string b) => string.Equals(
        a.TrimEnd('\\', '/'), b.TrimEnd('\\', '/'), StringComparison.OrdinalIgnoreCase);
    private static void AssertGameStopped()
    {
        var processes = Process.GetProcessesByName("AI");
        try
        {
            if (processes.Any(p => !p.HasExited))
                throw new InvalidOperationException("Close Alien: Isolation before preparing its files.");
        }
        finally { foreach (var process in processes) process.Dispose(); }
    }
    private static GamePreparationDto EmptyStatus(string directory) => new(string.Empty, directory,
        string.IsNullOrWhiteSpace(directory) ? "NotConfigured" : "Idle", string.Empty, 0, 0, 0,
        string.Empty, "Select your game installation", string.Empty, null, null);
    internal sealed record FileStamp(long Length, long LastWriteTicks);
    private sealed record PreparedManifestReceipt(string LevelDirectory, string ManifestPath,
        string BinarySha256, string CommandsPakSha256, string CommandsBinSha256);
    private sealed record PreparationReceipt(int RecipeVersion, uint CompilerVersion, string GameDirectory,
        Dictionary<string, FileStamp> Files, PreparedManifestReceipt[] Manifests);
    private sealed class InlineProgress<T>(Action<T> report) : IProgress<T>
    {
        public void Report(T value) => report(value);
    }
}
