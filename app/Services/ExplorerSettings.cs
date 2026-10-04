using Microsoft.Win32;

namespace Skyggn.Services;

// file explorer's own options that touch thumbnails
public static class ExplorerSettings
{
    private const string AdvancedKey = @"Software\Microsoft\Windows\CurrentVersion\Explorer\Advanced";
    // folder options > view > "display file icon on thumbnails": explorer draws the default app's
    // icon in the corner where skyggn puts its badge
    private const string ShowTypeOverlay = "ShowTypeOverlay";

    public static bool AppIconOnThumbnails
    {
        // windows shows it unless the value says otherwise
        get
        {
            using var key = Registry.CurrentUser.OpenSubKey(AdvancedKey);
            return key?.GetValue(ShowTypeOverlay) is not int value || value != 0;
        }
        set
        {
            using var key = Registry.CurrentUser.CreateSubKey(AdvancedKey);
            key.SetValue(ShowTypeOverlay, value ? 1 : 0, RegistryValueKind.DWord);
            Engine.NotifyShell();
        }
    }
}
