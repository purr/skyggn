using System.Diagnostics;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Skyggn.Services;

namespace Skyggn.Pages;

public sealed partial class GeneralPage : Page
{
    // set while the page writes its controls, so their change events do not act
    private bool _loading;

    public GeneralPage()
    {
        InitializeComponent();
    }

    private void OnLoaded(object sender, RoutedEventArgs e) => Load();

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
