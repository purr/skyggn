using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;

namespace Skyggn.Services;

public enum Scope
{
    User = 0,
    Machine = 1,
}

public enum Category
{
    Video = 0,
    Audio = 1,
    Image = 2,
    Raw = 3,
    Book = 4,
    Document = 5,
}

public enum Handler
{
    None = 0,
    Skyggn = 1,
    Other = 2,
    Dead = 3,
}

public sealed record Format(string Extension, Category Category, bool Recommended);

public sealed record Setting(string Name, uint Default, uint Min, uint Max);

// what Engine.PrepareFolder did with the files it found (skyggn_prepare_result); Stopped when the
// progress callback asked it to stop
public sealed record Preparation(uint Made, uint Kept, uint WithoutPicture, uint OnlineOnly, uint UnreadableFolders,
    bool Stopped);

public sealed class EngineException(string action, int hresult)
    : Exception(Text.Format("ActionFailed", action, Marshal.GetExceptionForHR(hresult)?.Message ?? $"0x{hresult:X8}"))
{
    public int HResult32 { get; } = hresult;
}

// skyggn-engine.dll, the thumbnail engine, through its c api (engine/include/skyggn/skyggn.h)
public static unsafe partial class Engine
{
    private const string Library = "skyggn-engine.dll";

    // setting names, as the engine defines them
    public const string FramePosition = "FramePosition";
    public const string PreferCoverArt = "PreferCoverArt";
    public const string SkipBlackFrames = "SkipBlackFrames";
    public const string LowImpact = "LowImpact";
    public const string TimeLimitMs = "TimeLimitMs";
    public const string Badge = "BadgeStyle";
    public const string BadgeCorner = "BadgeCorner";
    public const string BadgeSize = "BadgeSize";
    public const string Placeholder = "Placeholder";
    public const string Language = "Language";

    [StructLayout(LayoutKind.Sequential)]
    private struct NativeFormat
    {
        public char* Extension;
        public Category Category;
        public int Recommended;
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct NativePreparation
    {
        public uint Made;
        public uint Kept;
        public uint WithoutPicture;
        public uint OnlineOnly;
        public uint UnreadableFolders;
    }

    // HRESULT_FROM_WIN32(ERROR_CANCELLED): skyggn_prepare_folder stopped because the callback said so
    private const int Cancelled = unchecked((int)0x800704C7);

    [StructLayout(LayoutKind.Sequential)]
    private struct NativeSetting
    {
        public char* Name;
        public char* Description;
        public uint Default;
        public uint Min;
        public uint Max;
    }

    public static string Version => new(skyggn_version());

    public static IReadOnlyList<Format> Formats { get; } = LoadFormats();

    public static IReadOnlyDictionary<string, Setting> Settings { get; } = LoadSettings();

    // the engine owns the setting list; a name it does not know means the two have drifted apart
    public static Setting Definition(string name) =>
        Settings.TryGetValue(name, out var setting)
            ? setting
            : throw new InvalidOperationException($"skyggn-engine.dll has no setting named {name}");

    // the setting's description in the language the app's texts are in
    public static string Description(string name) =>
        new(skyggn_setting_description(Definition(name).Name, Text.Get("LanguageTag")));

    public static uint GetSetting(string name) => skyggn_setting_get(Definition(name).Name);

    public static void SetSetting(string name, uint value) =>
        Check(skyggn_setting_set(Definition(name).Name, value), Text.Format("ActionSaving", name));

    // a thumbnail as windows would get it, at most size x size, as top-down bgra pixels
    public static (byte[] Pixels, int Width, int Height) Thumbnail(string path, uint size)
    {
        var pixels = new byte[size * size * 4];
        uint width;
        uint height;
        fixed (byte* buffer = pixels)
        {
            Check(skyggn_thumbnail_pixels(path, size, (uint*)buffer, size * size, out width, out height),
                Text.Format("ActionThumbnail", Path.GetFileName(path)));
        }
        Array.Resize(ref pixels, (int)(width * height * 4));
        return (pixels, (int)width, (int)height);
    }

    public static bool IsRegistered(Scope scope) => skyggn_is_registered(scope) != 0;

    public static void Install(Scope scope) => Check(skyggn_install(scope), Text.Get("ActionInstall"));

    public static void Register(Scope scope) => Check(skyggn_register_server(scope), Text.Get("ActionRegister"));

    public static void Unregister(Scope scope) => Check(skyggn_unregister_server(scope), Text.Get("ActionUnregister"));

    public static void SetHandled(Scope scope, string extension, bool handled) =>
        Check(skyggn_set_handled(scope, extension, handled ? 1 : 0),
            Text.Format(handled ? "ActionTypeOn" : "ActionTypeOff", extension));

    public static bool IsHandled(Scope scope, string extension) => skyggn_is_handled(scope, extension) != 0;

    public static Handler EffectiveHandler(string extension) => skyggn_effective_handler(extension);

    public static uint Repair(Scope scope)
    {
        Check(skyggn_repair(scope, out uint removed), Text.Get("ActionRepair"));
        return removed;
    }

    public static uint CountDead(Scope scope)
    {
        Check(skyggn_count_dead(scope, out uint count), Text.Get("ActionCountDead"));
        return count;
    }

    public static void NotifyShell() => skyggn_notify_shell();

    // whether skyggn reads the details windows keeps for its own handler (mkv, webm)
    public static bool SystemDetailsTaken => skyggn_system_details_taken() != 0;

    // has windows make the thumbnails of a folder's files it does not have yet (skyggn_prepare_folder).
    // `progress` gets done, total and the file just done, on the engine's threads, one at a time; it
    // returns false to stop. blocks until done, so it runs off the ui thread.
    public static Preparation PrepareFolder(string folder, bool recursive, Func<uint, uint, string, bool> progress)
    {
        var handle = GCHandle.Alloc(progress);
        try
        {
            var hresult = skyggn_prepare_folder(folder, recursive ? 1 : 0, 0, &OnPrepareProgress,
                GCHandle.ToIntPtr(handle), out NativePreparation result);
            if (hresult != Cancelled)
            {
                Check(hresult, Text.Format("ActionPrepare", folder));
            }
            return new Preparation(result.Made, result.Kept, result.WithoutPicture, result.OnlineOnly,
                result.UnreadableFolders, hresult == Cancelled);
        }
        finally
        {
            handle.Free();
        }
    }

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvStdcall)])]
    private static int OnPrepareProgress(IntPtr context, uint done, uint total, char* file)
    {
        try
        {
            var progress = (Func<uint, uint, string, bool>)GCHandle.FromIntPtr(context).Target!;
            return progress(done, total, new string(file)) ? 1 : 0;
        }
        catch (Exception)
        {
            // an exception must not cross into the engine's threads; stopping is the safe answer,
            // and the page shows that it stopped
            return 0;
        }
    }

    private static void Check(int hresult, string action)
    {
        if (hresult < 0)
        {
            throw new EngineException(action, hresult);
        }
    }

    private static List<Format> LoadFormats()
    {
        var formats = new List<Format>();
        for (uint i = 0; i < skyggn_format_count(); i++)
        {
            NativeFormat* format = skyggn_format_at(i);
            formats.Add(new Format(new string(format->Extension), format->Category, format->Recommended != 0));
        }
        return formats;
    }

    private static Dictionary<string, Setting> LoadSettings()
    {
        var settings = new Dictionary<string, Setting>();
        for (uint i = 0; i < skyggn_setting_count(); i++)
        {
            NativeSetting* setting = skyggn_setting_at(i);
            var name = new string(setting->Name);
            settings[name] = new Setting(name, setting->Default, setting->Min, setting->Max);
        }
        return settings;
    }

    [LibraryImport(Library)]
    private static partial char* skyggn_version();

    [LibraryImport(Library)]
    private static partial uint skyggn_format_count();

    [LibraryImport(Library)]
    private static partial NativeFormat* skyggn_format_at(uint index);

    [LibraryImport(Library)]
    private static partial uint skyggn_setting_count();

    [LibraryImport(Library)]
    private static partial NativeSetting* skyggn_setting_at(uint index);

    [LibraryImport(Library, StringMarshalling = StringMarshalling.Utf16)]
    private static partial char* skyggn_setting_description(string name, string language);

    [LibraryImport(Library, StringMarshalling = StringMarshalling.Utf16)]
    private static partial uint skyggn_setting_get(string name);

    [LibraryImport(Library, StringMarshalling = StringMarshalling.Utf16)]
    private static partial int skyggn_setting_set(string name, uint value);

    [LibraryImport(Library, StringMarshalling = StringMarshalling.Utf16)]
    private static partial int skyggn_thumbnail_pixels(string path, uint size, uint* pixels, uint capacity,
        out uint width, out uint height);

    [LibraryImport(Library)]
    private static partial int skyggn_is_registered(Scope scope);

    [LibraryImport(Library)]
    private static partial int skyggn_install(Scope scope);

    [LibraryImport(Library)]
    private static partial int skyggn_register_server(Scope scope);

    [LibraryImport(Library)]
    private static partial int skyggn_unregister_server(Scope scope);

    [LibraryImport(Library, StringMarshalling = StringMarshalling.Utf16)]
    private static partial int skyggn_set_handled(Scope scope, string extension, int handled);

    [LibraryImport(Library, StringMarshalling = StringMarshalling.Utf16)]
    private static partial int skyggn_is_handled(Scope scope, string extension);

    [LibraryImport(Library, StringMarshalling = StringMarshalling.Utf16)]
    private static partial Handler skyggn_effective_handler(string extension);

    [LibraryImport(Library)]
    private static partial int skyggn_repair(Scope scope, out uint removed);

    [LibraryImport(Library)]
    private static partial int skyggn_count_dead(Scope scope, out uint count);

    [LibraryImport(Library)]
    private static partial int skyggn_system_details_taken();

    [LibraryImport(Library, StringMarshalling = StringMarshalling.Utf16)]
    private static partial int skyggn_prepare_folder(string folder, int recursive, int force,
        delegate* unmanaged[Stdcall]<IntPtr, uint, uint, char*, int> progress, IntPtr context,
        out NativePreparation result);

    [LibraryImport(Library)]
    private static partial void skyggn_notify_shell();
}
