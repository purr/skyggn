using System.Diagnostics;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Media.Imaging;
using Skyggn.Services;

namespace Skyggn.Pages;

public sealed partial class AboutPage : Page
{
    public AboutPage()
    {
        InitializeComponent();
    }

    private void OnLoaded(object sender, RoutedEventArgs e)
    {
        VersionText.Text = Text.Format("VersionText", Engine.Version);
        AppIcon.Source = new BitmapImage(new Uri(Path.Combine(AppContext.BaseDirectory, "Assets", "skyggn.ico")));
    }

    // every license text ships in the licenses folder next to the programs
    private void OnLicensesClick(object sender, RoutedEventArgs e) =>
        Process.Start(new ProcessStartInfo(Path.Combine(AppContext.BaseDirectory, "licenses")) { UseShellExecute = true });
}
