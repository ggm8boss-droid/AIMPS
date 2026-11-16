using System.IO.Compression;
using System.Security.Cryptography;

namespace AIMP.Launcher;

internal sealed record LogRetentionResult(int Compressed, int Deleted, int Skipped)
{
    internal static LogRetentionResult Empty { get; } = new(0, 0, 0);

    internal LogRetentionResult Add(LogRetentionResult other) =>
        new(Compressed + other.Compressed, Deleted + other.Deleted, Skipped + other.Skipped);
}

/// <summary>
/// Keeps %LOCALAPPDATA%\AIMP\logs bounded. Every finished log is gzipped
/// (lossless; the archive is decompressed and compared byte-for-byte with the
/// original before the original is removed), and only the newest
/// <see cref="MaximumFilesPerKind"/> files of each log kind are kept.
/// Files still open by a running launcher or game are never touched.
/// </summary>
internal static class LogRetention
{
    internal const int MaximumFilesPerKind = 10;
    private const string CompressedExtension = ".gz";
    private const string TemporaryExtension = ".tmp";
    private const int BufferSize = 81920;

    // One entry per log family written into the AIMP logs directory.
    internal static readonly IReadOnlyList<string> Kinds = new[]
    {
        "AIMP.Launcher-",
        "AIMP.NetZ-",
        "AIMP.Crash-",
        "AIMP.WS3.RootTrace-",
        "AIMP.WS3.RootJournal-",
        "AIMP.WS3.SemanticProbe-",
        "AIMP.RemoteLocomotionFlight-",
    };

    internal static LogRetentionResult Apply(
        string directory,
        string? activePath = null,
        int maximumFilesPerKind = MaximumFilesPerKind)
    {
        if (maximumFilesPerKind < 1) throw new ArgumentOutOfRangeException(nameof(maximumFilesPerKind));
        if (!Directory.Exists(directory)) return LogRetentionResult.Empty;
        var active = activePath is null ? null : Path.GetFullPath(activePath);
        RemoveAbandonedTemporaryFiles(directory);
        return Kinds.Aggregate(LogRetentionResult.Empty, (total, kind) => total
            .Add(CompressKind(directory, kind, active))
            .Add(PruneKind(directory, kind, active, maximumFilesPerKind)));
    }

    private static LogRetentionResult CompressKind(string directory, string kind, string? active)
    {
        int compressed = 0, skipped = 0;
        foreach (var path in Enumerate(directory, kind))
        {
            if (IsCompressed(path) || IsActive(path, active)) continue;
            if (TryCompress(path)) compressed++;
            else skipped++;
        }
        return new(compressed, 0, skipped);
    }

    private static LogRetentionResult PruneKind(
        string directory, string kind, string? active, int maximumFilesPerKind)
    {
        int deleted = 0, skipped = 0;
        var expired = Enumerate(directory, kind)
            .Select(path => new FileInfo(path))
            .OrderByDescending(file => file.LastWriteTimeUtc)
            .ThenByDescending(file => file.Name, StringComparer.OrdinalIgnoreCase)
            .Skip(maximumFilesPerKind);
        foreach (var file in expired)
        {
            if (IsActive(file.FullName, active)) continue;
            if (TryDelete(file.FullName)) deleted++;
            else skipped++;
        }
        return new(0, deleted, skipped);
    }

    private static IReadOnlyList<string> Enumerate(string directory, string kind)
    {
        try
        {
            return Directory.EnumerateFiles(directory, kind + "*", SearchOption.TopDirectoryOnly)
                .Where(path => !path.EndsWith(TemporaryExtension, StringComparison.OrdinalIgnoreCase))
                .ToList();
        }
        catch (Exception exception) when (IsFileSystemFailure(exception))
        {
            return Array.Empty<string>();
        }
    }

    private static bool IsFileSystemFailure(Exception exception) =>
        exception is IOException or UnauthorizedAccessException;

    private static bool IsCompressed(string path) =>
        path.EndsWith(CompressedExtension, StringComparison.OrdinalIgnoreCase);

    private static bool IsActive(string path, string? active) =>
        active is not null && string.Equals(Path.GetFullPath(path), active, StringComparison.OrdinalIgnoreCase);

    /// <summary>
    /// Compresses <paramref name="path"/> to <c>path.gz</c> and removes the
    /// original only after the archive decompresses to identical bytes.
    /// Returns false and leaves the original untouched if the file is still
    /// being written or anything fails.
    /// </summary>
    internal static bool TryCompress(string path)
    {
        var destination = path + CompressedExtension;
        var temporary = $"{destination}.{Guid.NewGuid():N}{TemporaryExtension}";
        try
        {
            return CompressVerified(path, temporary, destination);
        }
        catch (Exception exception) when (IsFileSystemFailure(exception) || exception is InvalidDataException)
        {
            return false;
        }
        finally
        {
            TryDeleteQuietly(temporary);
        }
    }

    private static bool CompressVerified(string path, string temporary, string destination)
    {
        var lastWrite = File.GetLastWriteTimeUtc(path);
        var (hash, length) = WriteArchive(path, temporary);
        if (!ArchiveMatches(temporary, hash, length)) return false;
        File.SetLastWriteTimeUtc(temporary, lastWrite);
        File.Move(temporary, destination, overwrite: true);
        File.Delete(path);
        return true;
    }

    private static (byte[] Hash, long Length) WriteArchive(string path, string archive)
    {
        // FileShare.Read denies writers: if the game or another launcher still
        // holds the log open for writing this throws and the file is skipped.
        using var source = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.Read);
        using var output = new FileStream(archive, FileMode.CreateNew, FileAccess.Write, FileShare.None);
        using var gzip = new GZipStream(output, CompressionLevel.Optimal);
        return Copy(source, gzip);
    }

    private static bool ArchiveMatches(string archive, byte[] expectedHash, long expectedLength)
    {
        using var input = new FileStream(archive, FileMode.Open, FileAccess.Read, FileShare.Read);
        using var gzip = new GZipStream(input, CompressionMode.Decompress);
        var (hash, length) = Copy(gzip, Stream.Null);
        return length == expectedLength && hash.AsSpan().SequenceEqual(expectedHash);
    }

    private static (byte[] Hash, long Length) Copy(Stream source, Stream destination)
    {
        using var hash = IncrementalHash.CreateHash(HashAlgorithmName.SHA256);
        var buffer = new byte[BufferSize];
        long length = 0;
        int read;
        while ((read = source.Read(buffer, 0, buffer.Length)) > 0)
        {
            hash.AppendData(buffer, 0, read);
            destination.Write(buffer, 0, read);
            length += read;
        }
        return (hash.GetHashAndReset(), length);
    }

    private static bool TryDelete(string path)
    {
        try
        {
            if (IsCompressed(path)) File.Delete(path);
            else DeleteIfUnused(path);
            return true;
        }
        catch (Exception exception) when (IsFileSystemFailure(exception))
        {
            return false;
        }
    }

    private static void DeleteIfUnused(string path)
    {
        // An uncompressed log that survived the compression pass is either
        // still being written or unreadable. Opening it exclusively with
        // DeleteOnClose removes it only when nobody else holds it open.
        using var _ = new FileStream(path, FileMode.Open, FileAccess.ReadWrite, FileShare.None, 1,
            FileOptions.DeleteOnClose);
    }

    private static void RemoveAbandonedTemporaryFiles(string directory)
    {
        // Leftovers from a launcher killed mid-compression. The original log is
        // only deleted after a verified move, so these are redundant copies.
        try
        {
            var cutoff = DateTime.UtcNow - TimeSpan.FromHours(1);
            var pattern = "AIMP.*" + CompressedExtension + ".*" + TemporaryExtension;
            foreach (var path in Directory.EnumerateFiles(directory, pattern))
                if (File.GetLastWriteTimeUtc(path) < cutoff) TryDeleteQuietly(path);
        }
        catch (Exception exception) when (IsFileSystemFailure(exception))
        {
        }
    }

    private static void TryDeleteQuietly(string path)
    {
        try
        {
            if (File.Exists(path)) File.Delete(path);
        }
        catch (Exception exception) when (IsFileSystemFailure(exception))
        {
        }
    }
}
