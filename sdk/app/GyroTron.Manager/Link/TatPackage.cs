using System;
using System.Buffers.Binary;
using System.Collections.Generic;
using System.IO;
using System.Text;
using Org.BouncyCastle.Crypto.Parameters;
using Org.BouncyCastle.Crypto.Signers;

namespace GyroTron.Manager.Link;

/// <summary>The verdict on a package's SIG section. A broken or unknown-key signature
/// is not a state - it fails the parse, the way any other damage does.</summary>
public enum SignatureState
{
    /// <summary>No SIG section: everything packed before signatures, and every community package.</summary>
    None,
    /// <summary>Verified against one of the trusted keys - the official channel.</summary>
    Valid,
}

/// <summary>
/// Just enough of a .tat to decide whether it is worth sending: the header, and the checks
/// the watch would do anyway. Reading it here means a file that is not a package, or was
/// truncated on its way to you, is refused in a moment rather than after a transfer that
/// takes half a minute - and the reason can name the game.
///
/// The layout is docs/reference/GAME_API.md section 2, written by tools/mktat.py. The
/// SIG section (signatures) is parsed and verified here too: a broken signature fails
/// the parse outright, because a tampered official package is exactly the thing this
/// app must never install.
/// </summary>
public sealed record TatPackage(
    string Id, string Name, string Author,
    int Version, int ApiMajor, int ApiMinor,
    byte AccentR, byte AccentG, byte AccentB,
    int Kind, long Bytes)
{
    const int HeaderBytes = 96;
    public const int KindGame = 1;

    public bool IsGame => Kind == KindGame;

    /// <summary>The game's own icon as a PNG, if it packed one.</summary>
    public byte[]? IconPng { get; init; }

    /// <summary>The SIG section's verdict, and which key id it was made with.</summary>
    public SignatureState Signature { get; init; }
    public int SignatureKeyId { get; init; }

    /// <summary>The same rule the console applies: same major, and a minor it can satisfy.</summary>
    public bool RunsOn(int major, int minor) => ApiMajor == major && ApiMinor <= minor;

    // ------------------------------------------------------------------ trusted keys

    /// <summary>
    /// The keys a SIG section may verify against: the official channel's table plus
    /// whatever GYROTRON_TAT_PUBKEYS adds ("id:hex" pairs, comma separated, bare hex
    /// meaning id 0) - the same env the Python tools read, so a local catalog's keys can
    /// be trusted everywhere at once. The table here, in components/loader/keys.c and in
    /// backend/tools/tatpkg.py is one table in three spellings; rotation changes all three.
    /// </summary>
    public static IReadOnlyList<(byte Id, byte[] Key)> TrustedKeys { get; } = LoadTrustedKeys();

    static List<(byte, byte[])> LoadTrustedKeys()
    {
        var keys = new List<(byte, byte[])>
        {
            (0, Convert.FromHexString("9ece320a08a2b66f0ba3f594d05cdab3d4c345688571f9d89c00953080868213")),
        };
        foreach (var item in (Environment.GetEnvironmentVariable("GYROTRON_TAT_PUBKEYS") ?? "").Split(',', StringSplitOptions.RemoveEmptyEntries))
        {
            var parts = item.Trim().Split(':', 2);
            var hex = parts.Length == 2 ? parts[1].Trim() : parts[0].Trim();
            var id = parts.Length == 2 ? byte.Parse(parts[0]) : (byte)0;
            keys.RemoveAll(k => k.Item1 == id);   // an env key overrides the built-in with the same id
            keys.Add((id, Convert.FromHexString(hex)));
        }
        return keys;
    }

    public static TatPackage Read(string path) => Parse(File.ReadAllBytes(path));

    public static TatPackage Parse(byte[] bytes)
    {
        if (bytes.Length < HeaderBytes) throw new InvalidDataException("too short to be a package");

        var magic = Encoding.ASCII.GetString(bytes, 0, 6);
        if (magic != "TATPKG") throw new InvalidDataException("no TATPKG marker");
        if (bytes[7] != 1) throw new InvalidDataException($"format version {bytes[7]} is not one this app knows");

        var span = bytes.AsSpan();
        var headerCrc = BinaryPrimitives.ReadUInt32LittleEndian(span[8..]);
        var totalSize = BinaryPrimitives.ReadUInt32LittleEndian(span[12..]);
        if (totalSize != bytes.Length)
            throw new InvalidDataException($"says it is {totalSize} bytes but is {bytes.Length}");

        var kind = bytes[16];
        var apiMajor = BinaryPrimitives.ReadUInt16LittleEndian(span[18..]);
        var apiMinor = BinaryPrimitives.ReadUInt16LittleEndian(span[20..]);
        var version = BinaryPrimitives.ReadUInt16LittleEndian(span[22..]);
        var id = Str(span.Slice(24, 16));
        var name = Str(span.Slice(40, 24));
        var author = Str(span.Slice(64, 24));
        var sections = BinaryPrimitives.ReadUInt16LittleEndian(span[92..]);
        if (sections == 0 || sections > 64) throw new InvalidDataException("the section table makes no sense");

        // The header's CRC covers everything after the field itself, the section table
        // included, so a file that was cut short or meddled with is caught here.
        var covered = HeaderBytes + sections * 16 - 12;
        if (bytes.Length < 12 + covered) throw new InvalidDataException("the section table runs past the end");
        if (Crc32(span.Slice(12, covered)) != headerCrc) throw new InvalidDataException("it is damaged");

        if (id.Length == 0 || name.Length == 0) throw new InvalidDataException("it has no name");
        // The id becomes a file name, on the watch and in the library. The packer only ever
        // writes lower-case letters, digits and underscores; anything else was made by hand,
        // and "../" in a file name is not something to find out about afterwards.
        foreach (var ch in id)
            if (!(ch is (>= 'a' and <= 'z') or (>= '0' and <= '9') or '_'))
                throw new InvalidDataException("its id is not a plain name");
        if (kind != KindGame) throw new InvalidDataException("it is not a game");

        // Every section carries its own CRC, except SIG: its table entry is 0 by format
        // rule (a CRC over the signature bytes would be circular - the signature covers
        // the entry that would carry the CRC). Checking the rest costs nothing at these
        // sizes and means a file that was damaged on the watch, or on its way back from
        // one, is never quietly filed away as a good copy.
        byte[]? icon = null;
        int sigOffset = -1, sigLen = 0;
        for (var i = 0; i < sections; i++)
        {
            var entry = span.Slice(HeaderBytes + i * 16, 16);
            var tag = Encoding.ASCII.GetString(entry[..4]);
            var off = BinaryPrimitives.ReadUInt32LittleEndian(entry[4..]);
            var len = BinaryPrimitives.ReadUInt32LittleEndian(entry[8..]);
            var crc = BinaryPrimitives.ReadUInt32LittleEndian(entry[12..]);
            if ((ulong)off + len > (ulong)bytes.Length) throw new InvalidDataException("a section runs past the end");
            var payload = span.Slice((int)off, (int)len);
            if (tag == "SIG ")
            {
                if (sigOffset >= 0) throw new InvalidDataException("it carries two signatures");
                if (crc != 0 || len != 65 || (ulong)off + len != (ulong)bytes.Length)
                    throw new InvalidDataException("its signature section is malformed");
                sigOffset = (int)off;
                sigLen = (int)len;
                continue;
            }
            if (Crc32(payload) != crc) throw new InvalidDataException("it is damaged");
            if (tag == "ICON" && icon is null) icon = payload.ToArray();
        }

        var signature = SignatureState.None;
        var signatureKeyId = 0;
        if (sigOffset >= 0)
        {
            var keyId = bytes[sigOffset];
            var key = FindKey(keyId);
            // The signature covers everything before it: header, table, every other
            // section - so nothing about the package can change after signing.
            if (key is null)
                throw new InvalidDataException($"it is signed with key {keyId}, which this app does not know");
            var signer = new Ed25519Signer();
            signer.Init(false, new Ed25519PublicKeyParameters(key, 0));
            signer.BlockUpdate(bytes, 0, sigOffset);
            if (!signer.VerifySignature(bytes[(sigOffset + 1)..]))
                throw new InvalidDataException("its signature is broken - the package has been changed since it was signed");
            signature = SignatureState.Valid;
            signatureKeyId = keyId;
        }

        return new TatPackage(id, name, author, version, apiMajor, apiMinor,
                              bytes[88], bytes[89], bytes[90], kind, bytes.Length)
        { IconPng = icon, Signature = signature, SignatureKeyId = signatureKeyId };
    }

    static byte[]? FindKey(byte id)
    {
        foreach (var (kid, key) in TrustedKeys)
            if (kid == id) return key;
        return null;
    }

    static string Str(ReadOnlySpan<byte> fixedField)
    {
        var end = fixedField.IndexOf((byte)0);
        return Encoding.UTF8.GetString(end < 0 ? fixedField : fixedField[..end]).Trim();
    }

    /// <summary>The same CRC-32 the watch and the packer use.</summary>
    static uint Crc32(ReadOnlySpan<byte> data)
    {
        var crc = 0xFFFFFFFFu;
        foreach (var b in data)
        {
            crc ^= b;
            for (var i = 0; i < 8; i++) crc = (crc >> 1) ^ (0xEDB88320u & (uint)(-(int)(crc & 1)));
        }
        return ~crc;
    }
}
