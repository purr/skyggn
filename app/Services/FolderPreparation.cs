namespace Skyggn.Services;

// making a folder's thumbnails ahead of time (Engine.PrepareFolder), one folder at a time and apart
// from any page: the pages are made anew each time they are shown, and a thumbnail refresh has to stop
// it first. used on the ui thread.
public static class FolderPreparation
{
    // how far a job is, as the engine last said (skyggn_progress): while it looks for files, Done is 0
    // and File the folder it reads; then Done of Total files, File the one just done
    public sealed record Snapshot(uint Done, uint Total, string File);

    public sealed class Job
    {
        public required string Folder { get; init; }

        public required bool Recursive { get; init; }

        // the latest progress, replaced whole by the engine's calls, so a reader never sees half of one
        public Snapshot? Latest => _latest;

        public bool Stopping => _stop;

        // the counts once it ended; faulted when it could not run
        public Task<Preparation> Done { get; internal set; } = null!;  // set by Start, right after

        internal volatile Snapshot? _latest;
        internal volatile bool _stop;
    }

    // the job running now, if one is
    public static Job? Current { get; private set; }

    // the last job that ended, so a page shown afterwards can still say how it went
    public static Job? Finished { get; private set; }

    public static Job Start(string folder, bool recursive)
    {
        var job = new Job { Folder = folder, Recursive = recursive };
        Current = job;
        job.Done = RunAsync(job);
        return job;
    }

    // the files being made finish first
    public static void Stop()
    {
        if (Current is { } job)
        {
            job._stop = true;
        }
    }

    // stops the job running now and waits for it to end, however it ends
    public static async Task StopAsync()
    {
        if (Current is not { } job)
        {
            return;
        }
        job._stop = true;
        try
        {
            await job.Done;
        }
        catch (Exception)
        {
            // the job's own failure is shown where the job is followed; whoever waits here only needs it over
        }
    }

    private static async Task<Preparation> RunAsync(Job job)
    {
        try
        {
            // a refresh under way has file explorer closed and the cache files going: it ends first
            await ThumbnailCache.WaitAsync();
            return await Task.Run(() => Engine.PrepareFolder(job.Folder, job.Recursive, (done, total, file) =>
            {
                job._latest = new Snapshot(done, total, file);
                return !job._stop;
            }));
        }
        finally
        {
            Current = null;
            Finished = job;
        }
    }
}
