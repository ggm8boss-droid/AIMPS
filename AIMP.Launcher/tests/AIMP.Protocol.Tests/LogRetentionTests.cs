using System.IO.Compression;
using System.Text;
using AIMP.Launcher;

namespace AIMP.Protocol.Tests;

internal static class LogRetentionTests
{
    internal static Task CompressesAndKeepsNewestAsync()
    {
        using var temporary = new TemporaryDirectory();
        var root = temporary.Combine("logs");
        Directory.CreateDirectory(root);
        var start = new DateTime(2026, 11, 1, 0, 0, 0, DateTimeKind.Utc);
        var originals = new Dictionary<string, byte[]>();
        for (var index = 0; index < 13; index++)
        {
            var path = Path.Combine(root, $"AIMP.NetZ-20261101T0000{index:D2}-000-{1000 + index}.jsonl");
            originals[path] = WriteLog(path, index, start.AddMinutes(index));
        }
        var crash = Path.Combine(root, "AIMP.Crash-77.txt");
        var crashBytes = WriteLog(crash, 99, start);
        var unrelated = Path.Combine(root, "notes.txt");
        File.WriteAllText(unrelated, "keep me");

        var result = LogRetention.Apply(root);

        AssertEx.Equal(14, result.Compressed);
        AssertEx.Equal(3, result.Deleted);
        AssertEx.Equal(0, result.Skipped);
        AssertEx.True(File.Exists(unrelated), "unrelated files are untouched");
        AssertEx.SequenceEqual(crashBytes, Decompress(crash + ".gz"));
        foreach (var (path, bytes) in originals)
        {
            AssertEx.False(File.Exists(path), "original removed after compression");
            var index = int.Parse(Path.GetFileNameWithoutExtension(path).Split('-')[^1]) - 1000;
            AssertEx.Equal(index >= 3, File.Exists(path + ".gz"), $"newest 10 kept ({index})");
            if (index < 3) continue;
            AssertEx.SequenceEqual(bytes, Decompress(path + ".gz"));
            AssertEx.Equal(start.AddMinutes(index), File.GetLastWriteTimeUtc(path + ".gz"));
        }

        return Task.CompletedTask;
    }

    internal static Task LeavesOpenLogsAloneAsync()
    {
        using var temporary = new TemporaryDirectory();
        var root = temporary.Combine("logs");
        Directory.CreateDirectory(root);
        var start = new DateTime(2026, 11, 1, 0, 0, 0, DateTimeKind.Utc);
        for (var index = 0; index < 3; index++)
            WriteLog(Path.Combine(root, $"AIMP.NetZ-old-{index}.jsonl"), index, start.AddMinutes(index));
        var open = Path.Combine(root, "AIMP.NetZ-open-1.jsonl");
        WriteLog(open, 10, start);
        var active = Path.Combine(root, "AIMP.Launcher-active-1.log");
        WriteLog(active, 11, start);

        LogRetentionResult result;
        using (new FileStream(open, FileMode.Open, FileAccess.Write, FileShare.ReadWrite))
            result = LogRetention.Apply(root, active, maximumFilesPerKind: 1);

        AssertEx.True(File.Exists(open), "a log still being written is neither compressed nor deleted");
        AssertEx.False(File.Exists(open + ".gz"));
        AssertEx.True(File.Exists(active), "the launcher's own log is untouched");
        AssertEx.Equal(1, Directory.GetFiles(root, "AIMP.NetZ-old-*.gz").Length);
        AssertEx.True(File.Exists(Path.Combine(root, "AIMP.NetZ-old-2.jsonl.gz")), "newest archive kept");
        AssertEx.True(result.Skipped >= 1);
        return Task.CompletedTask;
    }

    internal static async Task DiagnosticsRunsRetentionAsync()
    {
        using var temporary = new TemporaryDirectory();
        var root = temporary.Combine("logs");
        Directory.CreateDirectory(root);
        var old = Path.Combine(root, "AIMP.Launcher-20261101T000000Z-1.log");
        var bytes = WriteLog(old, 1, DateTime.UtcNow.AddDays(-1));
        using var diagnostics = new LauncherDiagnostics(root);
        await diagnostics.RetentionTask.WaitAsync(TimeSpan.FromSeconds(10));
        AssertEx.SequenceEqual(bytes, Decompress(old + ".gz"));
        AssertEx.True(File.Exists(diagnostics.Path), "current launcher log stays uncompressed");
    }

    private static byte[] WriteLog(string path, int seed, DateTime lastWriteUtc)
    {
        var builder = new StringBuilder();
        for (var line = 0; line < 500; line++)
            builder.Append($"{{\"sequence\":{line},\"seed\":{seed},\"text\":\"ünïcødé {Guid.NewGuid()}\"}}\n");
        var bytes = Encoding.UTF8.GetBytes(builder.ToString());
        File.WriteAllBytes(path, bytes);
        File.SetLastWriteTimeUtc(path, lastWriteUtc);
        return bytes;
    }

    private static byte[] Decompress(string path)
    {
        using var input = File.OpenRead(path);
        using var gzip = new GZipStream(input, CompressionMode.Decompress);
        using var output = new MemoryStream();
        gzip.CopyTo(output);
        return output.ToArray();
    }
}
