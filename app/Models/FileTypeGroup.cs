using System.ComponentModel;
using Skyggn.Services;

namespace Skyggn.Models;

// the file types of one kind (video, audio, image, camera raw, book, document) on the file types page
public sealed class FileTypeGroup : INotifyPropertyChanged
{
    public FileTypeGroup(Category category, IEnumerable<FileTypeItem> types)
    {
        Category = category;
        Types = types.OrderBy(type => type.Extension, StringComparer.Ordinal).ToList();
        foreach (var type in Types)
        {
            type.PropertyChanged += (_, _) =>
            {
                PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(nameof(Summary)));
                Changed?.Invoke(this, EventArgs.Empty);
            };
        }
    }

    public event PropertyChangedEventHandler? PropertyChanged;

    // a type in this group was turned on or off, or applied
    public event EventHandler? Changed;

    public Category Category { get; }

    public IReadOnlyList<FileTypeItem> Types { get; }

    public string Title => Text.Get("Group" + Category);

    public string Description => Text.Get("Group" + Category + "Description");

    public string Glyph => Category switch
    {
        Category.Video => "",
        Category.Audio => "",
        Category.Image => "",
        Category.Raw => "",
        Category.Book => "",
        _ => "",
    };

    public string Summary => Text.Format("GroupSummary", Types.Count(type => type.IsOn), Types.Count);

    public void TurnAllOn() => Set(_ => true);

    public void TurnRecommendedOn() => Set(type => type.Format.Recommended);

    public void TurnAllOff() => Set(_ => false);

    private void Set(Func<FileTypeItem, bool> on)
    {
        foreach (var type in Types)
        {
            type.IsOn = on(type);
        }
    }
}
