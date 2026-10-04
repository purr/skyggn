using System.Globalization;
using Microsoft.Windows.ApplicationModel.Resources;

namespace Skyggn.Services;

// the app's texts, from Strings/<language>/Resources.resw in the language the app runs in. the
// pages' own texts come from the same files, through x:Uid.
public static class Text
{
    private static readonly ResourceLoader Loader = new();

    // a name the files do not have means the code and the files have drifted apart
    public static string Get(string name)
    {
        var text = Loader.GetString(name);
        return string.IsNullOrEmpty(text)
            ? throw new InvalidOperationException($"Resources.resw has no text named {name}")
            : text;
    }

    public static string Format(string name, params object[] values) =>
        string.Format(CultureInfo.CurrentCulture, Get(name), values);
}
