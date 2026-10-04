using System.Runtime.InteropServices;
using Microsoft.UI.Windowing;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Media.Imaging;
using Skyggn.Pages;
using Skyggn.Services;
using Windows.Graphics;

namespace Skyggn;

public sealed partial class MainWindow : Window
{
    // in device-independent pixels, scaled to the monitor
    private const int StartWidth = 1000;
    private const int StartHeight = 740;

    public MainWindow()
    {
        InitializeComponent();
        ExtendsContentIntoTitleBar = true;
        SetTitleBar(TitleBarArea);

        var icon = Path.Combine(AppContext.BaseDirectory, "Assets", "skyggn.ico");
        AppWindow.SetIcon(icon);
        TitleBarIcon.Source = new BitmapImage(new Uri(icon));

        var scale = GetDpiForWindow(WinRT.Interop.WindowNative.GetWindowHandle(this)) / 96.0;
        AppWindow.Resize(new SizeInt32((int)(StartWidth * scale), (int)(StartHeight * scale)));
        AppWindow.Closing += OnClosing;

        Navigation.SelectedItem = Navigation.MenuItems[0];
    }

    // closing while the thumbnail cache is being cleared would end the app before it starts file
    // explorer again: the window waits for the clear, then closes
    private async void OnClosing(AppWindow sender, AppWindowClosingEventArgs args)
    {
        if (ThumbnailCache.Clearing.IsCompleted)
        {
            return;
        }
        args.Cancel = true;
        await ThumbnailCache.WaitAsync();
        Close();
    }

    private void OnNavigationSelectionChanged(NavigationView sender, NavigationViewSelectionChangedEventArgs args)
    {
        ContentFrame.Content = (args.SelectedItem as NavigationViewItem)?.Tag switch
        {
            "thumbnails" => new ThumbnailsPage(),
            "types" => new FileTypesPage(),
            "about" => new AboutPage(),
            _ => new GeneralPage(),
        };
    }

    [LibraryImport("user32.dll")]
    private static partial uint GetDpiForWindow(IntPtr window);
}
