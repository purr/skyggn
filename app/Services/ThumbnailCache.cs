using System.ComponentModel;
using System.Diagnostics;
using System.Runtime.InteropServices;

namespace Skyggn.Services;

// windows keeps every thumbnail it made in thumbcache_*.db files, held open by file explorer. to
// clear them, the restart manager closes explorer cleanly, the files are deleted, and the restart
// manager starts explorer again with its windows. the icons windows drew for file types
// (iconcache_*.db) go too: it draws an app's file type icons through the thumbnail handlers, so they
// can carry a look skyggn gave them.
public static partial class ThumbnailCache
{
    private const int CchRmSessionKey = 32;
    private const uint RmForceShutdown = 1;
    private const int RmExplorer = 4;  // RM_APP_TYPE of file explorer
    private const int ErrorMoreData = 234;

    // RM_PROCESS_INFO, with its strings as fixed buffers of utf-16 units so the struct stays blittable
    [StructLayout(LayoutKind.Sequential)]
    private unsafe struct RmProcessInfo
    {
        public uint ProcessId;
        public uint StartTimeLow;
        public uint StartTimeHigh;
        public fixed ushort AppName[256];
        public fixed ushort ServiceShortName[64];
        public int ApplicationType;
        public uint AppStatus;
        public uint TsSessionId;
        public int Restartable;
    }

    // RM_UNIQUE_PROCESS: a process id and its start time, which together name one process
    [StructLayout(LayoutKind.Sequential)]
    private struct RmUniqueProcess
    {
        public uint ProcessId;
        public uint StartTimeLow;
        public uint StartTimeHigh;
    }

    // one clear at a time: a second one, from another page or a second click, would close the
    // explorer the first had just started again
    private static Task? s_clearing;

    // the clear under way, if any; the app waits for it before it closes, or explorer stays closed
    public static Task Clearing => s_clearing ?? Task.CompletedTask;

    // waits for the clear under way to end, however it ends
    public static async Task WaitAsync()
    {
        try
        {
            await Clearing;
        }
        catch (Exception)
        {
            // the page that started the clear shows its error; whoever waits here only needs it over
        }
    }

    // called on the ui thread only, so the check and the start cannot interleave
    public static Task ClearAsync()
    {
        if (s_clearing is { IsCompleted: false } running)
        {
            return running;
        }
        return s_clearing = Task.Run(Clear);
    }

    private static unsafe void Clear()
    {
        var folder = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),
            "Microsoft", "Windows", "Explorer");
        var files = Directory.GetFiles(folder, "thumbcache_*.db");
        // not registered with the restart manager: programs that show file icons (a browser, a
        // screenshot tool) keep these open, and would have to close. they let the files be deleted,
        // which takes effect once they let go of them.
        var icons = Directory.GetFiles(folder, "iconcache_*.db");
        if (files.Length == 0)
        {
            return;
        }

        char* key = stackalloc char[CchRmSessionKey + 1];
        Check(RmStartSession(out uint session, 0, key), "CacheSession");
        try
        {
            var names = files.Select(Marshal.StringToHGlobalUni).ToArray();
            var explorers = Explorers();
            try
            {
                fixed (IntPtr* list = names)
                fixed (RmUniqueProcess* applications = explorers)
                {
                    Check(RmRegisterResources(session, (uint)names.Length, list, (uint)explorers.Length, applications,
                        0, null), "CacheRegister");
                }
            }
            finally
            {
                foreach (var name in names)
                {
                    Marshal.FreeHGlobal(name);
                }
            }

            // only file explorer is closed. the shutdown is forced, so another program holding the
            // cache (one showing a file dialog) would be ended with its unsaved work: it is named
            // instead, for the user to close
            var others = OtherHolders(session);
            if (others.Count > 0)
            {
                throw new IOException(Text.Format("CacheHeldBy", string.Join(", ", others)));
            }

            var shutdown = RmShutdown(session, RmForceShutdown, null);
            var locked = new List<string>();
            int restart;
            try
            {
                if (shutdown == 0)
                {
                    foreach (var file in files.Concat(icons))
                    {
                        try
                        {
                            File.Delete(file);
                        }
                        catch (Exception error) when (error is IOException or UnauthorizedAccessException)
                        {
                            locked.Add(Path.GetFileName(file));
                        }
                    }
                }
            }
            finally
            {
                // explorer comes back whatever happened once the shutdown began: one that failed part
                // way may have closed it too
                restart = RmRestart(session, 0, null);
                if (restart != 0)
                {
                    StartExplorerIfGone();
                }
            }
            Check(shutdown, "CacheClose");
            Check(restart, "CacheRestart");
            if (locked.Count > 0)
            {
                throw new IOException(Text.Format("CacheInUse", string.Join(", ", locked)));
            }
        }
        finally
        {
            RmEndSession(session);
        }
    }

    // every file explorer of this session, named to the restart manager so it closes and restarts
    // them whether or not they have a cache file open at that moment: one that had none open went on
    // running, and drew the thumbnails and icons it keeps in memory again after the refresh
    private static RmUniqueProcess[] Explorers()
    {
        using var current = Process.GetCurrentProcess();
        var found = new List<RmUniqueProcess>();
        foreach (var explorer in Process.GetProcessesByName("explorer"))
        {
            using (explorer)
            {
                try
                {
                    if (explorer.SessionId == current.SessionId &&
                        GetProcessTimes(explorer.Handle, out long created, out _, out _, out _))
                    {
                        found.Add(new RmUniqueProcess
                        {
                            ProcessId = (uint)explorer.Id,
                            StartTimeLow = (uint)created,
                            StartTimeHigh = (uint)(created >> 32),
                        });
                    }
                }
                catch (Exception error) when (error is Win32Exception or InvalidOperationException)
                {
                    // it ended meanwhile, or this account may not open it (one run as administrator):
                    // the restart manager still closes it if it has a cache file open
                }
            }
        }
        return found.ToArray();
    }

    // the names of the programs other than file explorer that have the registered files open
    private static unsafe List<string> OtherHolders(uint session)
    {
        uint count = 0;
        for (;;)
        {
            var infos = new RmProcessInfo[count];
            uint reasons = 0;
            int error;
            fixed (RmProcessInfo* list = infos)
            {
                error = RmGetList(session, out uint needed, ref count, count == 0 ? null : list, ref reasons);
                if (error == ErrorMoreData)
                {
                    count = needed;  // the list grew; ask again with room for it
                    continue;
                }
            }
            Check(error, "CacheList");
            var others = new List<string>();
            for (var i = 0; i < count; i++)
            {
                if (infos[i].ApplicationType != RmExplorer)
                {
                    fixed (ushort* name = infos[i].AppName)
                    {
                        others.Add(new string((char*)name));
                    }
                }
            }
            return others.Distinct().ToList();
        }
    }

    // the last resort when the restart manager could not start explorer again: without it, the
    // user has no taskbar and no desktop until they start it from task manager
    private static void StartExplorerIfGone()
    {
        using var current = Process.GetCurrentProcess();
        var explorers = Process.GetProcessesByName("explorer");
        var running = explorers.Any(process => process.SessionId == current.SessionId);
        foreach (var explorer in explorers)
        {
            explorer.Dispose();
        }
        if (!running)
        {
            Process.Start(new ProcessStartInfo("explorer.exe") { UseShellExecute = true })?.Dispose();
        }
    }

    // `action` names the text that says what was being done
    private static void Check(int error, string action)
    {
        if (error != 0)
        {
            throw new InvalidOperationException(
                Text.Format("ActionFailed", Text.Get(action), Marshal.GetPInvokeErrorMessage(error)));
        }
    }

    [LibraryImport("rstrtmgr.dll")]
    private static unsafe partial int RmStartSession(out uint session, uint flags, char* key);

    [LibraryImport("rstrtmgr.dll")]
    private static unsafe partial int RmRegisterResources(uint session, uint fileCount, IntPtr* files,
        uint applicationCount, RmUniqueProcess* applications, uint serviceCount, IntPtr* services);

    [LibraryImport("kernel32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static partial bool GetProcessTimes(IntPtr process, out long creation, out long exit, out long kernel,
        out long user);

    [LibraryImport("rstrtmgr.dll")]
    private static unsafe partial int RmGetList(uint session, out uint needed, ref uint count, RmProcessInfo* affected,
        ref uint rebootReasons);

    [LibraryImport("rstrtmgr.dll")]
    private static unsafe partial int RmShutdown(uint session, uint flags, void* progress);

    [LibraryImport("rstrtmgr.dll")]
    private static unsafe partial int RmRestart(uint session, uint reserved, void* progress);

    [LibraryImport("rstrtmgr.dll")]
    private static partial int RmEndSession(uint session);
}
