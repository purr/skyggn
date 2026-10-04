using System.ComponentModel;
using Skyggn.Services;

namespace Skyggn.Models;

// one file type on the file types page: whether skyggn handles it now (Applied) and whether the
// user wants it to (IsOn)
public sealed class FileTypeItem(Format format, bool applied) : INotifyPropertyChanged
{
    private bool _isOn = applied;

    public event PropertyChangedEventHandler? PropertyChanged;

    public string Extension { get; } = format.Extension;

    public Format Format { get; } = format;

    public bool Applied { get; private set; } = applied;

    public bool IsOn
    {
        get => _isOn;
        set
        {
            if (_isOn == value)
            {
                return;
            }
            _isOn = value;
            PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(nameof(IsOn)));
            PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(nameof(IsChanged)));
        }
    }

    public bool IsChanged => _isOn != Applied;

    // `on` is what was applied, which the box may no longer show
    public void MarkApplied(bool on)
    {
        Applied = on;
        PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(nameof(IsChanged)));
    }

    public void Discard() => IsOn = Applied;
}
