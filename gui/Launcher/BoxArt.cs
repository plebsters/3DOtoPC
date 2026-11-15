// BoxArt.cs - optional box art download from the libretro thumbnails collection
// (github.com/libretro-thumbnails/The_3DO_Company_-_3DO). Nothing is fetched
// unless the user asks: the "Get box art" button or a tile's menu.
//
// The collection names covers after the disc ("Crash 'n Burn (USA).png").
// Game folders are named by the converter ("Crash_n_Burn"), so names are
// compared loosely: case, punctuation, region tags and subtitles are ignored.
using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Net.Http;
using System.Text;
using System.Text.Json;
using System.Threading;
using System.Threading.Tasks;

namespace TdoLauncher;

public sealed record BoxArtMatch(string FileName, string Url, double Score);

public static class BoxArt
{
    const string Repo = "libretro-thumbnails/The_3DO_Company_-_3DO";
    const string Branch = "master";
    const string Folder = "Named_Boxarts/";
    static readonly TimeSpan IndexMaxAge = TimeSpan.FromDays(7);

    static readonly HttpClient Http = CreateClient();
    static List<string>? _names;

    static HttpClient CreateClient()
    {
        var c = new HttpClient { Timeout = TimeSpan.FromSeconds(30) };
        c.DefaultRequestHeaders.UserAgent.ParseAdd("3DO-Launcher/1.0");   // GitHub requires one
        return c;
    }

    static string CachePath => Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.ApplicationData),
                                            "3DO to PC", "boxart-index.json");

    public static string UrlFor(string fileName) =>
        $"https://raw.githubusercontent.com/{Repo}/{Branch}/Named_Boxarts/{Uri.EscapeDataString(fileName)}";

    /// All box art file names in the collection. Cached for a week (one request).
    public static async Task<List<string>> GetIndexAsync(CancellationToken ct = default)
    {
        if (_names != null) return _names;
        try
        {
            if (File.Exists(CachePath) && DateTime.Now - File.GetLastWriteTime(CachePath) < IndexMaxAge)
            {
                var cached = JsonSerializer.Deserialize<List<string>>(await File.ReadAllTextAsync(CachePath, ct));
                if (cached is { Count: > 0 }) return _names = cached;
            }
        }
        catch { /* bad cache: fetch again */ }

        string json = await Http.GetStringAsync($"https://api.github.com/repos/{Repo}/git/trees/{Branch}?recursive=1", ct);
        using var doc = JsonDocument.Parse(json);
        var names = new List<string>();
        foreach (var e in doc.RootElement.GetProperty("tree").EnumerateArray())
        {
            string path = e.GetProperty("path").GetString() ?? "";
            if (path.StartsWith(Folder, StringComparison.Ordinal) && path.EndsWith(".png", StringComparison.OrdinalIgnoreCase))
                names.Add(path.Substring(Folder.Length));
        }
        if (names.Count == 0) throw new InvalidDataException("The box art list was empty.");
        try
        {
            Directory.CreateDirectory(Path.GetDirectoryName(CachePath)!);
            await File.WriteAllTextAsync(CachePath, JsonSerializer.Serialize(names), ct);
        }
        catch { /* cache is optional */ }
        return _names = names;
    }

    // ---- name matching -------------------------------------------------------

    static readonly HashSet<string> Filler = new() { "the", "a", "an", "of" };

    /// "Crash 'n Burn (USA).png" -> "crash n burn"; "Slam_and_Jam_95" -> "slam n jam 95".
    public static string Normalize(string name, bool dropSubtitle = false)
    {
        name = Path.GetFileNameWithoutExtension(name.EndsWith(".png", StringComparison.OrdinalIgnoreCase) ? name : name + ".x");
        var sb = new StringBuilder();
        int depth = 0;
        foreach (char ch in name)      // drop (region) and [tag] groups
        {
            if (ch == '(' || ch == '[') { depth++; continue; }
            if (ch == ')' || ch == ']') { if (depth > 0) depth--; continue; }
            if (depth == 0) sb.Append(ch);
        }
        string s = sb.ToString().Replace('_', ' ');
        if (dropSubtitle)
        {
            int dash = s.IndexOf(" - ", StringComparison.Ordinal);
            if (dash > 0) s = s.Substring(0, dash);
        }
        var words = new List<string>();
        var w = new StringBuilder();
        void Flush()
        {
            if (w.Length == 0) return;
            string t = w.ToString().ToLowerInvariant();
            w.Clear();
            if (t == "and") t = "n";
            if (!Filler.Contains(t)) words.Add(t);
        }
        for (int i = 0; i < s.Length; ++i)
        {
            char ch = s[i];
            // "SlamNJam" -> "Slam N Jam" (folder names typed without spaces):
            // a capital after a small letter, or a capital starting a word after capitals.
            char prev = i > 0 ? s[i - 1] : ' ', next = i + 1 < s.Length ? s[i + 1] : ' ';
            if (char.IsUpper(ch) && (char.IsLower(prev) || (char.IsUpper(prev) && char.IsLower(next)))) Flush();
            if (char.IsLetterOrDigit(ch)) w.Append(ch);
            else if (ch == '\'' || ch == '’' || ch == '.') { /* "'n", "Dr." join */ }
            else Flush();
        }
        Flush();
        return string.Join(' ', words);
    }

    /// Preference among covers of the same game: USA first, odd variants last.
    static int RegionRank(string fileName)
    {
        string f = fileName.ToLowerInvariant();
        int r = f.Contains("(usa)") ? 0 : f.Contains("(usa,") ? 1 : f.Contains("(world)") ? 2 : f.Contains("europe") ? 3 : f.Contains("japan") ? 5 : 4;
        if (f.Contains("(alt") || f.Contains("not for resale") || f.Contains("demo") || f.Contains("(proto") || f.Contains("sample")) r += 10;
        // Fewer extra tags first: "(USA)" before "(USA) (RE2)".
        return r * 10 + f.Count(c => c == '(' || c == '[');
    }

    static double Similarity(string a, string b)
    {
        if (a.Length == 0 || b.Length == 0) return 0;
        if (a == b) return 1;
        var ta = a.Split(' '); var tb = b.Split(' ');
        int common = ta.Intersect(tb).Count();
        double jaccard = (double)common / ta.Union(tb).Count();
        // A folder name the converter shortened is a prefix of the full title.
        bool prefix = b.StartsWith(a + " ", StringComparison.Ordinal) || a.StartsWith(b + " ", StringComparison.Ordinal);
        return Math.Max(jaccard, prefix ? 0.85 : 0);
    }

    /// Candidates for a game, best first. Score 1 = same title.
    public static List<BoxArtMatch> Rank(string gameName, IEnumerable<string> index)
    {
        string g = Normalize(gameName), gShort = Normalize(gameName, dropSubtitle: true);
        return index
            .Select(f => (f, score: Math.Max(Similarity(g, Normalize(f)), Math.Max(Similarity(gShort, Normalize(f, true)) - 0.02, 0))))
            .Where(x => x.score > 0.3)
            .OrderByDescending(x => Math.Round(x.score, 2))
            .ThenBy(x => RegionRank(x.f))
            .ThenBy(x => x.f, StringComparer.OrdinalIgnoreCase)
            .Select(x => new BoxArtMatch(x.f, UrlFor(x.f), x.score))
            .ToList();
    }

    /// The cover to use without asking, or null when nothing is close enough.
    public static BoxArtMatch? Best(string gameName, IEnumerable<string> index) =>
        Rank(gameName, index).FirstOrDefault(m => m.Score >= 0.75);

    /// Without the list (GitHub refused it), try the likely file names directly.
    public static async Task<BoxArtMatch?> GuessAndSaveAsync(string gameName, string gameFolder, CancellationToken ct = default)
    {
        string title = gameName.Replace('_', ' ').Trim();
        foreach (var region in new[] { "USA", "USA, Europe", "World", "Europe" })
        {
            var m = new BoxArtMatch($"{title} ({region}).png", UrlFor($"{title} ({region}).png"), 1);
            try { await SaveAsync(m, gameFolder, ct); return m; }
            catch (HttpRequestException) { /* not under this name */ }
        }
        return null;
    }

    /// Downloads a cover into the game folder as cover.png, replacing any cover there.
    public static async Task SaveAsync(BoxArtMatch m, string gameFolder, CancellationToken ct = default)
    {
        byte[] png = await Http.GetByteArrayAsync(m.Url, ct);
        if (png.Length < 8 || png[0] != 0x89 || png[1] != (byte)'P') throw new InvalidDataException("The download was not a PNG image.");
        string tmp = Path.Combine(gameFolder, "cover.download");
        await File.WriteAllBytesAsync(tmp, png, ct);
        if (GameLibrary.FindCoverFile(gameFolder) is { } old) File.Delete(old);
        File.Move(tmp, Path.Combine(gameFolder, "cover.png"), true);
    }
}
