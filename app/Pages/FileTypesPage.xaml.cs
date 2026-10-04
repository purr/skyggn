using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Skyggn.Models;
using Skyggn.Services;

namespace Skyggn.Pages;

public sealed partial class FileTypesPage : Page
{
    private List<FileTypeGroup> _groups = [];
    private Scope? _scope;

    public FileTypesPage()
    {
        InitializeComponent();
    }

    private void OnLoaded(object sender, RoutedEventArgs e) => Load();

    private void Load()
    {
        _scope = Registration.ActiveScope;
        OffBar.IsOpen = _scope is null;
        Groups.IsEnabled = _scope is not null;
        _groups = Engine.Formats
            .GroupBy(format => format.Category)
            .OrderBy(group => group.Key)
            .Select(group => new FileTypeGroup(group.Key,
                group.Select(format => new FileTypeItem(format,
                    _scope is { } scope && Engine.IsHandled(scope, format.Extension)))))
            .ToList();
        foreach (var group in _groups)
        {
            group.Changed += (_, _) => UpdateApplyBar();
        }
        Groups.ItemsSource = _groups;
        ApplyShield.Visibility = _scope is { } active && Registration.NeedsAdmin(active)
            ? Visibility.Visible
            : Visibility.Collapsed;
        UpdateApplyBar();
    }

    private IEnumerable<FileTypeItem> Changed => _groups.SelectMany(group => group.Types).Where(type => type.IsChanged);

    private void UpdateApplyBar()
    {
        var count = Changed.Count();
        ApplyBar.Visibility = count > 0 ? Visibility.Visible : Visibility.Collapsed;
        PendingText.Text = count == 1 ? Text.Get("PendingOne") : Text.Format("PendingMany", count);
    }

    private void OnDiscardClick(object sender, RoutedEventArgs e)
    {
        foreach (var type in Changed.ToList())
        {
            type.Discard();
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

    private async void OnApplyClick(object sender, RoutedEventArgs e)
    {
        if (_scope is not { } scope)
        {
            return;
        }
        // what is sent, as it is now: the boxes can change while an elevated skyggnctl runs
        var changes = Changed.Select(type => (Type: type, On: type.IsOn)).ToList();
        ErrorBar.IsOpen = false;
        ApplyButton.IsEnabled = false;
        try
        {
            if (await Registration.SetTypesAsync(scope, changes.Select(change => (change.Type.Extension, change.On)).ToList()))
            {
                foreach (var (type, on) in changes)
                {
                    type.MarkApplied(on);
                }
                RefreshBar.IsOpen = true;
            }
        }
        catch (Exception error)
        {
            ErrorBar.Message = error.Message;
            ErrorBar.IsOpen = true;
            // show what actually happened: part of the changes may have been made
            Load();
        }
        ApplyButton.IsEnabled = true;
        UpdateApplyBar();
    }
}
