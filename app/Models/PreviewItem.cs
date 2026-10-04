using System.ComponentModel;
using System.Runtime.InteropServices.WindowsRuntime;
using Microsoft.UI.Xaml.Media;
using Microsoft.UI.Xaml.Media.Imaging;
using Skyggn.Services;

namespace Skyggn.Models;

// one thumbnail in the thumbnails page's preview: a sample file, or one the user chose
public sealed class PreviewItem(string path, string label) : INotifyPropertyChanged
{
    // one thumbnail is made at a time. requests that come in meanwhile (a slider being dragged
    // sends one per step) fold into a single one made after it, with the settings as they are then:
    // made one each, they queued up and the preview fell seconds behind the slider.
    private bool _making;
    private bool _again;
    private uint _size;

    public event PropertyChangedEventHandler? PropertyChanged;

    public string Path { get; } = path;

    public string Label { get; } = label;

    public ImageSource? Image { get; private set; }

    // what shows instead of the picture: that it is being made, or why there is none
    public string Hint { get; private set; } = Text.Get("PreviewMaking");

    // makes the thumbnail again with the current settings, at `size` like explorer asks for. called
    // on the ui thread only, which the awaits return to, so the flags need no lock.
    public async Task UpdateAsync(uint size)
    {
        _size = size;
        if (_making)
        {
            _again = true;
            return;
        }
        _making = true;
        try
        {
            do
            {
                _again = false;
                await MakeAsync(_size);
            } while (_again);
        }
        finally
        {
            _making = false;
        }
    }

    private async Task MakeAsync(uint size)
    {
        try
        {
            var (pixels, width, height) = await Task.Run(() => Engine.Thumbnail(Path, size));
            var bitmap = new WriteableBitmap(width, height);
            using (var stream = bitmap.PixelBuffer.AsStream())
            {
                await stream.WriteAsync(pixels);
            }
            Show(bitmap, "");
        }
        catch (EngineException error)
        {
            Show(null, Text.Format("PreviewFailed", error.Message));
        }
    }

    private void Show(ImageSource? image, string hint)
    {
        Image = image;
        Hint = hint;
        PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(nameof(Image)));
        PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(nameof(Hint)));
    }
}
