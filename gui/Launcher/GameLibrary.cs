// GameLibrary.cs - finds converted games on disk and remembers launcher settings.
//
// A converted game is a folder holding the game's .exe and its data\ folder
// (what 3DO to PC writes to games\<name>\). The launcher scans the games
// folder one level deep; it also accepts a single game folder.
using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Text.Json;
using System.Windows.Media;
using System.Windows.Media.Imaging;

namespace TdoLauncher;

public sealed class Game
{
    public required string Name { get; init; }
    public required string Folder { get; init; }
    public required string ExePath { get; init; }
    public ImageSource? Cover { get; set; }
    public string Initials => string.Concat(Name.Split(' ', StringSplitOptions.RemoveEmptyEntries)
                                                .Where(w => char.IsUpper(w[0]) || char.IsDigit(w[0])).Take(3).Select(w => char.ToUpperInvariant(w[0])));
    public DateTime? LastPlayed { get; set; }
    public int PlayCount { get; set; }
    public string Subtitle => LastPlayed is { } t ? $"Played {Ago(t)}" : "Never played";

    static string Ago(DateTime t)
    {
        var d = DateTime.Now - t;
        if (d.TotalMinutes < 1) return "just now";
        if (d.TotalHours < 1) return $"{(int)d.TotalMinutes} min ago";
        if (d.TotalDays < 1) return $"{(int)d.TotalHours} h ago";
        if (d.TotalDays < 2) return "yesterday";
        return t.ToString("MMM d");
    }
}

public sealed class GameStats { public DateTime? LastPlayed { get; set; } public int PlayCount { get; set; } }

public sealed class LauncherSettings
{
    public string? GamesFolder { get; set; }
    public string Sort { get; set; } = "Name";
    public Dictionary<string, GameStats> Stats { get; set; } = new(StringComparer.OrdinalIgnoreCase);

    static string FilePath => Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.ApplicationData),
                                           "3DO to PC", "launcher.json");

    public static LauncherSettings Load()
    {
        try
        {
            if (File.Exists(FilePath))
            {
                var s = JsonSerializer.Deserialize<LauncherSettings>(File.ReadAllText(FilePath));
                if (s != null)
                {
                    s.Stats = new Dictionary<string, GameStats>(s.Stats, StringComparer.OrdinalIgnoreCase);
                    return s;
                }
            }
        }
        catch { /* unreadable settings: start fresh */ }
        return new LauncherSettings();
    }

    public void Save()
    {
        try
        {
            Directory.CreateDirectory(Path.GetDirectoryName(FilePath)!);
            File.WriteAllText(FilePath, JsonSerializer.Serialize(this, new JsonSerializerOptions { WriteIndented = true }));
        }
        catch { /* settings are a convenience; never block the launcher */ }
    }
}

public static class GameLibrary
{
    static readonly string[] CoverNames = { "cover", "boxart", "box", "folder", "front" };
    static readonly string[] ImageExts = { ".png", ".jpg", ".jpeg", ".bmp", ".gif" };

    /// The games folder to use when none is set: "games" next to the launcher,
    /// or next to its parent (the launcher sits in dist\, games\ beside it).
    public static string DefaultGamesFolder()
    {
        string exeDir = AppContext.BaseDirectory;
        foreach (var candidate in new[] { Path.Combine(exeDir, "games"), Path.Combine(exeDir, "..", "games") })
            if (Directory.Exists(candidate)) return Path.GetFullPath(candidate);
        return Path.GetFullPath(Path.Combine(exeDir, "games"));
    }

    public static List<Game> Scan(string root, LauncherSettings settings)
    {
        var games = new List<Game>();
        if (!Directory.Exists(root)) return games;

        // The chosen folder may itself be a single game.
        if (TryGame(root, settings) is { } single) { games.Add(single); return games; }

        IEnumerable<string> dirs;
        try { dirs = Directory.EnumerateDirectories(root); } catch { return games; }
        foreach (var dir in dirs)
            if (TryGame(dir, settings) is { } g) games.Add(g);
        return games;
    }

    static Game? TryGame(string dir, LauncherSettings settings)
    {
        string folderName = Path.GetFileName(Path.TrimEndingDirectorySeparator(dir));
        string[] exes;
        try { exes = Directory.GetFiles(dir, "*.exe"); } catch { return null; }
        if (exes.Length == 0 || !Directory.Exists(Path.Combine(dir, "data"))) return null;

        // Prefer the exe named after the folder (how 3DO to PC names it).
        string exe = exes.FirstOrDefault(e => Path.GetFileNameWithoutExtension(e).Equals(folderName, StringComparison.OrdinalIgnoreCase))
                     ?? exes.FirstOrDefault(e => !Path.GetFileName(e).StartsWith("unins", StringComparison.OrdinalIgnoreCase))
                     ?? exes[0];

        var game = new Game
        {
            Name = folderName.Replace('_', ' ').Trim(),
            Folder = dir,
            ExePath = exe,
            Cover = LoadCover(dir),
        };
        if (settings.Stats.TryGetValue(dir, out var st)) { game.LastPlayed = st.LastPlayed; game.PlayCount = st.PlayCount; }
        return game;
    }

    public static string? FindCoverFile(string dir)
    {
        foreach (var name in CoverNames)
            foreach (var ext in ImageExts)
            {
                string p = Path.Combine(dir, name + ext);
                if (File.Exists(p)) return p;
            }
        return null;
    }

    public static ImageSource? LoadCover(string dir)
    {
        string? path = FindCoverFile(dir);
        if (path == null) return null;
        try
        {
            // Load fully into memory so the file is not kept locked (it can be replaced).
            var bmp = new BitmapImage();
            bmp.BeginInit();
            bmp.CacheOption = BitmapCacheOption.OnLoad;
            bmp.CreateOptions = BitmapCreateOptions.IgnoreImageCache;
            bmp.DecodePixelWidth = 400;
            bmp.UriSource = new Uri(path);
            bmp.EndInit();
            bmp.Freeze();
            return bmp;
        }
        catch { return null; }
    }
}
