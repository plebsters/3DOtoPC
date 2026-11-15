// BoxArtPicker.cs - a small window for choosing a game's cover from the
// online collection: a search box, the matching covers, and a preview.
using System;
using System.Collections.Generic;
using System.Linq;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Input;
using System.Windows.Media;
using System.Windows.Media.Imaging;

namespace TdoLauncher;

public sealed class BoxArtPicker : Window
{
    readonly List<string> _index;
    readonly TextBox _search = new() { Padding = new Thickness(8, 6, 8, 6), FontSize = 13 };
    readonly ListBox _list = new() { BorderThickness = new Thickness(0) };
    readonly Image _preview = new() { Stretch = Stretch.Uniform };
    readonly TextBlock _previewNote = new() { Foreground = Brushes.Gray, HorizontalAlignment = HorizontalAlignment.Center, VerticalAlignment = VerticalAlignment.Center };
    readonly Button _ok = new() { Content = "Use this cover", IsDefault = true, Padding = new Thickness(16, 6, 16, 6), Margin = new Thickness(0, 0, 8, 0) };

    public BoxArtMatch? Chosen { get; private set; }

    public BoxArtPicker(string gameName, List<string> index)
    {
        _index = index;
        Title = $"Box art for {gameName}";
        Width = 760; Height = 560; MinWidth = 520; MinHeight = 380;
        WindowStartupLocation = WindowStartupLocation.CenterOwner;
        FontFamily = new FontFamily("Segoe UI");

        var cancel = new Button { Content = "Cancel", IsCancel = true, Padding = new Thickness(16, 6, 16, 6) };
        _ok.Click += (_, _) => Accept();
        _list.SelectionChanged += (_, _) => ShowPreview();
        _list.MouseDoubleClick += (_, _) => Accept();
        _search.TextChanged += (_, _) => Filter();
        _search.PreviewKeyDown += (_, e) =>
        {
            if (e.Key == Key.Down && _list.Items.Count > 0) { _list.SelectedIndex = Math.Min(_list.SelectedIndex + 1, _list.Items.Count - 1); e.Handled = true; }
            if (e.Key == Key.Up && _list.SelectedIndex > 0) { _list.SelectedIndex--; e.Handled = true; }
        };

        var left = new DockPanel { Margin = new Thickness(12) };
        var hint = new TextBlock { Text = "Search the 3DO box art collection:", Margin = new Thickness(0, 0, 0, 6) };
        DockPanel.SetDock(hint, Dock.Top);
        DockPanel.SetDock(_search, Dock.Top);
        left.Children.Add(hint);
        left.Children.Add(_search);
        _list.Margin = new Thickness(0, 8, 0, 0);
        left.Children.Add(_list);

        var previewHost = new Grid { Margin = new Thickness(0, 12, 12, 12), Background = new SolidColorBrush(Color.FromRgb(0x20, 0x20, 0x29)) };
        previewHost.Children.Add(_previewNote);
        previewHost.Children.Add(_preview);

        var buttons = new StackPanel { Orientation = Orientation.Horizontal, HorizontalAlignment = HorizontalAlignment.Right, Margin = new Thickness(12, 0, 12, 12) };
        buttons.Children.Add(_ok);
        buttons.Children.Add(cancel);

        var root = new Grid();
        root.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1.3, GridUnitType.Star) });
        root.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
        root.RowDefinitions.Add(new RowDefinition { Height = new GridLength(1, GridUnitType.Star) });
        root.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });
        Grid.SetColumn(previewHost, 1);
        Grid.SetRow(buttons, 1); Grid.SetColumnSpan(buttons, 2);
        root.Children.Add(left);
        root.Children.Add(previewHost);
        root.Children.Add(buttons);
        Content = root;

        _search.Text = gameName.Replace('_', ' ');   // runs Filter()
        Loaded += (_, _) => { _search.Focus(); _search.SelectAll(); };
    }

    void Filter()
    {
        string q = _search.Text.Trim();
        List<string> items;
        if (q.Length == 0)
            items = _index.OrderBy(n => n, StringComparer.OrdinalIgnoreCase).ToList();
        else
        {
            // Loose title match first, then plain "contains" matches.
            var ranked = BoxArt.Rank(q, _index).Select(m => m.FileName);
            var contains = _index.Where(n => n.Contains(q, StringComparison.OrdinalIgnoreCase))
                                 .OrderBy(n => n, StringComparer.OrdinalIgnoreCase);
            items = ranked.Concat(contains).Distinct().Take(200).ToList();
        }
        _list.ItemsSource = items.Select(n => n.EndsWith(".png", StringComparison.OrdinalIgnoreCase) ? n[..^4] : n).ToList();
        _list.SelectedIndex = items.Count > 0 ? 0 : -1;
        if (items.Count == 0) ShowPreview();
    }

    void ShowPreview()
    {
        _ok.IsEnabled = _list.SelectedItem != null;
        if (_list.SelectedItem is not string name) { _preview.Source = null; _previewNote.Text = "No matching covers"; return; }
        _previewNote.Text = "Loading…";
        try
        {
            var bmp = new BitmapImage();
            bmp.BeginInit();
            bmp.UriSource = new Uri(BoxArt.UrlFor(name + ".png"));
            bmp.DecodePixelHeight = 500;
            bmp.EndInit();
            bmp.DownloadFailed += (_, _) => _previewNote.Text = "Preview unavailable";
            _preview.Source = bmp;
        }
        catch { _preview.Source = null; _previewNote.Text = "Preview unavailable"; }
    }

    void Accept()
    {
        if (_list.SelectedItem is not string name) return;
        string file = name + ".png";
        Chosen = new BoxArtMatch(file, BoxArt.UrlFor(file), 1);
        DialogResult = true;
    }
}
