using System;
using System.Diagnostics;
using System.IO;
using System.Windows;
using System.Windows.Input;
using Microsoft.Win32;

namespace TdoConverter;

public partial class MainWindow : Window
{
    private readonly ConversionService _svc = new();
    private bool _busy;

    public MainWindow()
    {
        InitializeComponent();
        _svc.Log += (level, msg) => Dispatcher.InvokeAsync(() => AppendLog(level, msg));
        _svc.Progress += (stage, f) => Dispatcher.InvokeAsync(() =>
        {
            StageText.Text = stage.ToString();
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
        var dlg = new OpenFileDialog { Filter = "3DO disc images (*.iso;*.bin)|*.iso;*.bin|All files|*.*" };
        if (dlg.ShowDialog(this) != true) return;
        DiscBox.Text = dlg.FileName;
        if (string.IsNullOrWhiteSpace(OutBox.Text))
            OutBox.Text = Path.Combine(Path.GetDirectoryName(dlg.FileName)!, Path.GetFileNameWithoutExtension(dlg.FileName) + "_pc");
    }

    private void BrowseOut_Click(object sender, RoutedEventArgs e)
    {
        var dlg = new OpenFolderDialog();
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
        var req = new ConversionRequest(DiscBox.Text, OutBox.Text, NameBox.Text, HintsBox.Text, null,
                                        BuildCheck.IsChecked == true, ScanCheck.IsChecked == true,
                                        ReleaseCheck.IsChecked == true);
        try
        {
            int rc = await _svc.ConvertAsync(req);
            StageText.Text = rc == 0 ? "Finished" : $"Failed (code {rc})";
            OpenButton.IsEnabled = Directory.Exists(OutBox.Text);
        }
        finally { SetBusy(false); }
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
        Mouse.OverrideCursor = busy ? Cursors.AppStarting : null;
    }
}
