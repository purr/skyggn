using System.ComponentModel;
using System.Diagnostics;
using System.Runtime.InteropServices;

namespace Skyggn.Services;

// turning skyggn and its file types on and off. changes for this user only are made directly;
// changes for every user need admin rights, so skyggnctl does them, elevated: one uac prompt.
public static class Registration
{
    private const int ErrorCancelled = 1223;

    // where skyggn is registered: for every user (the installer), for this user only (a
    // developer build turned on with skyggnctl install --user), or nowhere
    public static Scope? ActiveScope =>
        Engine.IsRegistered(Scope.Machine) ? Scope.Machine
        : Engine.IsRegistered(Scope.User) ? Scope.User
        : null;

    public static bool NeedsAdmin(Scope scope) => scope == Scope.Machine;

    public static int HandledCount(Scope scope) => Engine.Formats.Count(format => Engine.IsHandled(scope, format.Extension));

    // returns false when the user declined the uac prompt
    public static async Task<bool> InstallAsync(Scope scope)
    {
        if (NeedsAdmin(scope))
        {
            return await RunElevatedAsync("install");
        }
        Engine.Install(scope);
        Engine.NotifyShell();
        return true;
    }

    public static async Task<bool> UninstallAsync(Scope scope)
    {
        if (NeedsAdmin(scope))
        {
            return await RunElevatedAsync("uninstall");
        }
        Engine.Unregister(scope);
        Engine.NotifyShell();
        return true;
    }

    public static async Task<bool> SetTypesAsync(Scope scope, IReadOnlyCollection<(string Extension, bool On)> changes)
    {
        if (changes.Count == 0)
        {
            return true;
        }
        if (NeedsAdmin(scope))
        {
            return await RunElevatedAsync(
                "types " + string.Join(' ', changes.Select(change => (change.On ? "+" : "-") + change.Extension)));
        }
        if (changes.Any(change => change.On))
        {
            Engine.Register(scope);
        }
        foreach (var (extension, on) in changes)
        {
            Engine.SetHandled(scope, extension, on);
        }
        Engine.NotifyShell();
        return true;
    }

    public static async Task<bool> RepairAsync(Scope scope)
    {
        if (NeedsAdmin(scope))
        {
            return await RunElevatedAsync("repair");
        }
        Engine.Repair(scope);
        Engine.NotifyShell();
        return true;
    }

    // machine-wide only, so always through the elevated skyggnctl; false when the uac prompt was declined
    public static Task<bool> SetSystemDetailsAsync(bool take) => RunElevatedAsync(take ? "system-details on" : "system-details off");

    private static async Task<bool> RunElevatedAsync(string arguments)
    {
        var control = Path.Combine(AppContext.BaseDirectory, "skyggnctl.exe");
        var start = new ProcessStartInfo(control, arguments)
        {
            UseShellExecute = true,
            Verb = "runas",
            WindowStyle = ProcessWindowStyle.Hidden,
        };
        Process? process;
        try
        {
            process = Process.Start(start);
        }
        catch (Win32Exception error) when (error.NativeErrorCode == ErrorCancelled)
        {
            return false;  // the user said no to the uac prompt; nothing changed
        }
        if (process is null)
        {
            throw new InvalidOperationException(Text.Format("ControlNotStarted", arguments));
        }
        using (process)
        {
            await process.WaitForExitAsync();
            // a failure exits with its hresult, whose message says why; a usage error with a small code
            if (process.ExitCode < 0 && Marshal.GetExceptionForHR(process.ExitCode) is { } reason)
            {
                throw new InvalidOperationException(
                    Text.Format("ActionFailed", "skyggnctl " + arguments, reason.Message));
            }
            if (process.ExitCode != 0)
            {
                throw new InvalidOperationException(Text.Format("ControlFailed", arguments, process.ExitCode));
            }
        }
        return true;
    }
}
