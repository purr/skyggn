using System.Runtime.InteropServices;
using Microsoft.UI.Xaml;
using Microsoft.Windows.Globalization;
using Skyggn.Services;

namespace Skyggn;

public partial class App : Application
{
    private Window? _window;

    // file pickers need the window they belong to
    public Window? MainWindow => _window;

    private const uint IconError = 0x10;

    // the Language setting the app started with: its texts are in that language until a restart
    public static uint StartLanguage { get; private set; }

    public App()
    {
        UseChosenLanguage();
        InitializeComponent();
        UnhandledException += (_, e) =>
        {
            e.Handled = true;
            Stop(e.Exception);
        };
    }

    protected override void OnLaunched(LaunchActivatedEventArgs args)
    {
        try
        {
            _ = Engine.Version;
        }
        catch (DllNotFoundException)
        {
            // the app is only a front end for the engine; without it there is nothing to show
            MessageBoxW(IntPtr.Zero, Text.Get("EngineMissing"), "skyggn", IconError);
            Exit();
            return;
        }
        try
        {
            _window = new MainWindow();
            _window.Activate();
        }
        catch (Exception error)
        {
            Stop(error);
        }
    }

    // before any text loads: the language chosen on the general page, or windows' own (no override)
    private static void UseChosenLanguage()
    {
        try
        {
            StartLanguage = Engine.GetSetting(Engine.Language);
        }
        catch (Exception error) when (error is DllNotFoundException || error.InnerException is DllNotFoundException)
        {
            return;  // OnLaunched says the engine is missing, in windows' language
        }
        // windows' own language needs no override; an empty one is not allowed (the app does not start)
        if (StartLanguage != 0)
        {
            ApplicationLanguages.PrimaryLanguageOverride = StartLanguage == 2 ? "de-DE" : "en-US";
        }
    }

    // an error nothing else caught: keep the details, say what happened, and close
    private void Stop(Exception error)
    {
        var folder = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "skyggn");
        Directory.CreateDirectory(folder);
        var report = Path.Combine(folder, "crash.txt");
        File.WriteAllText(report, $"{DateTime.Now:O}\n{error}\n");
        string message;
        try
        {
            message = Text.Format("Crashed", error.Message, report);
        }
        catch (Exception)
        {
            // the last word must not fail on the texts themselves: say it in english
            message = $"skyggn stopped because of an error:\n{error.Message}\n\nThe details are in {report}.";
        }
        MessageBoxW(IntPtr.Zero, message, "skyggn", IconError);
        Exit();
    }

    [LibraryImport("user32.dll", StringMarshalling = StringMarshalling.Utf16)]
    private static partial int MessageBoxW(IntPtr owner, string text, string caption, uint type);
}
