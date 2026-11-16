using AIMP.Protocol;
using System.IO.Compression;
using System.Text;
using System.Text.Json;

namespace AIMP.Launcher;

internal sealed class LauncherDiagnostics : IDisposable
{
    private const int MaximumMemoryEntries = 1_000;
    private readonly object _gate = new();
    private readonly Queue<string> _entries = new();
    private readonly StreamWriter _writer;
    private int _disposed;
    private int _enabled = 1;

    internal LauncherDiagnostics(string? directory = null)
    {
        var root = directory ?? DefaultLogDirectory();
        Directory.CreateDirectory(root);
        Path = System.IO.Path.Combine(root,
            $"AIMP.Launcher-{DateTime.UtcNow:yyyyMMddTHHmmssZ}-{Environment.ProcessId}.log");
        var stream = new FileStream(Path, FileMode.Create, FileAccess.Write,
            FileShare.Read | FileShare.Delete);
        _writer = new StreamWriter(stream, new UTF8Encoding(false), 32 * 1024) { AutoFlush = false };
        Log("Launcher", "Launcher diagnostics started.");
        RetentionTask = Task.Run(() => ApplyRetention(root));
    }

    /// <summary>Completes once old logs have been compressed and pruned.</summary>
    internal Task RetentionTask { get; }

    private void ApplyRetention(string root)
    {
        try
        {
            var result = LogRetention.Apply(root, Path);
            if (result.Compressed + result.Deleted + result.Skipped > 0)
                Log("Launcher", $"Log retention: compressed={result.Compressed}, " +
                    $"deleted={result.Deleted}, skipped={result.Skipped}, " +
                    $"keep={LogRetention.MaximumFilesPerKind} per kind.");
        }
        catch (Exception exception)
        {
            try { Log("Launcher", $"Log retention failed: {exception.Message}"); }
            catch (ObjectDisposedException) { }
        }
    }

    internal string Path { get; }

    internal event EventHandler<string>? EntryWritten;

    internal bool Enabled => Volatile.Read(ref _enabled) != 0;

    internal void SetEnabled(bool enabled) =>
        Volatile.Write(ref _enabled, enabled ? 1 : 0);

    internal void Log(string category, string message)
    {
        if (Volatile.Read(ref _disposed) != 0 || Volatile.Read(ref _enabled) == 0) return;
        var entry = $"[{DateTimeOffset.Now:HH:mm:ss}] [{category}] {message}";
        lock (_gate)
        {
            _entries.Enqueue(entry);
            while (_entries.Count > MaximumMemoryEntries) _entries.Dequeue();
            _writer.WriteLine(entry);
        }

        EntryWritten?.Invoke(this, entry);
    }

    internal IReadOnlyList<string> SnapshotEntries()
    {
        lock (_gate) return _entries.ToArray();
    }

    internal void Flush()
    {
        lock (_gate) _writer.Flush();
    }

    public void Dispose()
    {
        if (Interlocked.Exchange(ref _disposed, 1) != 0) return;
        lock (_gate) _writer.Dispose();
    }

    private static string DefaultLogDirectory() => System.IO.Path.Combine(
        Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),
        "AIMP",
        "logs");
}

internal sealed record RuntimeDiagnosticReport(
    string LauncherVersion,
    string NativeDllVersion,
    int AiExePid,
    string AiExeExecutablePath,
    string AiExeModuleBase,
    string DllPath,
    bool DllInjected,
    bool IpcConnected,
    bool EngineHooksInstalled,
    bool MainThreadCharacterProcessingHookInstalled,
    bool CharacterManagerUpdateHookInstalled,
    bool CharacterReadinessProducerHookInstalled,
    bool EngineThreadReady,
    int EngineThreadId,
    string EngineThreadSource,
    DateTimeOffset? FirstEngineCallbackTimestamp,
    DateTimeOffset? LastEngineCallbackTimestamp,
    int PendingCommandCount,
    string LastCommand,
    string LastCommandResult,
    string LastNativeException,
    int CurrentLevelGeneration,
    string NativeLogPath,
    string LauncherLogPath,
    IReadOnlyList<string> LauncherLogs);

internal static class RuntimeDiagnosticsExporter
{
    private static readonly JsonSerializerOptions JsonOptions = new()
    {
        WriteIndented = true
    };

    internal static async Task<string> ExportAsync(
        string archivePath,
        NativeRuntimeSnapshot snapshot,
        LauncherDiagnostics diagnostics,
        DiagnosticSnapshotResultPayload? nativeSnapshot,
        CancellationToken cancellationToken)
    {
        var destination = System.IO.Path.GetFullPath(archivePath);
        Directory.CreateDirectory(System.IO.Path.GetDirectoryName(destination)!);
        var temporary = destination + $".{Guid.NewGuid():N}.tmp";
        try
        {
            await CreateArchiveAsync(temporary, snapshot, diagnostics, nativeSnapshot,
                cancellationToken).ConfigureAwait(false);
            File.Move(temporary, destination, true);
            return destination;
        }
        finally
        {
            if (File.Exists(temporary)) File.Delete(temporary);
        }
    }

    private static async Task CreateArchiveAsync(
        string path,
        NativeRuntimeSnapshot snapshot,
        LauncherDiagnostics diagnostics,
        DiagnosticSnapshotResultPayload? nativeSnapshot,
        CancellationToken token)
    {
        await using var stream = new FileStream(path, FileMode.CreateNew, FileAccess.ReadWrite);
        using var archive = new ZipArchive(stream, ZipArchiveMode.Create, true);
        await AddReportAsync(archive, snapshot, diagnostics, token).ConfigureAwait(false);
        AddFile(archive, diagnostics.Path, "logs/launcher.log");
        AddNativeFiles(archive, snapshot, nativeSnapshot);
    }

    private static async Task AddReportAsync(
        ZipArchive archive,
        NativeRuntimeSnapshot snapshot,
        LauncherDiagnostics diagnostics,
        CancellationToken token)
    {
        var report = CreateReport(snapshot, diagnostics);
        var bytes = JsonSerializer.SerializeToUtf8Bytes(report, JsonOptions);
        var entry = archive.CreateEntry("runtime-diagnostics.json", CompressionLevel.Optimal);
        await using var destination = entry.Open();
        await destination.WriteAsync(bytes, token).ConfigureAwait(false);
    }

    private static RuntimeDiagnosticReport CreateReport(
        NativeRuntimeSnapshot value,
        LauncherDiagnostics diagnostics)
    {
        return new RuntimeDiagnosticReport(ProductInfo.LauncherVersion,
            value.NativeDllVersion, value.ProcessId, value.ExecutablePath,
            $"0x{value.ModuleBase:X}", value.DllPath, value.DllInjected,
            value.IpcConnected, value.EngineHooksInstalled, value.MainHookInstalled,
            value.FallbackHookInstalled, value.ProducerHookInstalled,
            value.EngineThreadReady, value.EngineThreadId,
            value.EngineThreadSource, value.FirstEngineCallbackUtc,
            value.LastEngineCallbackUtc, value.PendingCommandCount, value.LastCommand,
            value.LastCommandResult, value.LastNativeException, value.LevelGeneration,
            value.NativeLogPath, diagnostics.Path, diagnostics.SnapshotEntries());
    }

    private static void AddNativeFiles(
        ZipArchive archive,
        NativeRuntimeSnapshot runtime,
        DiagnosticSnapshotResultPayload? snapshot)
    {
        // A live IPC snapshot is authoritative. If the game has already
        // exited, fall back to the last retained path, then to the newest
        // native log whose filename ends in the last AI.exe PID. This keeps
        // door diagnostics available for the common "test, exit, export" flow.
        var nativeLogPath = snapshot?.NativeLogPath;
        if (string.IsNullOrWhiteSpace(nativeLogPath))
            nativeLogPath = runtime.NativeLogPath;
        if (string.IsNullOrWhiteSpace(nativeLogPath))
            nativeLogPath = FindNativeLogForProcess(runtime.ProcessId);
        AddTrustedFile(archive, nativeLogPath ?? string.Empty, "logs/native.log");
        AddTrustedFile(archive, FindWs3RootTraceForProcess(runtime.ProcessId) ?? string.Empty,
            "native/ws3-root-trace.json");
        AddTrustedFile(archive, FindProcessArtifact(runtime.ProcessId, "AIMP.WS3.RootJournal-", ".jsonl") ?? string.Empty,
            "native/ws3-root-journal.jsonl");
        AddTrustedFile(archive, FindWs3SemanticProbeForProcess(runtime.ProcessId) ?? string.Empty,
            "native/ws3-semantic-probe.json");
        AddTrustedFile(archive, FindProcessArtifact(runtime.ProcessId, "AIMP.RemoteLocomotionFlight-", ".jsonl") ?? string.Empty,
            "native/remote-locomotion-flight.jsonl");
        AddTrustedFile(archive, FindProcessArtifact(runtime.ProcessId, "AIMP.Crash-", ".txt") ?? string.Empty,
            "native/crash.txt");

        if (snapshot is null) return;
        var index = 0;
        foreach (var path in snapshot.ArtifactPaths.Take(32))
            AddTrustedFile(archive, path, $"native/{index++:D2}-{SafeName(path)}");
    }

    private static string? FindProcessArtifact(int processId, string prefix, string suffix)
    {
        if (processId <= 0) return null;
        try
        {
            var root = System.IO.Path.Combine(
                Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),
                "AIMP", "logs");
            var path = System.IO.Path.Combine(root, $"{prefix}{processId}{suffix}");
            return File.Exists(path) ? path : null;
        }
        catch (IOException)
        {
            return null;
        }
        catch (UnauthorizedAccessException)
        {
            return null;
        }
    }

    private static string? FindWs3RootTraceForProcess(int processId)
    {
        if (processId <= 0) return null;
        try
        {
            var root = System.IO.Path.Combine(
                Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),
                "AIMP", "logs");
            var path = System.IO.Path.Combine(root, $"AIMP.WS3.RootTrace-{processId}.json");
            return File.Exists(path) ? path : null;
        }
        catch (IOException)
        {
            return null;
        }
        catch (UnauthorizedAccessException)
        {
            return null;
        }
    }

    private static string? FindWs3SemanticProbeForProcess(int processId)
    {
        if (processId <= 0) return null;
        try
        {
            var root = System.IO.Path.Combine(
                Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),
                "AIMP", "logs");
            var path = System.IO.Path.Combine(root, $"AIMP.WS3.SemanticProbe-{processId}.json");
            return File.Exists(path) ? path : null;
        }
        catch (IOException)
        {
            return null;
        }
        catch (UnauthorizedAccessException)
        {
            return null;
        }
    }

    private static string? FindNativeLogForProcess(int processId)
    {
        if (processId <= 0) return null;
        try
        {
            var root = System.IO.Path.Combine(
                Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),
                "AIMP", "logs");
            if (!Directory.Exists(root)) return null;
            var suffix = $"-{processId}.jsonl";
            return Directory.EnumerateFiles(root, "AIMP.NetZ-*.jsonl", SearchOption.TopDirectoryOnly)
                .Where(path => path.EndsWith(suffix, StringComparison.OrdinalIgnoreCase))
                .OrderByDescending(File.GetLastWriteTimeUtc)
                .FirstOrDefault();
        }
        catch (IOException)
        {
            return null;
        }
        catch (UnauthorizedAccessException)
        {
            return null;
        }
    }

    private static void AddTrustedFile(ZipArchive archive, string path, string entryName)
    {
        if (!IsTrustedNativeFile(path)) return;
        AddFile(archive, path, entryName);
    }

    private static bool IsTrustedNativeFile(string path)
    {
        if (string.IsNullOrWhiteSpace(path) || !File.Exists(path)) return false;
        var root = System.IO.Path.Combine(
            Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "AIMP");
        var relative = System.IO.Path.GetRelativePath(root, System.IO.Path.GetFullPath(path));
        return !System.IO.Path.IsPathRooted(relative)
            && relative != ".."
            && !relative.StartsWith($"..{System.IO.Path.DirectorySeparatorChar}",
                StringComparison.Ordinal);
    }

    private static void AddFile(ZipArchive archive, string source, string entryName)
    {
        if (!File.Exists(source)) return;
        var entry = archive.CreateEntry(entryName, CompressionLevel.Optimal);
        using var input = new FileStream(source, FileMode.Open, FileAccess.Read,
            FileShare.ReadWrite | FileShare.Delete);
        using var output = entry.Open();
        input.CopyTo(output);
    }

    private static string SafeName(string path)
    {
        var invalid = System.IO.Path.GetInvalidFileNameChars().ToHashSet();
        var name = System.IO.Path.GetFileName(path);
        return new string(name.Select(value => invalid.Contains(value) ? '_' : value).ToArray());
    }
}
