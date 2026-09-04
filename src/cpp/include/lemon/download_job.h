#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <abstraction/job/store.h>

namespace lemon {
namespace download {

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

    // Whether a partial already on disk may be continued. Crosses the boundary
    // rather than being decided by whoever fetches, or the same download would
    // resume or restart depending on which provider answered.
    bool may_resume = true;
};

// Holds the lease for one transfer and writes the checkpoint a successor would
// need. Every operation fails soft: a job store that cannot be opened or
// written must never stop bytes from moving.
class Tracker {
public:
    explicit Tracker(const Context& ctx);
    ~Tracker();

    Tracker(const Tracker&) = delete;
    Tracker& operator=(const Tracker&) = delete;

    bool active() const;

    // How many leading bytes of the partial file a predecessor proved. The
    // transport resumes from the file's own size, which is never larger.
    std::int64_t verified_prefix() const;

    // Checkpoints and renews the lease on bytes OR elapsed time, whichever
    // comes first — a byte threshold alone renews nothing on a slow link,
    // which is where the long transfers are.
    void observe(std::size_t downloaded, std::size_t total);

    // Claims a job whose previous owner let its lease lapse. A resume usually
    // begins while a killed owner's lease is still running, and giving up then
    // means the bytes are fetched and nothing is recorded.
    void take_over_if_free();

    // The bytes are verified and in place, but nobody has taken delivery yet.
    void mark_transferred(std::int64_t bytes);

    void mark_failed(const std::string& message);

private:
    struct State;
    std::unique_ptr<State> state_;
};

// Rooted under the Lemonade cache directory. Null when it could not be opened.
abstraction::job::FileStore* store();

// Reconcile every unheld download job with what is on disk, so a transfer
// interrupted by a close, a crash or a power cut continues from the bytes
// already fetched. Reclaiming is the mechanism; handing off is an optimisation
// a killed process never gets to perform.
void adopt_orphans();

// Close out transfers that finished while nobody was here to say so, and
// return what was delivered.
//
// TRANSFERRED means the bytes arrived and were proven; COMPLETE means somebody
// said "I have them". A TRANSFERRED job is excluded from the orphan sweep
// because adopting one would re-download a finished file, so without this it
// stays recorded forever at 100% with a resume button and nothing to fetch.
std::vector<abstraction::job::Record> take_delivery();

// Say what should happen to every unfinished transfer in a download, and
// return how many records were asked.
//
// The transfer is often not in this process — it may be on a NAS, moving bytes
// while every application that asked is closed — so writing a terminal state
// locally leaves the two disagreeing. Intent is the one field that needs no
// lease, precisely because whoever wants a job stopped is rarely the process
// doing it. This stops nothing by itself: the owner honours the intent at its
// next checkpoint, which is what makes the request survive a process, a
// machine and a reboot.
int intend(const std::string& group_id, const std::string& want, const std::string& by);

std::vector<abstraction::job::Record> unfinished_jobs();

std::string spec_string(const abstraction::job::Record& r, const char* key);
std::int64_t spec_int(const abstraction::job::Record& r, const char* key);

// Hand this transfer to the system downloader, if this machine has one.
//
// Not a speed optimisation: the difference is that the supervisor keeps moving
// bytes when Lemonade is closed, asleep or crashed, and may pass the work
// further on to something always-on.
//
// BLOCKS, so download_file's contract does not change: when this returns true
// the file is where the caller asked for it. Returns false when there is no
// system downloader, and the caller then downloads for itself exactly as
// before — the ordinary case, not an error path.
bool offload(const Context& ctx,
             const std::function<bool(std::size_t, std::size_t)>& on_progress,
             std::string& error_out);

// Cheap: two file reads.
bool offload_available();

}  // namespace download
}  // namespace lemon
