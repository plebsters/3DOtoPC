using System;
using System.Collections.Generic;
using System.Collections.ObjectModel;
using System.ComponentModel;
using System.Diagnostics;
using System.IO;
using System.Linq;
using System.Text.RegularExpressions;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Input;
using System.Windows.Media;
using System.Windows.Media.Imaging;
using Microsoft.Win32;

namespace TdoConverter;

public enum ItemState { Waiting, Converting, Done, Failed, Skipped }

/// One game in the batch queue (disc 1 plus any other discs of the set).
public sealed class BatchItem : INotifyPropertyChanged
{
    private static readonly Brush Good = Frozen(0x3F, 0xB9, 0x50), Bad = Frozen(0xF0, 0x88, 0x3E),
                                  Accent = Frozen(0x5B, 0x9B, 0xFF), Faint = Frozen(0x8B, 0x92, 0x9C),
                                  Active = Frozen(0x1B, 0x22, 0x2D);
    private static Brush Frozen(byte r, byte g, byte b) { var x = new SolidColorBrush(Color.FromRgb(r, g, b)); x.Freeze(); return x; }

    public required string First { get; init; }
    public required List<string> Extras { get; init; }
    public required bool IsSet { get; init; }
    public string Title { get; set; } = "";

    private ItemState _state;
    private string _status = "Waiting";
    private double _progress;
    public ItemState State { get => _state; set { _state = value; Changed(nameof(State), nameof(Icon), nameof(StatusBrush), nameof(RowBrush), nameof(RemoveVisibility)); } }
    public string StatusText { get => _status; set { _status = value; Changed(nameof(StatusText)); } }
    public double Progress { get => _progress; set { _progress = value; Changed(nameof(Progress)); } }
    public bool Locked { get; set; }       // while the batch runs nothing can be removed

    public string Detail
    {
        get
        {
            string d = Path.GetFileName(First);
            if (Extras.Count > 0) d += $" · {1 + Extras.Count} discs";
            return d;
        }
    }
    public string Icon => _state switch
    {
        ItemState.Done => "\uE73E",        // check
        ItemState.Failed => "\uE783",      // error
        ItemState.Converting => "\uE895",  // sync
        ItemState.Skipped => "\uE8D8",     // skip
        _ => "\uE823",                     // clock
    };
    public Brush StatusBrush => _state switch
    {
        ItemState.Done => Good, ItemState.Failed => Bad, ItemState.Converting => Accent, _ => Faint,
    };
    public Brush RowBrush => _state == ItemState.Converting ? Active : Brushes.Transparent;
    public Visibility RemoveVisibility => Locked || _state == ItemState.Converting ? Visibility.Hidden : Visibility.Visible;

    public void Relock(bool locked) { Locked = locked; Changed(nameof(RemoveVisibility)); }

    public event PropertyChangedEventHandler? PropertyChanged;
    private void Changed(params string[] names) { foreach (var n in names) PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(n)); }
}

public partial class MainWindow : Window
{
    private readonly ConversionService _svc = new();
    private bool _busy;
    private bool _compilerFound = true;
    private string? _disc;              // disc 1 of the chosen game
    private string? _suggestedOut;      // the output folder we filled in (replaced when another disc is chosen)
    private string? _suggestedName;     // likewise for the game name
    private string? _userCover;         // a picture the user picked; null lets the converter find one
    private string? _previewCover;      // temp PNG of the cover shown
    private readonly ObservableCollection<BatchItem> _queue = new();
    private bool _batchRunning, _batchPause, _batchStop;
    private BatchItem? _current;        // batch item being converted

    private static readonly SolidColorBrush AccentBrush = (SolidColorBrush)Application.Current.Resources["AccentBrush"];
    private static readonly SolidColorBrush GoodBrush = (SolidColorBrush)Application.Current.Resources["GoodBrush"];
    private static readonly SolidColorBrush BadBrush = (SolidColorBrush)Application.Current.Resources["BadBrush"];
    private static readonly SolidColorBrush IdleBrush = (SolidColorBrush)Application.Current.Resources["StepIdleBrush"];
    private static readonly SolidColorBrush TextBrush = (SolidColorBrush)Application.Current.Resources["TextBrush"];
    private static readonly SolidColorBrush FaintBrush = (SolidColorBrush)Application.Current.Resources["FaintBrush"];

    private static readonly string[] DiscExts = { ".cue", ".iso", ".bin" };

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

    /// A .bin or .iso with the same name as a .cue that is also chosen is the
    /// same disc (the cue sheet points at it): it is kept once, as the .cue.
    private static List<string> DropCueTwins(IEnumerable<string> files)
    {
        var picked = files.Distinct(StringComparer.OrdinalIgnoreCase).ToList();
        picked.RemoveAll(f => (f.EndsWith(".bin", StringComparison.OrdinalIgnoreCase) ||
                               f.EndsWith(".iso", StringComparison.OrdinalIgnoreCase)) &&
                              picked.Exists(c => c.EndsWith(".cue", StringComparison.OrdinalIgnoreCase) &&
                                                 string.Equals(Path.ChangeExtension(c, null), Path.ChangeExtension(f, null),
                                                               StringComparison.OrdinalIgnoreCase)));
        return picked;
    }

    private static bool IsDiscFile(string f) => DiscExts.Contains(Path.GetExtension(f).ToLowerInvariant());

    /// An unpacked disc: a folder holding the disc's files (LaunchMe at the top).
    private static bool IsDiscFolder(string d) => File.Exists(Path.Combine(d, "LaunchMe"));

    public MainWindow()
    {
        InitializeComponent();
        QueueList.ItemsSource = _queue;
        BatchOutBox.Text = GamesFolder();
        _svc.Log += (level, msg) => Dispatcher.InvokeAsync(() => AppendLog(level, msg));
        _svc.Progress += (stage, f) => Dispatcher.InvokeAsync(() => ShowStage(stage, f));
        Loaded += (_, _) =>
        {
            try
            {
                AboutItem.Header = NativeMethods.Version();
                var (found, desc) = NativeMethods.DetectToolchain();
                _compilerFound = found;
                ToolchainDot.Fill = found ? GoodBrush : BadBrush;
                ToolchainText.Text = found ? "Compiler ready · " + ShortCompiler(desc) : "No compiler found · project only";
                ToolchainText.ToolTip = desc;
                if (!found) BuildCheck.IsChecked = false;
            }
            catch (DllNotFoundException)
            {
                ToolchainDot.Fill = BadBrush;
                ToolchainText.Text = "tdocore.dll is missing";
                ToolchainText.ToolTip = "tdocore.dll was not found next to the app; build the native core first.";
                ConvertButton.IsEnabled = StartBatchButton.IsEnabled = InspectButton.IsEnabled = false;
            }
            UpdateAdvancedSummary();
            UpdateQueueSummary();
        };
        Closed += (_, _) => DeletePreview();
    }

    /// "bundled zig (clang 20.1) at C:\...\zig.exe" -> "bundled zig/clang".
    private static string ShortCompiler(string desc)
    {
        string d = desc.ToLowerInvariant();
        if (d.Contains("bundled") || d.Contains("zig")) return "bundled zig/clang";
        if (d.Contains("visual studio") || d.Contains("msvc")) return "Visual Studio";
        if (d.Contains("clang")) return "clang";
        if (d.Contains("gcc") || d.Contains("mingw")) return "MinGW";
        int cut = desc.IndexOfAny(new[] { '(', ',', ';' });
        return (cut > 0 ? desc[..cut] : desc).Trim();
    }

    // ------------------------------------------------------------------ tabs

    private void Tab_Checked(object sender, RoutedEventArgs e)
    {
        if (SingleView is null || BatchView is null) return;    // during InitializeComponent
        bool single = SingleTab.IsChecked == true;
        SingleView.Visibility = single ? Visibility.Visible : Visibility.Collapsed;
        BatchView.Visibility = single ? Visibility.Collapsed : Visibility.Visible;
    }

    private void Settings_Click(object sender, RoutedEventArgs e)
    {
        if (SettingsButton.ContextMenu is { } menu)
        {
            menu.PlacementTarget = SettingsButton;
            menu.Placement = System.Windows.Controls.Primitives.PlacementMode.Bottom;
            menu.IsOpen = true;
        }
    }

    private void OpenGames_Click(object sender, RoutedEventArgs e) => OpenFolder(GamesFolder());

    private void CopyLog_Click(object sender, RoutedEventArgs e)
    {
        try { if (LogBox.Text.Length > 0) Clipboard.SetText(LogBox.Text); } catch { }
    }

    private void ShowLog_Click(object sender, RoutedEventArgs e) => SingleTab.IsChecked = true;

    // ------------------------------------------------------------------ log and progress

    private void AppendLog(int level, string msg)
    {
        string prefix = level switch { 1 => "warning: ", 2 => "error: ", _ => "" };
        LogBox.AppendText(prefix + msg + Environment.NewLine);
        LogBox.ScrollToEnd();
    }

    private static int StepOf(Stage s) => s switch
    {
        Stage.Open or Stage.Extract => 0,
        Stage.Analyze => 1,
        Stage.Emit => 2,
        Stage.Configure or Stage.Build => 3,
        _ => 4,
    };

    private static readonly string[] StepNames = { "Reading the disc", "Analyzing the code", "Generating C++", "Compiling", "Done" };

    private void ShowStage(Stage stage, float f)
    {
        int step = StepOf(stage);
        f = Math.Clamp(f, 0f, 1f);
        double overall = stage == Stage.Done ? 1 : (step + f) / 4.0;
        SetSteps(step, stage == Stage.Done ? 2 : 1);
        Progress.Value = Math.Min(1, overall);
        ProgressTitle.Text = StepNames[step] + (stage == Stage.Done ? "" : "…");
        ProgressDetail.Text = stage == Stage.Done ? "" : $"{(int)Math.Round(f * 100)}%";
        if (_current is not null)
        {
            _current.Progress = Math.Min(1, overall);
            _current.StatusText = stage == Stage.Done ? "Finishing…" : StepNames[step];
            NowStage.Text = ProgressTitle.Text;
            NowProgress.Value = Math.Min(1, overall);
        }
    }

    /// Steps before `step` done, `step` itself active (mode 1), done (2) or failed (3).
    private void SetSteps(int step, int mode)
    {
        var bars = new[] { Step0, Step1, Step2, Step3, Step4 };
        var texts = new[] { StepText0, StepText1, StepText2, StepText3, StepText4 };
        for (int i = 0; i < bars.Length; ++i)
        {
            Brush bar = IdleBrush, text = FaintBrush;
            if (step >= 0 && i < step) { bar = GoodBrush; text = TextBrush; }
            else if (step >= 0 && i == step)
            {
                bar = mode switch { 2 => GoodBrush, 3 => BadBrush, _ => AccentBrush };
                text = TextBrush;
            }
            bars[i].Background = bar;
            texts[i].Foreground = text;
        }
    }

    private void ResetProgress(string title)
    {
        SetSteps(-1, 0);
        Progress.Value = 0;
        Progress.Foreground = AccentBrush;
        ProgressTitle.Text = title;
        ProgressDetail.Text = "";
    }

    private void Finish(int rc)
    {
        int step = 0;
        for (int i = 0; i < 5; ++i) if (((Border)FindName("Step" + i)).Background == AccentBrush) step = i;
        if (rc == 0)
        {
            SetSteps(4, 2);
            Progress.Value = 1;
            Progress.Foreground = GoodBrush;
            ProgressTitle.Text = BuildCheck.IsChecked == true ? "Finished: the game is ready" : "Finished: the project is written";
        }
        else
        {
            SetSteps(step, 3);
            Progress.Foreground = BadBrush;
            ProgressTitle.Text = $"Failed (code {rc}) · see the log";
        }
        ProgressDetail.Text = "";
    }

    // ------------------------------------------------------------------ single game

    private void BrowseDisc_Click(object sender, RoutedEventArgs e)
    {
        var dlg = new OpenFileDialog { Filter = "3DO disc images (*.cue;*.iso;*.bin)|*.cue;*.iso;*.bin|All files|*.*" };
        if (dlg.ShowDialog(this) == true) SetDisc(dlg.FileName);
    }

    private async void SetDisc(string path)
    {
        if (_busy) return;
        var set = Directory.Exists(path) ? (path, new List<string>(), false) : DiscSet(path);
        _disc = set.Item1;
        _userCover = null;
        string title = Directory.Exists(_disc) ? Path.GetFileName(_disc.TrimEnd('\\', '/')) : GameFolderName(_disc, !set.Item3);

        CoverTitle.Text = title;
        string info = Path.GetFileName(_disc.TrimEnd('\\', '/'));
        try { if (File.Exists(_disc)) info += $" · {DiscSize(_disc)}"; } catch { }
        if (set.Item3) info += $" · {1 + set.Item2.Count} discs, converted together";
        CoverInfo.Text = info;
        ChooseDiscButton.Content = "Other disc…";
        ChangeCoverButton.IsEnabled = true;
        ResetCoverButton.Visibility = Visibility.Collapsed;
        if (set.Item3)
            AppendLog(0, $"Multi-disc game: {1 + set.Item2.Count} disc(s) found next to it; they are converted together.");

        // Converted games go to games\<title> in the app's folder, where the
        // launcher finds them. A folder or name the user typed is left alone.
        if (string.IsNullOrWhiteSpace(OutBox.Text) || OutBox.Text == _suggestedOut)
            OutBox.Text = _suggestedOut = Path.Combine(GamesFolder(), title);
        if (string.IsNullOrWhiteSpace(NameBox.Text) || NameBox.Text == _suggestedName)
            NameBox.Text = _suggestedName = title;
        ResetProgress("Ready to convert");
        OpenButton.IsEnabled = Directory.Exists(OutBox.Text);
        await LoadCoverPreview();
    }

    /// Size of the disc: the .bin a cue sheet points at, else the file itself.
    private static string DiscSize(string disc)
    {
        long bytes = new FileInfo(disc).Length;
        if (disc.EndsWith(".cue", StringComparison.OrdinalIgnoreCase))
        {
            foreach (string line in File.ReadLines(disc))
            {
                int q1 = line.IndexOf('"'), q2 = line.LastIndexOf('"');
                if (line.Contains("FILE") && q1 >= 0 && q2 > q1)
                {
                    string bin = Path.Combine(Path.GetDirectoryName(disc) ?? ".", line.Substring(q1 + 1, q2 - q1 - 1));
                    if (File.Exists(bin)) bytes = new FileInfo(bin).Length;
                    break;
                }
            }
        }
        return bytes >= 1 << 30 ? $"{bytes / (double)(1 << 30):0.0} GB" : $"{bytes / (double)(1 << 20):0} MB";
    }

    private void DeletePreview()
    {
        if (_previewCover is null) return;
        try { File.Delete(_previewCover); } catch { }
        _previewCover = null;
    }

    /// Shows the cover the conversion will use: the user's picture, else what
    /// the converter finds (a picture beside the disc, or the disc's title screen).
    private async System.Threading.Tasks.Task LoadCoverPreview()
    {
        string? disc = _disc;
        if (disc is null) return;
        if (_userCover is not null)
        {
            ShowCover(_userCover, "Cover: " + Path.GetFileName(_userCover) + " (your picture)", true);
            return;
        }
        DeletePreview();
        string png = Path.Combine(Path.GetTempPath(), $"3do2pc-cover-{Guid.NewGuid():N}.png");
        (int rc, string source) r;
        try { r = await _svc.FindCoverAsync(disc, png); }
        catch (Exception) { r = (2, ""); }
        if (disc != _disc || _userCover is not null) { try { File.Delete(png); } catch { } return; }   // changed meanwhile
        if (r.rc == 0)
        {
            _previewCover = png;
            string where = r.source == "the disc" ? "title screen from the disc" : r.source + " next to the disc";
            ShowCover(png, "Cover: " + where, true);
        }
        else
        {
            ShowCover(null, "No cover found. Add one with Change cover…, or put cover.jpg next to the disc.", false);
        }
    }

    private void ShowCover(string? file, string source, bool ok)
    {
        CoverImage.Source = null;
        if (file is not null)
        {
            try
            {
                var bmp = new BitmapImage();
                bmp.BeginInit();
                bmp.CacheOption = BitmapCacheOption.OnLoad;       // don't keep the file open
                bmp.CreateOptions = BitmapCreateOptions.IgnoreImageCache;
                bmp.UriSource = new Uri(file);
                bmp.EndInit();
                bmp.Freeze();
                CoverImage.Source = bmp;
            }
            catch (Exception) { ok = false; source = "That picture could not be read."; }
        }
        CoverEmpty.Visibility = CoverImage.Source is null ? Visibility.Visible : Visibility.Collapsed;
        CoverEmptyText.Text = _disc is null ? "Drop a 3DO disc here" : "No cover";
        CoverSourceRow.Visibility = Visibility.Visible;
        CoverSource.Text = source;
        CoverSourceIcon.Text = ok ? "\uE73E" : "\uE946";
        CoverSourceIcon.Foreground = ok ? GoodBrush : FaintBrush;
    }

    private async void ChangeCover_Click(object sender, RoutedEventArgs e)
    {
        var dlg = new OpenFileDialog
        {
            Title = "Choose a cover picture (used for the game's window and exe icon)",
            Filter = "Pictures (*.png;*.jpg;*.jpeg;*.bmp)|*.png;*.jpg;*.jpeg;*.bmp|All files|*.*",
        };
        if (_disc is not null && File.Exists(_disc)) dlg.InitialDirectory = Path.GetDirectoryName(_disc);
        if (dlg.ShowDialog(this) != true) return;
        _userCover = dlg.FileName;
        ResetCoverButton.Visibility = Visibility.Visible;
        await LoadCoverPreview();
    }

    private async void ResetCover_Click(object sender, RoutedEventArgs e)
    {
        _userCover = null;
        ResetCoverButton.Visibility = Visibility.Collapsed;
        await LoadCoverPreview();
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

    private void AdvancedToggle_Click(object sender, RoutedEventArgs e)
    {
        bool show = AdvancedPanel.Visibility != Visibility.Visible;
        AdvancedPanel.Visibility = show ? Visibility.Visible : Visibility.Collapsed;
        AdvancedToggle.Content = show ? "Hide" : "Show";
    }

    private void BatchAdvanced_Click(object sender, RoutedEventArgs e)
    {
        bool show = BatchAdvancedPanel.Visibility != Visibility.Visible;
        BatchAdvancedPanel.Visibility = show ? Visibility.Visible : Visibility.Collapsed;
        BatchAdvancedLink.Content = show ? "Hide advanced options" : "Advanced options…";
    }

    private void Options_Changed(object sender, RoutedEventArgs e) => UpdateAdvancedSummary();

    private void UpdateAdvancedSummary()
    {
        if (AdvancedSummary is null || BuildCheck is null || ScanCheck is null || ReleaseCheck is null) return;
        var parts = new List<string>();
        parts.Add(BuildCheck.IsChecked == true ? "build .exe" : "project only (no .exe)");
        if (ScanCheck.IsChecked == true) parts.Add("extra function discovery");
        if (BuildCheck.IsChecked == true) parts.Add(ReleaseCheck.IsChecked == true ? "release build" : "debug build");
        AdvancedSummary.Text = string.Join(" · ", parts);
        if (ConvertButton is not null) ConvertButton.Content = BuildCheck.IsChecked == true ? "Convert" : "Write the project";
    }

    private async void Inspect_Click(object sender, RoutedEventArgs e)
    {
        if (_busy || _disc is null || !(File.Exists(_disc) || Directory.Exists(_disc))) return;
        LogBox.Clear();
        SetBusy(true);
        ResetProgress("Listing the disc's files…");
        try
        {
            int rc = await _svc.InspectAsync(_disc);
            ResetProgress(rc == 0 ? "Disc listed in the log" : $"Could not read the disc (code {rc})");
        }
        finally { SetBusy(false); }
    }

    private async void Convert_Click(object sender, RoutedEventArgs e)
    {
        if (_busy) return;
        if (_disc is null || !(File.Exists(_disc) || Directory.Exists(_disc)))
        {
            MessageBox.Show(this, "Choose a disc image first.", "3DO to PC"); return;
        }
        if (string.IsNullOrWhiteSpace(OutBox.Text)) { MessageBox.Show(this, "Choose where to save the game.", "3DO to PC"); return; }

        LogBox.Clear();
        SetBusy(true);
        ResetProgress("Starting…");
        var set = Directory.Exists(_disc) ? (_disc, new List<string>(), false) : DiscSet(_disc);
        var req = new ConversionRequest(_disc, OutBox.Text, NameBox.Text, HintsBox.Text, null,
                                        BuildCheck.IsChecked == true, ScanCheck.IsChecked == true,
                                        ReleaseCheck.IsChecked == true)
        {
            ExtraDiscs = set.Item3 && set.Item1 == _disc ? set.Item2 : new List<string>(),
            CoverPath = _userCover,
        };
        try
        {
            int rc;
            try { rc = await _svc.ConvertAsync(req); }
            catch (Exception ex) { AppendLog(2, ex.Message); rc = -1; }
            Finish(rc);
            OpenButton.IsEnabled = Directory.Exists(OutBox.Text);
        }
        finally { SetBusy(false); }
    }

    private void OpenOutput_Click(object sender, RoutedEventArgs e)
    {
        string dir = BatchView.Visibility == Visibility.Visible || _current is not null ? BatchOutBox.Text : OutBox.Text;
        if (SingleView.Visibility == Visibility.Visible && Directory.Exists(OutBox.Text)) dir = OutBox.Text;
        OpenFolder(dir);
    }

    private static void OpenFolder(string dir)
    {
        if (Directory.Exists(dir))
            Process.Start(new ProcessStartInfo("explorer.exe", $"\"{dir}\"") { UseShellExecute = true });
    }

    // ------------------------------------------------------------------ drag and drop

    private void Window_DragOver(object sender, DragEventArgs e)
    {
        e.Effects = e.Data.GetDataPresent(DataFormats.FileDrop) && !_busy ? DragDropEffects.Copy : DragDropEffects.None;
        e.Handled = true;
    }

    private void Window_Drop(object sender, DragEventArgs e)
    {
        if (_busy && !_batchRunning) return;
        if (e.Data.GetData(DataFormats.FileDrop) is not string[] paths || paths.Length == 0) return;
        if (BatchView.Visibility == Visibility.Visible)
        {
            AddToQueue(paths);
            return;
        }
        if (_busy) return;
        // Single game: the first disc dropped (a picture dropped sets the cover).
        string p = paths[0];
        string ext = Path.GetExtension(p).ToLowerInvariant();
        if (_disc is not null && (ext is ".png" or ".jpg" or ".jpeg" or ".bmp"))
        {
            _userCover = p;
            ResetCoverButton.Visibility = Visibility.Visible;
            _ = LoadCoverPreview();
            return;
        }
        if (paths.Length > 1 && paths.Count(f => IsDiscFile(f) || Directory.Exists(f)) > 1)
        {
            // Several games dropped: they go to the batch queue.
            BatchTab.IsChecked = true;
            AddToQueue(paths);
            return;
        }
        if (IsDiscFile(p) || (Directory.Exists(p) && IsDiscFolder(p))) SetDisc(p);
        else if (Directory.Exists(p)) { BatchTab.IsChecked = true; AddToQueue(paths); }
    }

    // ------------------------------------------------------------------ batch

    private void AddDiscs_Click(object sender, RoutedEventArgs e)
    {
        var dlg = new OpenFileDialog
        {
            Title = "Choose the disc images to convert",
            Filter = "3DO disc images (*.cue;*.iso;*.bin)|*.cue;*.iso;*.bin|All files|*.*",
            Multiselect = true,
        };
        if (dlg.ShowDialog(this) == true) AddToQueue(dlg.FileNames);
    }

    private void AddFolder_Click(object sender, RoutedEventArgs e)
    {
        var dlg = new OpenFolderDialog { Title = "Choose a folder of disc images (subfolders are searched too)" };
        if (dlg.ShowDialog(this) == true) AddToQueue(new[] { dlg.FolderName });
    }

    /// Adds discs (files) and the discs in folders (searched to any depth) to
    /// the queue. Discs of one game become one item, from disc 1; a game
    /// already in the queue is not added twice.
    private void AddToQueue(IEnumerable<string> paths)
    {
        var files = new List<string>();
        foreach (string p in paths)
        {
            if (Directory.Exists(p))
            {
                if (IsDiscFolder(p)) { files.Add(p); continue; }
                try
                {
                    files.AddRange(Directory.EnumerateFiles(p, "*.*", SearchOption.AllDirectories).Where(IsDiscFile));
                }
                catch (Exception ex) { AppendLog(1, $"Could not search {p}: {ex.Message}"); }
            }
            else if (File.Exists(p) && IsDiscFile(p)) files.Add(p);
        }
        int added = 0;
        foreach (string f in DropCueTwins(files))
        {
            var set = Directory.Exists(f) ? (f, new List<string>(), false) : DiscSet(f);
            if (_queue.Any(q => string.Equals(q.First, set.Item1, StringComparison.OrdinalIgnoreCase))) continue;
            // An .iso/.bin whose .cue twin is already queued is the same disc.
            if (_queue.Any(q => q.First.EndsWith(".cue", StringComparison.OrdinalIgnoreCase) &&
                                string.Equals(Path.ChangeExtension(q.First, null), Path.ChangeExtension(set.Item1, null),
                                              StringComparison.OrdinalIgnoreCase))) continue;
            _queue.Add(new BatchItem { First = set.Item1, Extras = set.Item2, IsSet = set.Item3, Locked = _batchRunning });
            ++added;
        }
        if (files.Count > 0 && added == 0) AppendLog(0, "Those discs are already in the queue.");
        if (files.Count == 0) AppendLog(1, "No disc images (.cue, .iso, .bin) were found there.");
        SortWaiting();
        AssignTitles();
        UpdateQueueSummary();
    }

    /// Waiting items in name order (items already converted stay where they are).
    private void SortWaiting()
    {
        var waiting = _queue.Where(q => q.State == ItemState.Waiting).ToList();
        var sorted = waiting.OrderBy(q => Path.GetFileName(q.First), StringComparer.OrdinalIgnoreCase).ToList();
        int start = _queue.Count - waiting.Count;
        if (waiting.SequenceEqual(sorted) || _queue.Take(start).Any(q => q.State == ItemState.Waiting)) return;
        for (int i = 0; i < sorted.Count; ++i)
        {
            int from = _queue.IndexOf(sorted[i]);
            if (from != start + i) _queue.Move(from, start + i);
        }
    }

    /// Folder (and exe) names: the title before the first "(" or "[". Different
    /// discs with the same title (a sampler's JP and US releases, say) keep
    /// their whole file name instead, so each gets its own folder rather than
    /// all but the first being skipped as "already converted".
    private void AssignTitles()
    {
        var titles = _queue.Select(q => Directory.Exists(q.First) ? Path.GetFileName(q.First.TrimEnd('\\', '/'))
                                                                  : GameFolderName(q.First, !q.IsSet)).ToList();
        for (int i = 0; i < _queue.Count; ++i)
        {
            if (_queue[i].State != ItemState.Waiting && _queue[i].Title.Length > 0) continue;
            string t = titles[i];
            if (titles.Count(x => string.Equals(x, t, StringComparison.OrdinalIgnoreCase)) > 1)
            {
                t = Path.GetFileNameWithoutExtension(_queue[i].First).Trim();
                foreach (char c in Path.GetInvalidFileNameChars()) t = t.Replace(c, '_');
            }
            _queue[i].Title = t;
        }
        QueueList.Items.Refresh();
    }

    private void RemoveItem_Click(object sender, RoutedEventArgs e)
    {
        if (sender is FrameworkElement { Tag: BatchItem item } && !item.Locked && item.State != ItemState.Converting)
        {
            _queue.Remove(item);
            AssignTitles();
            UpdateQueueSummary();
        }
    }

    private void UpdateQueueSummary()
    {
        int n = _queue.Count;
        int done = _queue.Count(q => q.State == ItemState.Done);
        int failed = _queue.Count(q => q.State == ItemState.Failed);
        int skipped = _queue.Count(q => q.State == ItemState.Skipped);
        int waiting = _queue.Count(q => q.State == ItemState.Waiting);
        if (n == 0) QueueSummary.Text = "Empty · add discs or a folder";
        else
        {
            string s = $"{done} of {n} done";
            if (failed > 0) s += $" · {failed} failed";
            if (skipped > 0) s += $" · {skipped} skipped";
            if (waiting > 0 && !_batchRunning && done + failed + skipped > 0) s += $" · {waiting} waiting";
            QueueSummary.Text = s;
        }
        if (!_batchRunning)
        {
            StartBatchButton.Content = done + failed + skipped > 0 && waiting > 0 ? "Continue" : "Start";
            StartBatchButton.IsEnabled = waiting > 0 && !_busy;
        }
    }

    private void BrowseBatchOut_Click(object sender, RoutedEventArgs e)
    {
        var dlg = new OpenFolderDialog { InitialDirectory = Directory.Exists(BatchOutBox.Text) ? BatchOutBox.Text : GamesFolder() };
        if (dlg.ShowDialog(this) == true) BatchOutBox.Text = dlg.FolderName;
    }

    private void PauseBatch_Click(object sender, RoutedEventArgs e)
    {
        if (!_batchRunning) return;
        _batchPause = true;
        PauseButton.IsEnabled = false; PauseButton.Content = "Pausing…";
        NowStage.Text = "Pausing after this game…";
    }

    private void StopBatch_Click(object sender, RoutedEventArgs e)
    {
        if (!_batchRunning) return;
        _batchStop = true;
        PauseButton.IsEnabled = StopButton.IsEnabled = false; StopButton.Content = "Stopping…";
        NowStage.Text = "Stopping after this game…";
    }

    // Converts the waiting games one after another into <folder>\<title>\ with
    // the advanced options of the Single game page. A game whose folder already
    // holds an .exe is skipped when that box is ticked. Pause and Stop take
    // effect after the current game: a conversion in progress is not cut off.
    private async void StartBatch_Click(object sender, RoutedEventArgs e)
    {
        if (_busy || _batchRunning) return;
        var jobs = _queue.Where(q => q.State == ItemState.Waiting).ToList();
        if (jobs.Count == 0) return;
        string games = string.IsNullOrWhiteSpace(BatchOutBox.Text) ? GamesFolder() : BatchOutBox.Text.Trim();
        try { Directory.CreateDirectory(games); }
        catch (Exception ex) { MessageBox.Show(this, $"Cannot use {games}: {ex.Message}", "3DO to PC"); return; }

        bool build = BuildCheck.IsChecked == true, scan = ScanCheck.IsChecked == true, release = ReleaseCheck.IsChecked == true;
        bool skipConverted = SkipCheck.IsChecked == true, keepGoing = KeepGoingCheck.IsChecked == true;
        var results = new List<string>();
        int ok = 0, failed = 0, skipped = 0;
        LogBox.Clear();
        SetBusy(true);
        _batchRunning = true; _batchPause = _batchStop = false;
        foreach (var q in _queue) q.Relock(true);
        StartBatchButton.IsEnabled = false; StartBatchButton.Content = "Converting…";
        PauseButton.IsEnabled = StopButton.IsEnabled = true;
        string end = "";
        try
        {
            for (int i = 0; i < jobs.Count; ++i)
            {
                var item = jobs[i];
                if (_batchStop)
                {
                    for (int k = i; k < jobs.Count; ++k)
                    {
                        jobs[k].State = ItemState.Skipped; jobs[k].StatusText = "Stopped";
                        results.Add("Stopped   " + jobs[k].Title);
                    }
                    end = " Stopped.";
                    break;
                }
                if (_batchPause) { end = $" Paused; {jobs.Count - i} game(s) still waiting."; break; }

                string disc = item.First, title = item.Title, outDir = Path.Combine(games, title);
                string discs = item.Extras.Count > 0 ? $", {1 + item.Extras.Count} discs" : "";
                AppendLog(0, $"===== [{i + 1}/{jobs.Count}] {title}  ({Path.GetFileName(disc)}{discs})");
                if (skipConverted && Directory.Exists(outDir) && Directory.EnumerateFiles(outDir, "*.exe").Any())
                {
                    AppendLog(0, $"Already converted in {outDir}: skipped (delete that folder to convert it again).");
                    item.State = ItemState.Skipped; item.StatusText = "Already converted"; item.Progress = 1;
                    results.Add($"Skipped   {title} (already converted)"); ++skipped;
                    UpdateQueueSummary();
                    continue;
                }
                _current = item;
                item.State = ItemState.Converting; item.StatusText = "Starting…"; item.Progress = 0;
                NowTitle.Text = $"{title}  ({i + 1} of {jobs.Count})";
                NowStage.Text = "Starting…"; NowProgress.Value = 0;
                ResetProgress(title);
                OutBox.Text = _suggestedOut = outDir;
                // The game's title names the exe and window (the disc's volume
                // label is "CD-ROM" on most discs).
                var req = new ConversionRequest(disc, outDir, title, null, null, build, scan, release) { ExtraDiscs = item.Extras };
                int rc;
                try { rc = await _svc.ConvertAsync(req); }
                catch (Exception ex) { AppendLog(2, ex.Message); rc = -1; }
                _current = null;
                Finish(rc);
                if (rc == 0)
                {
                    item.State = ItemState.Done; item.StatusText = build ? "Done" : "Project written"; item.Progress = 1;
                    results.Add($"OK        {title}"); ++ok;
                }
                else
                {
                    item.State = ItemState.Failed; item.StatusText = $"Failed (code {rc})";
                    results.Add($"FAILED    {title} (code {rc})"); ++failed;
                    if (!keepGoing && i + 1 < jobs.Count)
                    {
                        end = $" Stopped at the failure; {jobs.Count - i - 1} game(s) still waiting.";
                        break;
                    }
                }
                UpdateQueueSummary();
            }
        }
        finally
        {
            _current = null;
            _batchRunning = false;
            foreach (var q in _queue) q.Relock(false);
            PauseButton.Content = "Pause"; StopButton.Content = "Stop";
            PauseButton.IsEnabled = StopButton.IsEnabled = false;
            SetBusy(false);
            UpdateQueueSummary();
        }
        string summary = $"Batch finished: {ok} converted, {failed} failed, {skipped} skipped.{end}";
        AppendLog(0, "");
        AppendLog(0, "===== " + summary);
        foreach (var r in results) AppendLog(0, "  " + r);
        try
        {
            File.WriteAllText(Path.Combine(games, "batch-log.txt"), LogBox.Text);
            AppendLog(0, "Full log saved to " + Path.Combine(games, "batch-log.txt"));
        }
        catch { }
        NowTitle.Text = failed > 0 ? $"{ok} converted · {failed} failed" : $"{ok} converted";
        NowStage.Text = summary;
        NowProgress.Value = 1;
        OpenButton.IsEnabled = Directory.Exists(OutBox.Text);
    }

    private void SetBusy(bool busy)
    {
        _busy = busy;
        ConvertButton.IsEnabled = InspectButton.IsEnabled = !busy;
        ChooseDiscButton.IsEnabled = !busy;
        ChangeCoverButton.IsEnabled = !busy && _disc is not null;
        AddDiscsButton.IsEnabled = AddFolderButton.IsEnabled = !busy || _batchRunning;
        if (!_batchRunning) StartBatchButton.IsEnabled = !busy && _queue.Any(q => q.State == ItemState.Waiting);
        Mouse.OverrideCursor = busy ? Cursors.AppStarting : null;
    }
}
