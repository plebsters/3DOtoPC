using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Linq;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Input;
using System.Windows.Threading;
using Microsoft.Win32;

namespace TdoLauncher;

public partial class MainWindow : Window
{
    readonly LauncherSettings _settings = LauncherSettings.Load();
    List<Game> _all = new();
    string _folder = "";
    bool _gameRunning;

    // Controller polling: edge-triggered with key-repeat while held.
    readonly DispatcherTimer _padTimer = new() { Interval = TimeSpan.FromMilliseconds(50) };
    PadButtons _padPrev;
    DateTime _padRepeatAt;

    public MainWindow()
    {
        InitializeComponent();
        _folder = _settings.GamesFolder is { Length: > 0 } f && Directory.Exists(f) ? f : GameLibrary.DefaultGamesFolder();
        SortBox.SelectedItem = SortBox.Items.Cast<ComboBoxItem>().FirstOrDefault(i => (string)i.Tag == _settings.Sort)
                               ?? SortBox.Items[0];
        Loaded += (_, _) => { Rescan(); GameList.Focus(); };
        Closed += (_, _) => _settings.Save();
        _padTimer.Tick += PadTimer_Tick;
        _padTimer.Start();
    }

    // ---- library -----------------------------------------------------------

    void Rescan(string? keepFolder = null)
    {
        string? selected = keepFolder ?? (GameList.SelectedItem as Game)?.Folder;
        _all = GameLibrary.Scan(_folder, _settings);
        ApplyView(selected);
    }

    void ApplyView(string? selectFolder = null)
    {
        selectFolder ??= (GameList.SelectedItem as Game)?.Folder;
        string q = SearchBox.Text.Trim();
        IEnumerable<Game> view = _all;
        if (q.Length > 0) view = view.Where(g => g.Name.Contains(q, StringComparison.OrdinalIgnoreCase));
        view = _settings.Sort switch
        {
            "Recent" => view.OrderByDescending(g => g.LastPlayed ?? DateTime.MinValue).ThenBy(g => g.Name),
            "Most" => view.OrderByDescending(g => g.PlayCount).ThenBy(g => g.Name),
            _ => view.OrderBy(g => g.Name, StringComparer.CurrentCultureIgnoreCase),
        };
        var list = view.ToList();
        GameList.ItemsSource = list;
        GameList.SelectedItem = list.FirstOrDefault(g => g.Folder == selectFolder) ?? list.FirstOrDefault();
        if (GameList.SelectedItem != null) GameList.ScrollIntoView(GameList.SelectedItem);

        CountText.Text = _all.Count == 1 ? $"1 game · {_folder}" : $"{_all.Count} games · {_folder}";
        bool empty = _all.Count == 0;
        EmptyPanel.Visibility = empty ? Visibility.Visible : Visibility.Collapsed;
        GameList.Visibility = empty ? Visibility.Collapsed : Visibility.Visible;
        EmptyText.Text = $"Nothing to launch in {_folder}. Convert a game with 3DO to PC, or point the launcher at the folder that holds your converted games (each game is a folder with its .exe and a data folder).";
        UpdateFooter();
    }

    void UpdateFooter()
    {
        if (GameList.SelectedItem is Game g)
        {
            SelName.Text = g.Name;
            SelInfo.Text = g.PlayCount > 0 ? $"{g.Subtitle} · {g.PlayCount}× · {g.Folder}" : g.Folder;
            PlayButton.IsEnabled = FolderButton.IsEnabled = !_gameRunning;
        }
        else
        {
            SelName.Text = ""; SelInfo.Text = "";
            PlayButton.IsEnabled = FolderButton.IsEnabled = false;
        }
        if (_gameRunning) PlayButton.Content = "Running…";
        else PlayButton.Content = "▶  Play";
    }

    // ---- launching ---------------------------------------------------------

    void Launch(Game? g)
    {
        if (g == null || _gameRunning) return;
        if (!File.Exists(g.ExePath)) { MessageBox.Show(this, $"The game's program is missing:\n{g.ExePath}", "3DO Launcher"); Rescan(); return; }

        Process? p;
        try
        {
            // Games read their data\ folder relative to the working directory.
            p = Process.Start(new ProcessStartInfo(g.ExePath) { WorkingDirectory = g.Folder, UseShellExecute = false });
        }
        catch (Exception ex)
        {
            MessageBox.Show(this, $"Could not start {g.Name}:\n{ex.Message}", "3DO Launcher");
            return;
        }
        if (p == null) return;

        var st = _settings.Stats.TryGetValue(g.Folder, out var s) ? s : (_settings.Stats[g.Folder] = new GameStats());
        st.LastPlayed = DateTime.Now; st.PlayCount++;
        g.LastPlayed = st.LastPlayed; g.PlayCount = st.PlayCount;
        _settings.Save();

        _gameRunning = true;
        UpdateFooter();
        WindowState = WindowState.Minimized;
        p.EnableRaisingEvents = true;
        p.Exited += (_, _) => Dispatcher.Invoke(() =>
        {
            _gameRunning = false;
            _padPrev = Gamepad.Read();   // ignore buttons still held from the game
            WindowState = WindowState.Normal;
            Activate();
            Rescan(g.Folder);
            GameList.Focus();
        });
    }

    // ---- events ------------------------------------------------------------

    void Play_Click(object sender, RoutedEventArgs e) => Launch(GameList.SelectedItem as Game);
    void MenuPlay_Click(object sender, RoutedEventArgs e) => Launch(MenuGame(sender));

    void GameList_MouseDoubleClick(object sender, MouseButtonEventArgs e)
    {
        if (e.OriginalSource is DependencyObject d && ItemsControl.ContainerFromElement(GameList, d) is ListBoxItem)
            Launch(GameList.SelectedItem as Game);
    }

    void GameList_KeyDown(object sender, KeyEventArgs e)
    {
        if (e.Key == Key.Enter) { Launch(GameList.SelectedItem as Game); e.Handled = true; }
    }

    void GameList_SelectionChanged(object sender, SelectionChangedEventArgs e) => UpdateFooter();
    void SearchBox_TextChanged(object sender, TextChangedEventArgs e) => ApplyView();

    void SortBox_SelectionChanged(object sender, SelectionChangedEventArgs e)
    {
        if (SortBox.SelectedItem is ComboBoxItem item) { _settings.Sort = (string)item.Tag; _settings.Save(); }
        if (IsLoaded) ApplyView();
    }

    void Refresh_Click(object sender, RoutedEventArgs e) => Rescan();
    void Refresh_Executed(object sender, ExecutedRoutedEventArgs e) => Rescan();
    void Search_Executed(object sender, ExecutedRoutedEventArgs e) { SearchBox.Focus(); SearchBox.SelectAll(); }

    void ChooseFolder_Click(object sender, RoutedEventArgs e)
    {
        var dlg = new OpenFolderDialog { Title = "Choose the folder that holds your converted games" };
        if (Directory.Exists(_folder)) dlg.InitialDirectory = _folder;
        if (dlg.ShowDialog(this) != true) return;
        _folder = dlg.FolderName;
        _settings.GamesFolder = _folder;
        _settings.Save();
        Rescan();
    }

    Game? MenuGame(object sender) =>
        (sender as FrameworkElement)?.DataContext as Game ?? GameList.SelectedItem as Game;

    void MenuOpenFolder_Click(object sender, RoutedEventArgs e)
    {
        if (MenuGame(sender) is { } g)
            Process.Start(new ProcessStartInfo("explorer.exe", $"\"{g.Folder}\"") { UseShellExecute = true });
    }

    void MenuSetCover_Click(object sender, RoutedEventArgs e)
    {
        if (MenuGame(sender) is not { } g) return;
        var dlg = new OpenFileDialog { Title = $"Cover image for {g.Name}", Filter = "Images|*.png;*.jpg;*.jpeg;*.bmp;*.gif" };
        if (dlg.ShowDialog(this) != true) return;
        try
        {
            if (GameLibrary.FindCoverFile(g.Folder) is { } old) File.Delete(old);
            File.Copy(dlg.FileName, Path.Combine(g.Folder, "cover" + Path.GetExtension(dlg.FileName).ToLowerInvariant()), true);
        }
        catch (Exception ex) { MessageBox.Show(this, $"Could not save the cover:\n{ex.Message}", "3DO Launcher"); }
        Rescan(g.Folder);
    }

    void MenuRemoveCover_Click(object sender, RoutedEventArgs e)
    {
        if (MenuGame(sender) is not { } g) return;
        try { if (GameLibrary.FindCoverFile(g.Folder) is { } old) File.Delete(old); }
        catch (Exception ex) { MessageBox.Show(this, $"Could not remove the cover:\n{ex.Message}", "3DO Launcher"); }
        Rescan(g.Folder);
    }

    // ---- box art ---------------------------------------------------------------

    async void GetBoxArt_Click(object sender, RoutedEventArgs e)
    {
        var missing = _all.Where(g => GameLibrary.FindCoverFile(g.Folder) == null).ToList();
        if (missing.Count == 0) { StatusText.Text = "Every game already has a cover. Right-click a game to change its cover."; return; }

        BoxArtButton.IsEnabled = false;
        int added = 0;
        var notFound = new List<string>();
        try
        {
            StatusText.Text = "Getting the box art list…";
            List<string>? index = null;
            try { index = await BoxArt.GetIndexAsync(); }
            catch { /* list refused: fall back to guessing file names */ }
            for (int i = 0; i < missing.Count; ++i)
            {
                var g = missing[i];
                StatusText.Text = $"Box art {i + 1}/{missing.Count}: {g.Name}…";
                if (index == null)
                {
                    if (await BoxArt.GuessAndSaveAsync(g.Name, g.Folder) != null) added++; else notFound.Add(g.Name);
                    continue;
                }
                var m = BoxArt.Best(g.Name, index);
                if (m == null) { notFound.Add(g.Name); continue; }
                try { await BoxArt.SaveAsync(m, g.Folder); added++; }
                catch { notFound.Add(g.Name); }
            }
            StatusText.Text = notFound.Count == 0
                ? $"Added {added} cover(s)."
                : $"Added {added} cover(s). Not found: {string.Join(", ", notFound)}. Right-click a game → Choose box art online… to pick one.";
        }
        catch (Exception ex)
        {
            StatusText.Text = $"Couldn't get box art: {ex.Message}";
        }
        finally
        {
            BoxArtButton.IsEnabled = true;
            Rescan();
        }
    }

    async void MenuChooseBoxArt_Click(object sender, RoutedEventArgs e)
    {
        if (MenuGame(sender) is not { } g) return;
        List<string> index;
        try
        {
            StatusText.Text = "Getting the box art list…";
            index = await BoxArt.GetIndexAsync();
            StatusText.Text = "";
        }
        catch (Exception ex) { StatusText.Text = $"Couldn't get box art: {ex.Message}"; return; }

        var picker = new BoxArtPicker(g.Name, index) { Owner = this };
        if (picker.ShowDialog() != true || picker.Chosen is not { } m) return;
        try
        {
            StatusText.Text = $"Downloading the cover for {g.Name}…";
            await BoxArt.SaveAsync(m, g.Folder);
            StatusText.Text = $"Cover set for {g.Name}.";
        }
        catch (Exception ex) { StatusText.Text = $"Couldn't download the cover: {ex.Message}"; }
        Rescan(g.Folder);
    }

    // ---- controller ----------------------------------------------------------

    void PadTimer_Tick(object? sender, EventArgs e)
    {
        // Only while the launcher is in front and no game owns the controller.
        if (_gameRunning || !IsActive) { _padPrev = Gamepad.Read(); return; }
        var now = Gamepad.Read();
        var pressed = now & ~_padPrev;
        const PadButtons dirs = PadButtons.Up | PadButtons.Down | PadButtons.Left | PadButtons.Right;
        var held = now & dirs;
        if ((pressed & dirs) != 0) _padRepeatAt = DateTime.Now.AddMilliseconds(400);
        else if (held != 0 && DateTime.Now >= _padRepeatAt) { pressed |= held; _padRepeatAt = DateTime.Now.AddMilliseconds(120); }
        _padPrev = now;

        if (pressed.HasFlag(PadButtons.Up)) MoveSelection(FocusNavigationDirection.Up);
        else if (pressed.HasFlag(PadButtons.Down)) MoveSelection(FocusNavigationDirection.Down);
        else if (pressed.HasFlag(PadButtons.Left)) MoveSelection(FocusNavigationDirection.Left);
        else if (pressed.HasFlag(PadButtons.Right)) MoveSelection(FocusNavigationDirection.Right);

        if (pressed.HasFlag(PadButtons.A) || pressed.HasFlag(PadButtons.Start)) Launch(GameList.SelectedItem as Game);
        if (pressed.HasFlag(PadButtons.Y)) Rescan();
        if (pressed.HasFlag(PadButtons.B) && SearchBox.Text.Length > 0) SearchBox.Text = "";
    }

    void MoveSelection(FocusNavigationDirection dir)
    {
        if (GameList.Items.Count == 0) return;
        if (GameList.SelectedItem == null) { GameList.SelectedIndex = 0; return; }
        // Let WPF find the tile in that direction in the wrapped grid.
        if (GameList.ItemContainerGenerator.ContainerFromItem(GameList.SelectedItem) is not ListBoxItem item) return;
        item.Focus();
        if (item.MoveFocus(new TraversalRequest(dir)) &&
            Keyboard.FocusedElement is ListBoxItem next && next.DataContext is Game g)
        {
            GameList.SelectedItem = g;
            GameList.ScrollIntoView(g);
        }
        else item.Focus();
    }
}
