using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Net.Http;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using GyroTron.Manager.Link;

namespace GyroTron.Manager;

/// <summary>One game in the catalog index (docs/reference/CATALOG.md, schema 1).</summary>
public sealed record CatalogEntry(
    string Id, string Name, string Author, int Version,
    int ApiMajor, int ApiMinor, string Channel,
    long Size, string Sha256, string Url, string IconUrl,
    string[] Shots, string? Source, string? MinFw, string Summary)
{
    public bool RunsOn(int major, int minor) => ApiMajor == major && ApiMinor <= minor;
}

/// <summary>The result of loading the catalog: the entries, and where they came from -
/// a cached copy is still a catalog, it is just labelled stale.</summary>
public sealed record CatalogResult(List<CatalogEntry> Games, bool FromCache, string Generated, string? Error);

/// <summary>
/// The catalog the pane browses: index.json fetched over HTTPS, cached on disk so the
/// pane still works offline, packages downloaded into the library on request. The
/// catalog is only a source of bytes - downloading goes through Library.Add and
/// installing through Library.Install, so there is exactly one install path.
///
/// Urls in the index are relative to the /otadb/ root, which is two levels up from the
/// index itself (.../otadb/games/index.json); GYROTRON_CATALOG_URL points somewhere
/// else entirely (a local mock, a mirror) when set.
/// </summary>
public sealed class CatalogService
{
    /// <summary>The default catalog, on the same host the watch's firmware updates come from.</summary>
    public const string DefaultIndexUrl = "http://tat.secstant.ru/otadb/games/index.json";

    public string IndexUrl { get; }
    public string CachePath { get; }

    public CatalogService(string? indexUrl = null, string? cachePath = null)
    {
        IndexUrl = indexUrl
                   ?? Environment.GetEnvironmentVariable("GYROTRON_CATALOG_URL")
                   ?? DefaultIndexUrl;
        var home = Path.GetDirectoryName(new Library().Folder)!;
        CachePath = cachePath ?? Path.Combine(home, "catalog-index.json");
    }

    static readonly HttpClient Http = new() { Timeout = TimeSpan.FromSeconds(30) };

    /// <summary>Fetch the index (or read the cache when the host is unreachable). A
    /// fetch that succeeds overwrites the cache; a fetch that fails falls back to it
    /// rather than to an empty pane.</summary>
    public CatalogResult Load(bool refresh, Action<long, long>? progress = null)
    {
        string text;
        var fromCache = false;
        try
        {
            if (IsLocalPath(IndexUrl))
            {
                text = File.ReadAllText(IndexUrl);
            }
            else
            {
                using var stream = Http.GetStreamAsync(IndexUrl).GetAwaiter().GetResult();
                text = Encoding.UTF8.GetString(ReadAll(stream, progress));
            }
            try { Directory.CreateDirectory(Path.GetDirectoryName(CachePath)!); File.WriteAllText(CachePath, text); }
            catch (IOException) { /* a cache we cannot write is not worth failing over */ }
        }
        catch (Exception e) when (e is HttpRequestException or IOException or InvalidOperationException)
        {
            if (!File.Exists(CachePath))
                return new CatalogResult(new List<CatalogEntry>(), false, "", $"the catalog is unreachable ({e.Message}) and there is no cached copy yet");
            text = File.ReadAllText(CachePath);
            fromCache = true;
        }
        return Parse(text, fromCache);
    }

    /// <summary>Download a catalog entry's package and check it against the index's
    /// sha256 - the hash is the catalog's promise about which bytes these are.</summary>
    public byte[] Download(CatalogEntry entry, Action<long, long>? progress = null)
    {
        byte[] bytes;
        if (IsLocalPath(IndexUrl))
        {
            // A local catalog (file path) keeps its files beside the index, so the same
            // relative urls resolve against its directory.
            var path = Path.GetFullPath(Path.Combine(Path.GetDirectoryName(Path.GetFullPath(IndexUrl))!, "../..", entry.Url.Replace('/', Path.DirectorySeparatorChar)));
            bytes = File.ReadAllBytes(path);
            progress?.Invoke(bytes.Length, bytes.Length);
        }
        else
        {
            using var stream = Http.GetStreamAsync(Resolve(entry.Url)).GetAwaiter().GetResult();
            bytes = ReadAll(stream, progress);
        }
        var sha = Convert.ToHexString(SHA256.HashData(bytes)).ToLowerInvariant();
        if (sha != entry.Sha256)
            throw new InvalidDataException($"{entry.Name} downloaded as {sha[..12]}… but the catalog says {entry.Sha256[..12]}…");
        return bytes;
    }

    /// <summary>An icon or screenshot from the index, for the tiles.</summary>
    public byte[] DownloadImage(string url)
    {
        if (IsLocalPath(IndexUrl))
        {
            var path = Path.GetFullPath(Path.Combine(Path.GetDirectoryName(Path.GetFullPath(IndexUrl))!, "../..", url.Replace('/', Path.DirectorySeparatorChar)));
            return File.ReadAllBytes(path);
        }
        return Http.GetByteArrayAsync(Resolve(url)).GetAwaiter().GetResult();
    }

    /// <summary>Index urls are /otadb/-relative; the index sits two directories down
    /// (.../otadb/games/index.json), so the base is its grandparent.</summary>
    string Resolve(string url)
    {
        var uri = new Uri(IndexUrl);
        var path = uri.AbsolutePath;
        var baseDir = path.Substring(0, path.TrimEnd('/').LastIndexOf('/', path.TrimEnd('/').LastIndexOf('/') - 1));
        return $"{uri.Scheme}://{uri.Authority}{baseDir.TrimEnd('/')}/{url}";
    }

    static bool IsLocalPath(string url) =>
        !url.Contains("://") && (File.Exists(url) || !url.StartsWith("http"));

    static byte[] ReadAll(System.IO.Stream stream, Action<long, long>? progress)
    {
        using var ms = new MemoryStream();
        var buf = new byte[64 * 1024];
        long done = 0;
        int n;
        while ((n = stream.Read(buf, 0, buf.Length)) > 0)
        {
            ms.Write(buf, 0, n);
            done += n;
            progress?.Invoke(done, 0);
        }
        progress?.Invoke(done, done);
        return ms.ToArray();
    }

    static CatalogResult Parse(string text, bool fromCache)
    {
        List<CatalogEntry> games;
        string generated = "";
        try
        {
            using var doc = JsonDocument.Parse(text, new JsonDocumentOptions { AllowTrailingCommas = true });
            var root = doc.RootElement;
            generated = root.TryGetProperty("generated", out var g) ? g.GetString() ?? "" : "";
            games = root.GetProperty("games").EnumerateArray().Select(e => new CatalogEntry(
                e.GetProperty("id").GetString()!,
                e.GetProperty("name").GetString()!,
                e.GetProperty("author").GetString() ?? "",
                e.GetProperty("version").GetInt32(),
                e.GetProperty("api_major").GetInt32(),
                e.GetProperty("api_minor").GetInt32(),
                e.GetProperty("channel").GetString() ?? "community",
                e.GetProperty("size").GetInt64(),
                e.GetProperty("sha256").GetString() ?? "",
                e.GetProperty("url").GetString() ?? "",
                e.TryGetProperty("icon", out var ic) ? ic.GetString() ?? "" : "",
                e.TryGetProperty("shots", out var sh) ? sh.EnumerateArray().Select(s => s.GetString() ?? "").ToArray() : Array.Empty<string>(),
                e.TryGetProperty("source", out var src) && src.ValueKind != JsonValueKind.Null ? src.GetString() : null,
                e.TryGetProperty("min_fw", out var fw) && fw.ValueKind != JsonValueKind.Null ? fw.GetString() : null,
                e.TryGetProperty("summary", out var sum) ? sum.GetString() ?? "" : ""
            )).ToList();
        }
        catch (Exception e) when (e is JsonException or KeyNotFoundException or InvalidOperationException)
        {
            return new CatalogResult(new List<CatalogEntry>(), fromCache, "", $"the catalog index does not parse: {e.Message}");
        }
        return new CatalogResult(games, fromCache, generated, null);
    }
}
