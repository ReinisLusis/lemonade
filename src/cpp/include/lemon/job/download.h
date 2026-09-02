#pragma once

// The download layer: the one place that knows what a `kind: "download"` job
// means. The job layer underneath stores its spec and checkpoint without
// understanding either, which is what lets this file grow mirrors, chunk
// manifests or webseeds without changing the record Go, Python and C++ all
// have to agree about.
//
// Nothing here replaces the transport. libcurl keeps moving the bytes exactly
// as before; this gives the transfer an identity that outlives the process
// doing it, so a download interrupted by a close, a crash or a reboot is
// resumed rather than restarted.

#include <cstddef>
#include <cstdint>
#include <functional>
#include <lemon/job/store.h>
#include <memory>
#include <string>
#include <vector>

namespace lemon {
namespace download {

// The job kind this layer understands. A reader that meets a kind it does not
// know leaves that job alone rather than guessing at its spec.
constexpr const char* kKind = "download";

// What the transport knows about one transfer at the moment it starts.
struct Context {
    std::string url;
    std::string final_path;
    std::string partial_path;
    std::string digest;  // "sha256:<hex>" or empty when the caller cannot say
    std::int64_t size = 0;

    // Which /downloads row this file belongs to, so a restarted server can
    // rebuild that list from disk. Empty for transfers that are not part of a
    // user-visible download.
    std::string group_id;
    std::string display_name;
    std::string file;
    int file_index = -1;
    int total_files = 0;

    // Whether a partial already on disk may be continued.
    //
    // This has to cross the boundary or the provider decides it, and then the
    // same download resumes or restarts depending on whether a supervisor
    // happens to be running. A caller that says "start clean" means it however
    // the bytes are fetched -- backend archives say exactly that, and an
    // abstraction whose semantics depend on who answers is not one.
    bool may_resume = true;
};

// Holds the lease for one transfer, and writes the checkpoint a successor would
// need. Every operation fails soft: a job store that cannot be opened or
// written must never stop bytes from moving, because the downloader worked
// before this layer existed and has to keep working when it is broken.
class Tracker {
public:
    explicit Tracker(const Context& ctx);
    ~Tracker();

    Tracker(const Tracker&) = delete;
    Tracker& operator=(const Tracker&) = delete;

    bool active() const;

    // How many leading bytes of the partial file a predecessor proved. The
    // transport resumes from the file's own size, which is never larger; this
    // is what a successor in another process would be told.
    std::int64_t verified_prefix() const;

    // Called from the progress callback. Checkpoints and renews the lease on
    // bytes OR elapsed time, whichever comes first — a byte threshold alone
    // saves nothing on a slow link, which is where the long transfers are.
    void observe(std::size_t downloaded, std::size_t total);

    // Claims a job whose previous owner has let its lease lapse. A resume
    // almost always begins while the killed owner's lease is still running, and
    // giving up then means the bytes are fetched and nothing is recorded.
    void take_over_if_free();

    // The bytes are verified and in place, but nobody has taken delivery yet.
    void mark_transferred(std::int64_t bytes);

    void mark_failed(const std::string& message);

private:
    struct State;
    std::unique_ptr<State> state_;
};

// The store every download job lives in, rooted under the Lemonade cache
// directory. Returns nullptr when it could not be opened.
job::FileStore* store();

// At start-up, reconcile every download job nobody holds with what is actually
// on disk. This is the payoff: Lemonade closed mid-download, reopened, and the
// transfer continues from the bytes already fetched instead of restarting.
//
// Reclaiming is the mechanism and handing off is only an optimisation — a
// process that is killed, or a machine that loses power, never gets to hand
// anything over.
void adopt_orphans();

// Close out transfers that finished while nobody was here to say so.
//
// TRANSFERRED means the bytes arrived and were proven; COMPLETE means somebody
// said "I have them". The two-phase ending is the only way to express "this
// finished while your application was closed", and nothing was performing the
// second half. Every other state survives its owner dying because the lease
// lapses and a sweep adopts it -- but adopting a TRANSFERRED job would
// re-download a finished file, so it is deliberately excluded from the sweep,
// and the exclusion that prevents that loop is the same one that stranded the
// record. A download manager showed the result: rows at 100% labelled paused,
// offering a resume button with nothing left to fetch.
//
// Returns what it took delivery of, so a client can tell somebody. That list is
// exactly the set of downloads that finished while they were not watching.
std::vector<job::Record> take_delivery();

// Say what should happen to every unfinished transfer in a download.
//
// # Why this has to exist
//
// A pause clicked here used to stop this process and nothing else. The transfer
// is often not in this process at all -- it is in BITS, or on a NAS, moving
// bytes while every application that asked is closed -- and the only thing the
// UI could do was mark its own copy of the job "paused" and write a terminal
// CANCEL to the shared record. Measured consequence: the UI said paused at 38%,
// the record said cancelled, and the NAS fetched the remaining 2 GB and finished.
// Three participants sharing one store, three answers.
//
// Intent is the field that makes this expressible, and it is the ONE write that
// needs no lease -- precisely because whoever wants a job stopped is almost
// never the process doing it. Schema 4 added it for this exact case; nothing
// here had ever called it.
//
// A Lemonade download is a GROUP of files, so this asks about all of them.
// Returns how many records were asked.
//
// It does not stop anything by itself, and must not: the owner honours the
// intent at its next checkpoint, which is what makes the request work across a
// process, a machine, and a reboot.
int intend(const std::string& group_id, const std::string& want, const std::string& by);

// Every download job that is not yet finished, for rebuilding the user-visible
// download list after a restart.
std::vector<job::Record> unfinished_jobs();

// The parts of a download spec the server needs to describe a row.
std::string spec_string(const job::Record& r, const char* key);
std::int64_t spec_int(const job::Record& r, const char* key);

// Hand this transfer to the system downloader, if this machine has one.
//
// The point is not speed. libcurl here and a supervisor there move bytes at the
// same rate; the difference is that the supervisor keeps moving them when
// Lemonade is closed, asleep or crashed, and may itself pass the work further
// on to something always-on. Which is exactly the substitution this whole layer
// exists for: the caller asked for bytes and never chose a provider.
//
// Returns false when there is no system downloader, and then the caller
// downloads for itself exactly as before. That fallback is not an error path;
// it is the ordinary case on a machine nobody has set one up on.
//
// It BLOCKS until the transfer finishes, so download_file's contract does not
// change: when this returns true the file is where the caller asked for it.
// What changed is who fetched it. Closing Lemonade mid-transfer no longer stops
// the bytes -- the supervisor carries on, and the next start adopts the result
// instead of beginning again.
bool offload(const Context& ctx,
             const std::function<bool(std::size_t, std::size_t)>& on_progress,
             std::string& error_out);

// Whether a system downloader is available, for a status line. Cheap: two file
// reads.
bool offload_available();


}  // namespace download
}  // namespace lemon
