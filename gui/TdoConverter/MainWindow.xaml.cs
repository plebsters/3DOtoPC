using System;
using System.Diagnostics;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Text.RegularExpressions;
using System.Windows;
using System.Windows.Input;
using Microsoft.Win32;

namespace TdoConverter;

public partial class MainWindow : Window
{
    private readonly ConversionService _svc = new();
    private bool _busy;
    private string? _suggestedOut;     // the output folder we filled in (replaced when another disc is chosen)
    private bool _batchRunning, _batchStop;
    private string? _batchLabel;       // "Batch 2 of 5: Gex" while a batch runs

    /// The "games" folder in the app's folder (the installer makes it, and the
    /// launcher lists what is in it); next to dist\ when run from a build.
    private static string GamesFolder()
    {
        string exeDir = AppContext.BaseDirectory;
        foreach (var candidate in new[] { Path.Combine(exeDir, "games"), Path.Combine(exeDir, "..", "games") })
            if (Directory.Exists(candidate)) return Path.GetFullPath(candidate);
        string dir = Path.GetFullPath(Path.Combine(exeDir, "games"));
        try { Directory.CreateDirectory(dir); } catch { }
        return dir;
    }

    private static readonly Regex DiscTag = new(@"\(Disc (\d+) of (\d+)\)", RegexOptions.IgnoreCase);

    /// A folder name from a disc image name: "Samurai Shodown (1994)(Crystal
    /// Dynamics)(EU-US)[!]" -> "Samurai Shodown". A disc converted on its own
    /// (no disc 1 next to it) keeps its number: "Night Trap Disc 2".
    private static string GameFolderName(string discPath, bool withDiscNumber = false)
    {
        string name = Path.GetFileNameWithoutExtension(discPath);
        var disc = DiscTag.Match(name);
        int cut = name.IndexOfAny(new[] { '(', '[' });
        string title = (cut > 0 ? name[..cut] : name).Trim();
        if (title.Length == 0) title = name;
        if (withDiscNumber && disc.Success && disc.Groups[1].Value != "1") title += " Disc " + disc.Groups[1].Value;
        foreach (char c in Path.GetInvalidFileNameChars()) title = title.Replace(c, '_');
        return title;
    }

    /// The discs of a multi-disc game, found by name in the same folder, as
    /// Convert-3DO.bat does: "Game (Disc 2 of 2).cue" -> disc 1 and [disc 2].
    /// Anything else, or a set whose disc 1 is missing, is a single disc.
    private static (string first, List<string> extras, bool isSet) DiscSet(string path)
    {
        string dir = Path.GetDirectoryName(path) ?? ".", file = Path.GetFileName(path);
        var m = DiscTag.Match(file);
        if (!m.Success || !int.TryParse(m.Groups[2].Value, out int count) || count < 2 || count > 15)
            return (path, new List<string>(), false);
        string Find(int k)
        {
            string guess = file[..m.Index] + $"(Disc {k} of {count})" + file[(m.Index + m.Length)..];
            string stem = Path.GetFileNameWithoutExtension(guess);
            foreach (string ext in new[] { Path.GetExtension(file), ".cue", ".iso", ".bin" })
            {
                string candidate = Path.Combine(dir, stem + ext);
                if (File.Exists(candidate)) return candidate;
            }
            return "";
        }
        string first = Find(1);
        if (first.Length == 0) return (path, new List<string>(), false);
        var extras = new List<string>();
        for (int k = 2; k <= count; ++k) { string d = Find(k); if (d.Length > 0) extras.Add(d); }
        return (first, extras, true);
    }

    public MainWindow()
    {
        InitializeComponent();
        _svc.Log += (level, msg) => Dispatcher.InvokeAsync(() => AppendLog(level, msg));
        _svc.Progress += (stage, f) => Dispatcher.InvokeAsync(() =>
        {
            StageText.Text = _batchLabel is null ? stage.ToString() : $"{_batchLabel} · {stage}";
            Progress.Value = stage == Stage.Done ? 1 : f;
        });
        Loaded += (_, _) =>
        {
            try
            {
                var (found, desc) = NativeMethods.DetectToolchain();
                ToolchainText.Text = $"{NativeMethods.Version()} · Compiler: {desc}";
                if (!found) BuildCheck.IsChecked = false;
            }
            catch (DllNotFoundException)
            {
                ToolchainText.Text = "tdocore.dll not found next to the app; build the native core first.";
                ConvertButton.IsEnabled = false;
                BatchButton.IsEnabled = false;
            }
        };
    }

    private void AppendLog(int level, string msg)
    {
        string prefix = level switch { 1 => "warning: ", 2 => "error: ", _ => "" };
        LogBox.AppendText(prefix + msg + Environment.NewLine);
        LogBox.ScrollToEnd();
    }

    private void BrowseDisc_Click(object sender, RoutedEventArgs e)
    {
        var dlg = new OpenFileDialog { Filter = "3DO disc images (*.iso;*.bin;*.cue)|*.iso;*.bin;*.cue|All files|*.*" };
        if (dlg.ShowDialog(this) != true) return;
        var set = DiscSet(dlg.FileName);
        DiscBox.Text = set.first;
        if (set.isSet)
            AppendLog(0, $"Multi-disc game: {1 + set.extras.Count} disc(s) found next to it; they are converted together.");
        // Converted games go to games\<title> in the app's folder, where the
        // launcher finds them. A folder the user picked is left alone.
        if (string.IsNullOrWhiteSpace(OutBox.Text) || OutBox.Text == _suggestedOut)
            OutBox.Text = _suggestedOut = Path.Combine(GamesFolder(), GameFolderName(set.first, !set.isSet));
    }

    private void BrowseOut_Click(object sender, RoutedEventArgs e)
    {
        var dlg = new OpenFolderDialog { InitialDirectory = GamesFolder() };
        if (dlg.ShowDialog(this) == true) OutBox.Text = dlg.FolderName;
    }

    private void BrowseHints_Click(object sender, RoutedEventArgs e)
    {
        var dlg = new OpenFileDialog { Filter = "Runtime hints (interp_hits.txt)|*.txt|All files|*.*" };
        if (dlg.ShowDialog(this) == true) HintsBox.Text = dlg.FileName;
    }

    private async void Inspect_Click(object sender, RoutedEventArgs e)
    {
        if (_busy || !File.Exists(DiscBox.Text)) return;
        LogBox.Clear();
        SetBusy(true);
        try { await _svc.InspectAsync(DiscBox.Text); }
        finally { SetBusy(false); }
    }

    private async void Convert_Click(object sender, RoutedEventArgs e)
    {
        if (_busy) return;
        if (!File.Exists(DiscBox.Text)) { MessageBox.Show(this, "Choose a disc image first."); return; }
        if (string.IsNullOrWhiteSpace(OutBox.Text)) { MessageBox.Show(this, "Choose an output folder."); return; }

        LogBox.Clear();
        SetBusy(true);
        var set = DiscSet(DiscBox.Text);
        var req = new ConversionRequest(DiscBox.Text, OutBox.Text, NameBox.Text, HintsBox.Text, null,
                                        BuildCheck.IsChecked == true, ScanCheck.IsChecked == true,
                                        ReleaseCheck.IsChecked == true)
        { ExtraDiscs = set.isSet && set.first == DiscBox.Text ? set.extras : new List<string>() };
        try
        {
            int rc = await _svc.ConvertAsync(req);
            StageText.Text = rc == 0 ? "Finished" : $"Failed (code {rc})";
            OpenButton.IsEnabled = Directory.Exists(OutBox.Text);
        }
        finally { SetBusy(false); }
    }

    // Batch: several disc images, converted one after another into
    // games\<title>\ with the options above. A game whose folder already holds
    // an .exe is skipped (delete the folder to convert it again). While it
    // runs, the button stops the batch after the current game; a conversion
    // in progress is not cut off.
    private async void Batch_Click(object sender, RoutedEventArgs e)
    {
        if (_batchRunning) { _batchStop = true; BatchButton.IsEnabled = false; BatchButton.Content = "Stopping…"; return; }
        if (_busy) return;
        var dlg = new OpenFileDialog
        {
            Title = "Choose the disc images to convert",
            Filter = "3DO disc images (*.iso;*.bin;*.cue)|*.iso;*.bin;*.cue|All files|*.*",
            Multiselect = true,
        };
        if (dlg.ShowDialog(this) != true || dlg.FileNames.Length == 0) return;
        // A .bin listed by a .cue that is also chosen is the same disc.
        var picked = new List<string>(dlg.FileNames);
        picked.RemoveAll(f => f.EndsWith(".bin", StringComparison.OrdinalIgnoreCase) &&
                              picked.Exists(c => c.EndsWith(".cue", StringComparison.OrdinalIgnoreCase) &&
                                                 Path.ChangeExtension(c, null) == Path.ChangeExtension(f, null)));
        // Discs of one game are converted together, once, from disc 1.
        var jobs = new List<(string first, List<string> extras, bool isSet)>();
        foreach (string f in picked)
        {
            var set = DiscSet(f);
            if (!jobs.Exists(j => string.Equals(j.first, set.first, StringComparison.OrdinalIgnoreCase))) jobs.Add(set);
        }
        jobs.Sort((a, b) => StringComparer.OrdinalIgnoreCase.Compare(Path.GetFileName(a.first), Path.GetFileName(b.first)));

        string games = GamesFolder();
        var results = new List<string>();
        int ok = 0, failed = 0, skipped = 0;
        LogBox.Clear();
        SetBusy(true);
        _batchRunning = true; _batchStop = false;
        BatchButton.IsEnabled = true; BatchButton.Content = "Stop after this game";
        try
        {
            for (int i = 0; i < jobs.Count; ++i)
            {
                if (_batchStop) { results.Add("Stopped before: " + Path.GetFileName(jobs[i].first)); break; }
                string disc = jobs[i].first, title = GameFolderName(disc, !jobs[i].isSet), outDir = Path.Combine(games, title);
                string discs = jobs[i].extras.Count > 0 ? $", {1 + jobs[i].extras.Count} discs" : "";
                AppendLog(0, $"===== [{i + 1}/{jobs.Count}] {title}  ({Path.GetFileName(disc)}{discs})");
                StageText.Text = _batchLabel = $"Batch {i + 1} of {jobs.Count}: {title}";
                if (Directory.Exists(outDir) && Directory.EnumerateFiles(outDir, "*.exe").Any())
                {
                    AppendLog(0, $"Already converted in {outDir}: skipped (delete that folder to convert it again).");
                    results.Add($"Skipped   {title} (already converted)"); ++skipped; continue;
                }
                DiscBox.Text = disc; OutBox.Text = _suggestedOut = outDir;
                var req = new ConversionRequest(disc, outDir, null, null, null, BuildCheck.IsChecked == true,
                                                ScanCheck.IsChecked == true, ReleaseCheck.IsChecked == true)
                { ExtraDiscs = jobs[i].extras };
                int rc;
                try { rc = await _svc.ConvertAsync(req); }
                catch (Exception ex) { AppendLog(2, ex.Message); rc = -1; }
                if (rc == 0) { results.Add($"OK        {title}"); ++ok; }
                else { results.Add($"FAILED    {title} (code {rc})"); ++failed; }
            }
        }
        finally
        {
            _batchRunning = false;
            _batchLabel = null;
            BatchButton.Content = "Batch…"; BatchButton.IsEnabled = true;
            SetBusy(false);
        }
        string summary = $"Batch finished: {ok} converted, {failed} failed, {skipped} skipped.";
        AppendLog(0, "");
        AppendLog(0, "===== " + summary);
        foreach (var r in results) AppendLog(0, "  " + r);
        try
        {
            File.WriteAllText(Path.Combine(games, "batch-log.txt"), LogBox.Text);
            AppendLog(0, "Full log saved to " + Path.Combine(games, "batch-log.txt"));
        }
        catch { }
        StageText.Text = summary;
        OpenButton.IsEnabled = Directory.Exists(OutBox.Text);
    }

    private void OpenOutput_Click(object sender, RoutedEventArgs e)
    {
        if (Directory.Exists(OutBox.Text))
            Process.Start(new ProcessStartInfo("explorer.exe", $"\"{OutBox.Text}\"") { UseShellExecute = true });
    }

    private void SetBusy(bool busy)
    {
        _busy = busy;
        ConvertButton.IsEnabled = !busy;
        if (!_batchRunning) BatchButton.IsEnabled = !busy;
        Mouse.OverrideCursor = busy ? Cursors.AppStarting : null;
    }
}
