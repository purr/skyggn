using System.Diagnostics;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Skyggn.Services;
using Windows.Storage.Pickers;

namespace Skyggn.Pages;

public sealed partial class GeneralPage : Page
{
    // set while the page writes its controls, so their change events do not act
    private bool _loading;

    public GeneralPage()
    {
        InitializeComponent();
        _prepareTimer.Tick += (_, _) => ShowPreparation();
    }

    // shows a folder preparation's progress while it runs: the engine's calls only record it
    // (FolderPreparation), and the page looks ten times a second, so a burst of small files never
    // leaves a stale count, and a page shown again starts from the real state
    private readonly DispatcherTimer _prepareTimer = new() { Interval = TimeSpan.FromMilliseconds(100) };

    private void OnLoaded(object sender, RoutedEventArgs e)
    {
        Load();
        if ((FolderPreparation.Current ?? FolderPreparation.Finished) is { } job)
        {
            PrepareFolderBox.Text = job.Folder;
            PrepareSubfoldersSwitch.IsOn = job.Recursive;
        }
        ShowPreparation();
    }

    private void OnUnloaded(object sender, RoutedEventArgs e) => _prepareTimer.Stop();

    private async void OnPrepareBrowseClick(object sender, RoutedEventArgs e)
    {
        ErrorBar.IsOpen = false;
        try
        {
            var picker = new FolderPicker();
            WinRT.Interop.InitializeWithWindow.Initialize(picker, WinRT.Interop.WindowNative.GetWindowHandle(
                ((App)Application.Current).MainWindow!));
            picker.FileTypeFilter.Add("*");
            if (await picker.PickSingleFolderAsync() is { } folder)
            {
                PrepareFolderBox.Text = folder.Path;
            }
        }
        catch (Exception error)
        {
            // the picker fails when the app runs as administrator, among others; a path can still be
            // pasted, and an async void handler that throws would close the app
            ErrorBar.Message = error.Message;
            ErrorBar.IsOpen = true;
        }
    }

    private void OnPrepareClick(object sender, RoutedEventArgs e)
    {
        if (FolderPreparation.Current is not null)
        {
            FolderPreparation.Stop();
            ShowPreparation();
            return;
        }
        // a pasted path may be in quotes, as explorer's "copy as path" gives it
        var folder = PrepareFolderBox.Text.Trim().Trim('"').Trim();
        if (folder.Length == 0 || !Directory.Exists(folder))
        {
            PrepareProgressCard.Visibility = Visibility.Visible;
            PrepareProgressBar.Visibility = Visibility.Collapsed;
            PrepareProgressText.Text = folder.Length == 0 ? Text.Get("PrepareNoFolder") : Text.Format("PrepareMissing", folder);
            return;
        }
        FolderPreparation.Start(folder, PrepareSubfoldersSwitch.IsOn);
        ShowPreparation();
    }

    // the card as the running job, or the last one that ended, says
    private void ShowPreparation()
    {
        var running = FolderPreparation.Current;
        PrepareButton.Content = Text.Get(running is null ? "PrepareStart" : "PrepareStop");
        PrepareButton.IsEnabled = running is not { Stopping: true };
        PrepareFolderBox.IsEnabled = running is null;
        PrepareBrowseButton.IsEnabled = running is null;
        PrepareSubfoldersSwitch.IsEnabled = running is null;
        if (running is null)
        {
            _prepareTimer.Stop();
            if (FolderPreparation.Finished is { } finished)
            {
                ShowPrepareOutcome(finished.Done);
            }
            return;
        }
        _prepareTimer.Start();
        PrepareProgressCard.Visibility = Visibility.Visible;
        PrepareProgressBar.Visibility = Visibility.Visible;
        var latest = running.Latest;
        // while it looks for files, the count grows and the folder read is named; the bar moves once
        // the total is known
        var looking = latest is null || (latest.Done == 0 && latest.File.Length > 0);
        PrepareProgressBar.IsIndeterminate = looking;
        PrepareProgressBar.Maximum = Math.Max(1u, latest?.Total ?? 1);
        PrepareProgressBar.Value = latest?.Done ?? 0;
        PrepareProgressText.Text = latest is null ? Text.Get("PrepareListing")
            : latest.Done == 0 ? Text.Format("PrepareFound", latest.Total)
            : Text.Format("PrepareProgress", latest.Done, latest.Total, Path.GetFileName(latest.File));
    }

    private void ShowPrepareOutcome(Task<Preparation> done)
    {
        PrepareProgressCard.Visibility = Visibility.Visible;
        PrepareProgressBar.Visibility = Visibility.Collapsed;
        if (!done.IsCompletedSuccessfully)
        {
            // the engine's message names what was being done and why it failed
            PrepareProgressText.Text = done.Exception?.InnerException?.Message ?? "";
            return;
        }
        var result = done.Result;
        var parts = new List<string> { Text.Format(result.Stopped ? "PrepareStopped" : "PrepareDone", result.Made, result.Kept) };
        if (result.WithoutPicture > 0)
        {
            parts.Add(Text.Format("PrepareNoPicture", result.WithoutPicture));
        }
        if (result.OnlineOnly > 0)
        {
            parts.Add(Text.Format("PrepareOnlineOnly", result.OnlineOnly));
        }
        if (result.UnreadableFolders > 0)
        {
            parts.Add(Text.Format("PrepareUnreadable", result.UnreadableFolders));
        }
        PrepareProgressText.Text = string.Join(" ", parts);
    }

    private void Load()
    {
        _loading = true;
        var scope = Registration.ActiveScope;
        EnabledSwitch.IsOn = scope is not null;
        // a fresh install is for every user; an existing one keeps its scope
        EnabledShield.Visibility = Registration.NeedsAdmin(scope ?? Scope.Machine) ? Visibility.Visible : Visibility.Collapsed;
        if (scope is { } active)
        {
            StatusBar.Severity = InfoBarSeverity.Success;
            StatusBar.Title = Text.Get("StatusOnTitle");
            StatusBar.Message = Text.Format(active == Scope.User ? "StatusOnMessageUser" : "StatusOnMessage",
                Registration.HandledCount(active));
        }
        else
        {
            StatusBar.Severity = InfoBarSeverity.Warning;
            StatusBar.Title = Text.Get("StatusOffTitle");
            StatusBar.Message = Text.Get("StatusOffMessage");
        }
        AppIconSwitch.IsOn = ExplorerSettings.AppIconOnThumbnails;

        // dead entries can sit in either scope; the machine's need admin to remove
        var dead = (Machine: Engine.CountDead(Scope.Machine), User: Engine.CountDead(Scope.User));
        var total = dead.Machine + dead.User;
        RepairCard.Visibility = total > 0 ? Visibility.Visible : Visibility.Collapsed;
        RepairCard.Description = total == 1 ? Text.Get("RepairDescriptionOne") : Text.Format("RepairDescriptionMany", total);
        RepairShield.Visibility = dead.Machine > 0 ? Visibility.Visible : Visibility.Collapsed;

        SystemDetailsCard.Visibility = scope == Scope.Machine ? Visibility.Visible : Visibility.Collapsed;
        SystemDetailsSwitch.IsOn = Engine.SystemDetailsTaken;

        LanguageCard.Description = Engine.Description(Engine.Language);
        LanguageBox.SelectedIndex = (int)Engine.GetSetting(Engine.Language);
        ShowRestart();
        _loading = false;
    }

    private async void OnEnabledToggled(object sender, RoutedEventArgs e)
    {
        if (_loading)
        {
            return;
        }
        await Run(async () =>
        {
            var on = EnabledSwitch.IsOn;
            var scope = Registration.ActiveScope ?? Scope.Machine;
            _ = on ? await Registration.InstallAsync(scope) : await Registration.UninstallAsync(scope);
        });
    }

    private async void OnRefreshClick(object sender, RoutedEventArgs e) =>
        await Run(() => RefreshThumbnails.AskAndRunAsync(XamlRoot));

    private async void OnAppIconToggled(object sender, RoutedEventArgs e)
    {
        if (_loading)
        {
            return;
        }
        await Run(() =>
        {
            ExplorerSettings.AppIconOnThumbnails = AppIconSwitch.IsOn;
            return Task.CompletedTask;
        });
    }

    private async void OnRepairClick(object sender, RoutedEventArgs e)
    {
        await Run(async () =>
        {
            if (Engine.CountDead(Scope.User) > 0)
            {
                await Registration.RepairAsync(Scope.User);
            }
            if (Engine.CountDead(Scope.Machine) > 0)
            {
                await Registration.RepairAsync(Scope.Machine);
            }
        });
    }

    private async void OnSystemDetailsToggled(object sender, RoutedEventArgs e)
    {
        if (_loading)
        {
            return;
        }
        await Run(async () => await Registration.SetSystemDetailsAsync(SystemDetailsSwitch.IsOn));
    }

    private async void OnLanguageChanged(object sender, SelectionChangedEventArgs e)
    {
        if (_loading)
        {
            return;
        }
        await Run(() =>
        {
            Engine.SetSetting(Engine.Language, (uint)Math.Max(0, LanguageBox.SelectedIndex));
            return Task.CompletedTask;
        });
    }

    // the language is chosen as the app starts (App.UseChosenLanguage), so a new one needs a restart
    private void ShowRestart() =>
        RestartButton.Visibility = Engine.GetSetting(Engine.Language) != App.StartLanguage
            ? Visibility.Visible
            : Visibility.Collapsed;

    private async void OnRestartClick(object sender, RoutedEventArgs e)
    {
        // a thumbnail refresh under way must finish first, or file explorer stays closed
        await ThumbnailCache.WaitAsync();
        Process.Start(new ProcessStartInfo(Environment.ProcessPath!) { UseShellExecute = false });
        Application.Current.Exit();
    }

    // runs a change, shows what went wrong if it fails, then shows the state as it now is. the
    // page's controls take no input meanwhile: a second change started during an elevated
    // skyggnctl (the switch flipped back, repair clicked) would run alongside it and interleave
    // their registry writes.
    private async Task Run(Func<Task> change)
    {
        ErrorBar.IsOpen = false;
        SetControlsEnabled(false);
        try
        {
            await change();
        }
        catch (Exception error)
        {
            ErrorBar.Message = error.Message;
            ErrorBar.IsOpen = true;
        }
        finally
        {
            SetControlsEnabled(true);
        }
        Load();
    }

    // the page's own controls only: the refresh's confirmation dialog must stay usable
    private void SetControlsEnabled(bool enabled)
    {
        foreach (var control in new Control[]
                     { EnabledSwitch, RefreshButton, AppIconSwitch, RepairButton, SystemDetailsSwitch, LanguageBox, RestartButton })
        {
            control.IsEnabled = enabled;
        }
    }
}
