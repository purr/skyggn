using System.Collections.ObjectModel;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Controls.Primitives;
using Skyggn.Models;
using Skyggn.Services;
using Windows.Storage.Pickers;

namespace Skyggn.Pages;

public sealed partial class ThumbnailsPage : Page
{
    // the size explorer asks for in its largest view
    private const uint PreviewSize = 256;

    private bool _loading;
    // the sample files that ship with the app, then the file the user chose, if any
    private readonly ObservableCollection<PreviewItem> _previews = [];
    private PreviewItem? _chosen;

    public ThumbnailsPage()
    {
        InitializeComponent();
    }

    private void OnLoaded(object sender, RoutedEventArgs e)
    {
        _loading = true;
        if (_previews.Count == 0)
        {
            var samples = Path.Combine(AppContext.BaseDirectory, "Assets", "samples");
            _previews.Add(new PreviewItem(Path.Combine(samples, "video.mkv"), Text.Get("PreviewSampleVideo")));
            _previews.Add(new PreviewItem(Path.Combine(samples, "song.mp3"), Text.Get("PreviewSampleSong")));
            Previews.ItemsSource = _previews;
            UpdatePreviews();
        }
        BadgeCard.Description = Engine.Description(Engine.Badge);
        BadgeCornerCard.Description = Engine.Description(Engine.BadgeCorner);
        BadgeSizeCard.Description = Engine.Description(Engine.BadgeSize);
        PlaceholderCard.Description = Engine.Description(Engine.Placeholder);
        PositionCard.Description = Engine.Description(Engine.FramePosition);
        BlackFramesCard.Description = Engine.Description(Engine.SkipBlackFrames);
        CoverArtCard.Description = Engine.Description(Engine.PreferCoverArt);
        LowImpactCard.Description = Engine.Description(Engine.LowImpact);
        TimeLimitCard.Description = Engine.Description(Engine.TimeLimitMs);

        var position = Engine.Definition(Engine.FramePosition);
        PositionSlider.Minimum = position.Min;
        PositionSlider.Maximum = position.Max;
        PositionSlider.Value = Engine.GetSetting(Engine.FramePosition);
        PositionText.Text = $"{PositionSlider.Value:0} %";

        var badgeSize = Engine.Definition(Engine.BadgeSize);
        BadgeSizeSlider.Minimum = badgeSize.Min;
        BadgeSizeSlider.Maximum = badgeSize.Max;
        BadgeSizeSlider.Value = Engine.GetSetting(Engine.BadgeSize);
        BadgeSizeText.Text = $"{BadgeSizeSlider.Value:0} %";

        var limit = Engine.Definition(Engine.TimeLimitMs);
        TimeLimitBox.Minimum = limit.Min / 1000.0;
        TimeLimitBox.Maximum = limit.Max / 1000.0;
        TimeLimitBox.Value = Engine.GetSetting(Engine.TimeLimitMs) / 1000.0;

        BadgeBox.SelectedIndex = (int)Engine.GetSetting(Engine.Badge);
        BadgeCornerBox.SelectedIndex = (int)Engine.GetSetting(Engine.BadgeCorner);
        ShowBadgeLayout();
        PlaceholderBox.SelectedIndex = (int)Engine.GetSetting(Engine.Placeholder);
        BlackFramesSwitch.IsOn = Engine.GetSetting(Engine.SkipBlackFrames) != 0;
        CoverArtSwitch.IsOn = Engine.GetSetting(Engine.PreferCoverArt) != 0;
        LowImpactSwitch.IsOn = Engine.GetSetting(Engine.LowImpact) != 0;
        _loading = false;
    }

    private void OnBadgeChanged(object sender, SelectionChangedEventArgs e)
    {
        ShowBadgeLayout();
        Save(Engine.Badge, (uint)Math.Max(0, BadgeBox.SelectedIndex));
    }

    // position and size mean nothing while the badge is off
    private void ShowBadgeLayout()
    {
        var shown = BadgeBox.SelectedIndex > 0;
        BadgeCornerCard.IsEnabled = shown;
        BadgeSizeCard.IsEnabled = shown;
    }

    private void OnBadgeCornerChanged(object sender, SelectionChangedEventArgs e) =>
        Save(Engine.BadgeCorner, (uint)Math.Max(0, BadgeCornerBox.SelectedIndex));

    private void OnBadgeSizeChanged(object sender, RangeBaseValueChangedEventArgs e)
    {
        BadgeSizeText.Text = $"{e.NewValue:0} %";
        Save(Engine.BadgeSize, (uint)e.NewValue);
    }

    private void OnPlaceholderChanged(object sender, SelectionChangedEventArgs e) =>
        Save(Engine.Placeholder, (uint)Math.Max(0, PlaceholderBox.SelectedIndex));

    private void OnPositionChanged(object sender, RangeBaseValueChangedEventArgs e)
    {
        PositionText.Text = $"{e.NewValue:0} %";
        Save(Engine.FramePosition, (uint)e.NewValue);
    }

    private void OnBlackFramesToggled(object sender, RoutedEventArgs e) =>
        Save(Engine.SkipBlackFrames, BlackFramesSwitch.IsOn ? 1u : 0u);

    private void OnCoverArtToggled(object sender, RoutedEventArgs e) =>
        Save(Engine.PreferCoverArt, CoverArtSwitch.IsOn ? 1u : 0u);

    private void OnLowImpactToggled(object sender, RoutedEventArgs e) =>
        Save(Engine.LowImpact, LowImpactSwitch.IsOn ? 1u : 0u);

    private void OnTimeLimitChanged(NumberBox sender, NumberBoxValueChangedEventArgs args)
    {
        if (double.IsNaN(args.NewValue))
        {
            sender.Value = args.OldValue;  // an emptied box keeps the last value
            return;
        }
        Save(Engine.TimeLimitMs, (uint)Math.Round(args.NewValue * 1000));
    }

    private void Save(string name, uint value)
    {
        if (_loading)
        {
            return;
        }
        ErrorBar.IsOpen = false;
        try
        {
            Engine.SetSetting(name, value);
        }
        catch (EngineException error)
        {
            ErrorBar.Message = error.Message;
            ErrorBar.IsOpen = true;
            return;
        }
        // speed settings leave the look alone
        if (name is not (Engine.LowImpact or Engine.TimeLimitMs))
        {
            RefreshBar.IsOpen = true;
        }
        UpdatePreviews();
    }

    private void UpdatePreviews()
    {
        foreach (var preview in _previews)
        {
            _ = preview.UpdateAsync(PreviewSize);
        }
    }

    private async void OnRefreshNowClick(object sender, RoutedEventArgs e)
    {
        ErrorBar.IsOpen = false;
        try
        {
            if (await RefreshThumbnails.AskAndRunAsync(XamlRoot))
            {
                RefreshBar.IsOpen = false;
            }
        }
        catch (Exception error)
        {
            ErrorBar.Message = error.Message;
            ErrorBar.IsOpen = true;
        }
    }

    private async void OnChooseClick(object sender, RoutedEventArgs e)
    {
        ErrorBar.IsOpen = false;
        try
        {
            var picker = new FileOpenPicker();
            WinRT.Interop.InitializeWithWindow.Initialize(picker, WinRT.Interop.WindowNative.GetWindowHandle(
                ((App)Application.Current).MainWindow!));
            // the types skyggn makes thumbnails for: explorer hands it no other file
            foreach (var format in Engine.Formats)
            {
                picker.FileTypeFilter.Add(format.Extension);
            }
            var file = await picker.PickSingleFileAsync();
            if (file is null)
            {
                return;
            }
            // one chosen file at a time, after the samples
            if (_chosen is not null)
            {
                _previews.Remove(_chosen);
            }
            _chosen = new PreviewItem(file.Path, file.Name);
            _previews.Add(_chosen);
            await _chosen.UpdateAsync(PreviewSize);
        }
        catch (Exception error)
        {
            // the picker fails when the app runs as administrator, among others; an async void
            // handler that throws would close the app
            ErrorBar.Message = error.Message;
            ErrorBar.IsOpen = true;
        }
    }
}
