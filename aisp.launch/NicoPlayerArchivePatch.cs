using System.Buffers.Binary;
using System.Text;

namespace aisp.launch;

/// <summary>
/// Redirects the client's AISP TV URL templates to the launcher's loopback player.
/// The client expands these templates from the Nico TV packet before passing the
/// resulting URL to Trident.
/// </summary>
internal static class NicoPlayerArchivePatch
{
    private const uint ImageScnCntCode = 0x00000020;
    private static readonly TemplateReplacement[] TemplateReplacements =
    [
        new(
            "http://aisp.jp/player/jdfoiajwpefha/nicoplayer.php?movieid=%s",
            "http://127.0.0.1/p?movieid=%s#///////////////////////////////"
        ),
        new(
            "http://aisp.jp/player/jdfoiajwpefha/nicoplayer.php?tvid=%d&chid=%d",
            "http://127.0.0.1/p?tvid=%d&chid=%d#///////////////////////////////"
        ),
        // Undo the previous Nico Live experiment if this launcher installed it.
        new(
            "http://127.0.0.1/watch/%s?npwarn=false#player////////",
            "http://live.nicovideo.jp/watch/%s?npwarn=false#player"
        ),
    ];

    public static GameLaunchResult TryApply(string gameExecutable)
    {
        try
        {
            var gameDirectory = Path.GetDirectoryName(gameExecutable);
            if (string.IsNullOrWhiteSpace(gameDirectory))
                return GameLaunchResult.Failure("Unable to locate the game directory.");

            var dataDirectory = Path.Combine(gameDirectory, "data");
            var headerPath = Path.Combine(dataDirectory, "settings.hed");
            if (!File.Exists(headerPath))
                return GameLaunchResult.Failure("AISP TV settings archive was not found.", headerPath);

            var archiveKey = DeriveArchiveKey(gameExecutable);
            var header = File.ReadAllBytes(headerPath);
            if (header.Length < 16 || !header.AsSpan(0, 4).SequenceEqual("FPMF"u8))
                return GameLaunchResult.Failure("AISP TV settings archive has an unexpected format.", headerPath);

            var bodySize = checked((int)BinaryPrimitives.ReadUInt32LittleEndian(header.AsSpan(8, 4)));
            if (bodySize < 0 || header.Length < 16 + bodySize)
                return GameLaunchResult.Failure("AISP TV settings archive is truncated.", headerPath);

            var body = header.AsSpan(16, bodySize).ToArray();
            TransformSubtract(body, archiveKey);

            var dataKey = GetDataKey(body);
            var entry = GetStringTableEntry(body);
            var dataPath = Path.Combine(dataDirectory, "settings", $"{entry.DataFileId:x4}.dat");
            if (!File.Exists(dataPath))
                return GameLaunchResult.Failure("AISP TV data archive was not found.", dataPath);

            var encrypted = ReadRange(dataPath, entry.Offset, entry.Size);
            TransformSubtract(encrypted, dataKey);

            foreach (var replacement in TemplateReplacements)
                ApplyTemplateReplacement(encrypted, replacement);

            TransformAdd(encrypted, dataKey);
            WriteRange(dataPath, entry.Offset, encrypted);
            return GameLaunchResult.Success(gameExecutable);
        }
        catch (Exception ex) when (ex is IOException or UnauthorizedAccessException or InvalidDataException or OverflowException)
        {
            return GameLaunchResult.Failure("Failed to configure the local AISP TV player.", ex.Message);
        }
    }

    private static byte[] DeriveArchiveKey(string executable)
    {
        var image = File.ReadAllBytes(executable);
        if (image.Length < 0x40)
            throw new InvalidDataException("Game executable is truncated.");

        var peOffset = checked((int)BinaryPrimitives.ReadUInt32LittleEndian(image.AsSpan(0x3C, 4)));
        if (peOffset < 0 || image.Length < peOffset + 24 || !image.AsSpan(peOffset, 4).SequenceEqual("PE\0\0"u8))
            throw new InvalidDataException("Game executable is not a valid PE file.");

        var sectionCount = BinaryPrimitives.ReadUInt16LittleEndian(image.AsSpan(peOffset + 6, 2));
        var optionalHeaderSize = BinaryPrimitives.ReadUInt16LittleEndian(image.AsSpan(peOffset + 20, 2));
        var sectionOffset = checked(peOffset + 24 + optionalHeaderSize);

        for (var index = 0; index < sectionCount; index++)
        {
            var offset = checked(sectionOffset + index * 40);
            if (image.Length < offset + 40)
                throw new InvalidDataException("Game executable has a truncated section table.");

            var characteristics = BinaryPrimitives.ReadUInt32LittleEndian(image.AsSpan(offset + 36, 4));
            if ((characteristics & ImageScnCntCode) == 0)
                continue;

            var size = checked((int)BinaryPrimitives.ReadUInt32LittleEndian(image.AsSpan(offset + 16, 4)));
            var rawOffset = checked((int)BinaryPrimitives.ReadUInt32LittleEndian(image.AsSpan(offset + 20, 4)));
            if (rawOffset < 0 || size < 0 || image.Length < rawOffset + size)
                throw new InvalidDataException("Game executable has a truncated code section.");

            var key = new byte[64];
            for (var byteIndex = 0; byteIndex < size; byteIndex++)
                key[byteIndex & 63] = unchecked((byte)(byteIndex + image[rawOffset + byteIndex] + key[byteIndex & 63]));
            return key;
        }

        throw new InvalidDataException("Game executable has no code section.");
    }

    private static byte[] GetDataKey(ReadOnlySpan<byte> body)
    {
        var record = GetRecord(body, 2);
        if (record.Length < 4)
            throw new InvalidDataException("AISP TV archive has an invalid data key record.");

        var keyLength = checked((int)BinaryPrimitives.ReadUInt32LittleEndian(record[..4]));
        if (keyLength <= 0 || record.Length < 4 + keyLength)
            throw new InvalidDataException("AISP TV archive has an invalid data key.");
        return record.Slice(4, keyLength).ToArray();
    }

    private static ArchiveEntry GetStringTableEntry(ReadOnlySpan<byte> body)
    {
        var record = GetRecord(body, 3);
        if (record.Length < 4)
            throw new InvalidDataException("AISP TV archive has an invalid file table.");

        var count = checked((int)BinaryPrimitives.ReadUInt32LittleEndian(record[..4]));
        var offset = 4;
        for (var index = 0; index < count; index++)
        {
            var folder = ReadWideString(record, ref offset);
            var name = ReadWideString(record, ref offset);
            if (record.Length < offset + 20)
                throw new InvalidDataException("AISP TV archive has a truncated file entry.");

            var dataFileId = BinaryPrimitives.ReadUInt32LittleEndian(record.Slice(offset, 4));
            var fileOffset = BinaryPrimitives.ReadUInt32LittleEndian(record.Slice(offset + 4, 4));
            var fileSize = BinaryPrimitives.ReadUInt32LittleEndian(record.Slice(offset + 8, 4));
            offset += 20; // data file id, offset, size, and the 64-bit timestamp

            if (string.Equals(folder + name, ".\\settings\\.\\tps_str_table.csv", StringComparison.OrdinalIgnoreCase))
                return new ArchiveEntry(dataFileId, fileOffset, fileSize);
        }

        throw new InvalidDataException("AISP TV string table entry was not found.");
    }

    private static ReadOnlySpan<byte> GetRecord(ReadOnlySpan<byte> body, uint wantedId)
    {
        var offset = 0;
        while (offset < body.Length)
        {
            if (body.Length < offset + 8)
                break;

            var id = BinaryPrimitives.ReadUInt32LittleEndian(body.Slice(offset, 4));
            var size = checked((int)BinaryPrimitives.ReadUInt32LittleEndian(body.Slice(offset + 4, 4)));
            offset += 8;
            if (size < 0 || body.Length < offset + size)
                throw new InvalidDataException("AISP TV archive contains an invalid record.");
            if (id == wantedId)
                return body.Slice(offset, size);
            offset += size;
        }

        throw new InvalidDataException($"AISP TV archive record {wantedId} was not found.");
    }

    private static string ReadWideString(ReadOnlySpan<byte> data, ref int offset)
    {
        if (offset >= data.Length)
            throw new InvalidDataException("AISP TV archive has a truncated string.");

        var characterCount = data[offset++];
        var byteCount = checked(characterCount * sizeof(char));
        if (data.Length < offset + byteCount)
            throw new InvalidDataException("AISP TV archive has a truncated string.");

        var value = Encoding.Unicode.GetString(data.Slice(offset, byteCount));
        offset += byteCount;
        return value;
    }

    private static byte[] ReadRange(string path, uint offset, uint size)
    {
        var buffer = new byte[checked((int)size)];
        using var stream = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.Read);
        stream.Position = offset;
        stream.ReadExactly(buffer);
        return buffer;
    }

    private static void WriteRange(string path, uint offset, byte[] data)
    {
        using var stream = new FileStream(path, FileMode.Open, FileAccess.Write, FileShare.Read);
        stream.Position = offset;
        stream.Write(data);
    }

    private static void TransformSubtract(Span<byte> data, ReadOnlySpan<byte> key)
    {
        for (var index = 0; index < data.Length; index++)
            data[index] = unchecked((byte)(data[index] - key[index % key.Length]));
    }

    private static void TransformAdd(Span<byte> data, ReadOnlySpan<byte> key)
    {
        for (var index = 0; index < data.Length; index++)
            data[index] = unchecked((byte)(data[index] + key[index % key.Length]));
    }

    private static void ApplyTemplateReplacement(Span<byte> data, TemplateReplacement replacement)
    {
        var original = Encoding.ASCII.GetBytes(replacement.Original);
        var local = Encoding.ASCII.GetBytes(replacement.Local);
        if (original.Length != local.Length)
            throw new InvalidOperationException("AISP TV template replacement must not change the archive entry length.");

        var offset = data.IndexOf(original);
        if (offset >= 0)
            local.CopyTo(data[offset..]);
        else if (data.IndexOf(local) < 0)
            throw new InvalidDataException("AISP TV URL template was not found in the game data.");
    }

    private readonly record struct ArchiveEntry(uint DataFileId, uint Offset, uint Size);

    private readonly record struct TemplateReplacement(string Original, string Local);
}
