# The job store, and why /downloads is derived from it

Lemonade tracks a download in three places and none of them is the disk.

`HttpClient::download_file` holds progress in the server's memory.
`DownloadTracker` in the renderer holds a `Map<string, DownloadItem>` in the
browser's memory. The partial files hold the only durable truth, and nothing
reads them.

The consequences are the ones users report. Reload the app and the download list
empties while the transfer continues. Restart the server and unfinished
transfers become invisible even though their bytes are still on disk. A pull
started from the CLI never appears in the app at all, because the app's list is
its own memory rather than a view of anything.

## What changed

`/downloads` rows are now derived from job records on disk, and a record is the
only thing that persists. The in-memory paths still serve live progress for a
transfer this process is running — that part was never wrong — but anything not
running is read from the store rather than remembered.

    src/cpp/job/          record and store: the cross-language contract
    src/cpp/server/utils/download_job.cpp   Lemonade's use of it

`Server::persisted_download_rows()` builds rows from `unfinished_jobs()`, and
`download::adopt_orphans()` runs at startup so work left by a previous run is
reconciled against what is actually on disk before anything can ask for one of
those files.

Rows for work nobody is moving report `status: "paused"` rather than
`"downloading"`. It is unfinished and resumable, which is what a user needs to
know, and claiming otherwise would put a progress bar on a transfer with no
process behind it.

## Why the record looks like that

The format is not Lemonade's. It is a contract shared with implementations in Go
and Python, and the C++ here is the third. That is deliberate: the value of a
record on disk is that a supervisor, a CLI, a NAS and this server can all act on
the same work without any of them talking to each other, and a format only one
program can read gives none of that.

Two consequences show up in this code and are worth knowing before changing it:

- **The schema is checked on read.** A record from a newer writer is refused
  rather than partially understood. Silently ignoring an unknown field is how two
  implementations start disagreeing about the same job.
- **Timestamps are six fractional digits, always.** Go's RFC3339Nano trimmed
  trailing zeros for a while and Python did not, so the same instant serialised
  two ways. Only a test that ran both found it.

The lease rules matter for the same reason. A job carries an epoch that rises on
every claim, so a process that was suspended past its own lease cannot write over
a successor's work when it wakes. Claiming is a file created with `O_EXCL`, which
is exclusive on NTFS, on ext4, and — measured — over SMB to a Synology.

## Running the tests

    cmake --build --preset vs18 --target test_job_record
    build\Release\test_job_record.exe

46 assertions: the exact bytes written, the refusals, epoch fencing, and two
rules that only became visible once three implementations existed —
a transferred job is not an orphan, and releasing a delegated job does not
demote it to pending. Both were wrong in all three implementations at first.
