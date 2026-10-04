using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Skyggn.Services;

namespace Skyggn.Pages;

// windows keeps the thumbnails it made; after a change to how they look, or to which types skyggn
// handles, the old ones stay until the cache is cleared. the pages that change those offer this.
internal static class RefreshThumbnails
{
    // asks first, since file explorer closes for a moment; true when the thumbnails were refreshed
    public static async Task<bool> AskAndRunAsync(XamlRoot root)
    {
        var confirm = new ContentDialog
        {
            XamlRoot = root,
            Title = Text.Get("RefreshDialogTitle"),
            Content = Text.Get("RefreshDialogContent"),
            PrimaryButtonText = Text.Get("RefreshDialogRefresh"),
            CloseButtonText = Text.Get("DialogCancel"),
            DefaultButton = ContentDialogButton.Primary,
        };
        if (await confirm.ShowAsync() != ContentDialogResult.Primary)
        {
            return false;
        }
        await ThumbnailCache.ClearAsync();
        return true;
    }
}
