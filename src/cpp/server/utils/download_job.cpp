#include <lemon/job/download.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <lemon/job/discovery.h>
#include <lemon/utils/aixlog.hpp>
#include <lemon/utils/path_utils.h>
#include <memory>
#include <mutex>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace lemon {
namespace download {

namespace {

using job::Json;
using job::Record;

// The lease outlives a stall but not a crash by much: long enough that a slow
// link does not lose ownership between checkpoints, short enough that a killed
// process's work is adoptable while the user is still watching.
constexpr auto kLeaseTtl = std::chrono::seconds(60);
constexpr auto kCheckpointInterval = std::chrono::seconds(5);
constexpr std::int64_t kCheckpointBytes = 8 * 1024 * 1024;

std::string owner_name() {
#ifdef _WIN32
    const auto pid = static_cast<long long>(::GetCurrentProcessId());
#else
    const auto pid = static_cast<long long>(::getpid());
#endif
    return "lemonade-" + std::to_string(pid);
}

std::int64_t file_size_or_zero(const std::string& path) {
    std::error_code ec;
    const auto size = fs::file_size(utils::path_from_utf8(path), ec);
    return ec ? 0 : static_cast<std::int64_t>(size);
}

bool file_exists(const std::string& path) {
    std::error_code ec;
    return fs::exists(utils::path_from_utf8(path), ec);
}

const Json* spec_at(const Record& r, const char* dotted) {
    const Json* node = &r.spec;
    std::string path(dotted);
    std::size_t start = 0;
    while (start <= path.size()) {
        const auto dot = path.find('.', start);
        const std::string key =
            path.substr(start, dot == std::string::npos ? std::string::npos : dot - start);
        if (!node->is_object() || !node->contains(key)) {
            return nullptr;
        }
        node = &node->at(key);
        if (dot == std::string::npos) {
            return node;
        }
        start = dot + 1;
    }
    return nullptr;
}

}  // namespace

// The record says a digest is "sha256:<hex>". Writing it bare is a contract
// violation that nothing here catches: the job layer will not parse a spec —
// that opacity is exactly what lets download evolve without a schema change in
// three languages — so a bare digest travels intact to a reader that builds
// "sha256:" + hex and compares strings.
//
// It cost a real 1.5 GB download. The error read "got sha256:1fc70f… want
// 1fc70f…", the same digest twice, and the mismatch path deletes the partial,
// so correct bytes were thrown away and fetched a second time.
std::string qualified_digest(const std::string& d) {
    if (d.empty() || d.find(':') != std::string::npos) {
        return d;
    }
    return "sha256:" + d;
}

std::string spec_string(const Record& r, const char* key) {
    const Json* node = spec_at(r, key);
    return (node != nullptr && node->is_string()) ? node->get<std::string>() : std::string();
}

std::int64_t spec_int(const Record& r, const char* key) {
    const Json* node = spec_at(r, key);
    return (node != nullptr && node->is_number_integer()) ? node->get<std::int64_t>() : 0;
}

job::FileStore* store() {
    static std::mutex mutex;
    static bool tried = false;
    static std::unique_ptr<job::FileStore> instance;

    std::lock_guard<std::mutex> lock(mutex);
    if (!tried) {
        tried = true;
        // The machine's store if this machine has one, and only then a private
        // one under the cache.
        //
        // A private store was the original mistake. It made a download two
        // records -- ours and the supervisor's -- for one piece of work, so
        // handing a transfer over meant COPYING a job between stores and
        // carrying its checkpoint across by hand. Two records for one thing is
        // a reconciliation problem nobody asked for, and it showed: moving the
        // cache directory moved the store with it, and every in-flight download
        // became invisible while its bytes sat on disk.
        //
        // With one store there is nothing to copy. Handing over stops being a
        // transfer of ownership and becomes what it always should have been:
        // not picking the job up yourself.
        std::string root = job::machine_store();
        if (root.empty()) {
            root = utils::path_to_utf8(utils::path_from_utf8(utils::get_cache_dir()) / "jobs");
        }
        try {
            instance = std::make_unique<job::FileStore>(root);
        } catch (const std::exception& e) {
            LOG(WARNING, "DownloadJob") << "job store unavailable, downloads will not be "
                                           "resumable across restarts: "
                                        << e.what() << std::endl;
        }
    }
    return instance.get();
}

struct Tracker::State {
    std::string id;
    std::int64_t epoch = 0;
    // Set when a claim was refused because somebody else holds the lease. If
    // that somebody is gone, their lease lapses within its TTL and the next
    // attempt succeeds — see observe().
    bool retry_claim = false;
    std::chrono::steady_clock::time_point next_claim_attempt;
    std::int64_t verified_prefix = 0;
    std::int64_t last_checkpoint_bytes = 0;
    std::chrono::steady_clock::time_point last_checkpoint;
    bool holding = false;
};

Tracker::Tracker(const Context& ctx) {
    job::FileStore* jobs = store();
    if (jobs == nullptr || ctx.final_path.empty()) {
        return;
    }

    // Outside the try so the LeaseHeld handler can remember which job it was
    // refused, and keep asking for it.
    std::string id;
    try {
        // A download is identified by where its bytes are going. Two runs
        // fetching the same file are the same job, which is what makes a
        // restart resume rather than start over.
        for (const Record& candidate : jobs->list()) {
            if (candidate.kind == kKind && !candidate.terminal() &&
                spec_string(candidate, "sink.final") == ctx.final_path) {
                id = candidate.id;
            }
        }

        if (id.empty()) {
            Record r;
            r.kind = kKind;
            r.progress.total = ctx.size;
            Json spec = Json::object();
            Json artifact = Json::object();
            if (!ctx.digest.empty()) {
                artifact["digest"] = qualified_digest(ctx.digest);
            }
            if (ctx.size > 0) {
                artifact["size"] = ctx.size;
            }
            spec["artifact"] = std::move(artifact);
            Json source = Json::object();
            source["scheme"] = ctx.url.rfind("http://", 0) == 0 ? "http" : "https";
            source["locator"] = ctx.url;
            spec["sources"] = Json::array({std::move(source)});
            Json sink = Json::object();
            sink["partial"] = ctx.partial_path;
            sink["final"] = ctx.final_path;
            spec["sink"] = std::move(sink);
            if (!ctx.group_id.empty()) {
                spec["group_id"] = ctx.group_id;
                spec["display_name"] = ctx.display_name;
                spec["file"] = ctx.file;
                spec["file_index"] = ctx.file_index;
                spec["total_files"] = ctx.total_files;
            }
            r.spec = std::move(spec);
            id = jobs->submit(std::move(r));
        }

        const Record claimed = jobs->claim(id, owner_name(), kLeaseTtl);
        state_ = std::unique_ptr<State>(new State());
        state_->id = id;
        state_->epoch = claimed.lease.epoch;
        state_->holding = true;
        // Dated back one interval so the first progress event checkpoints
        // immediately: until it does, an observer in another process cannot
        // tell a claimed job from a stalled one.
        state_->last_checkpoint = std::chrono::steady_clock::now() - kCheckpointInterval;
        if (claimed.checkpoint && claimed.checkpoint->is_object() &&
            claimed.checkpoint->contains("verified_prefix")) {
            const Json& prefix = claimed.checkpoint->at("verified_prefix");
            if (prefix.is_number_integer()) {
                state_->verified_prefix = prefix.get<std::int64_t>();
            }
        }
    } catch (const job::LeaseHeld& e) {
        // Somebody holds the lease. Let the transfer carry on — refusing here
        // would be a behaviour change — but KEEP TRYING, because the holder is
        // very often gone.
        //
        // Resuming an interrupted download is the ordinary case: a process was
        // killed, its lease has seconds left to run, and the obvious next thing
        // a person does is start the download again. Giving up here meant the
        // bytes were fetched and NOTHING was recorded — a real 397 MB file
        // arrived complete while the store still called it a 95% orphan owned by
        // a dead pid, which is exactly the state a supervisor would try to
        // "fix" by downloading it again.
        //
        // So retry from observe(). Within one lease TTL the dead owner's claim
        // lapses and this one succeeds; if the holder is genuinely alive it
        // keeps its job and this stays a passive observer, which is what the
        // original comment was right about.
        LOG(INFO, "DownloadJob") << "another owner holds this download, will keep trying: "
                                 << e.what() << std::endl;
        state_ = std::unique_ptr<State>(new State());
        state_->id = id;
        state_->retry_claim = true;
        state_->next_claim_attempt = std::chrono::steady_clock::now() + kCheckpointInterval;
    } catch (const std::exception& e) {
        LOG(WARNING, "DownloadJob") << "could not track this download: " << e.what() << std::endl;
    }
}

Tracker::~Tracker() {
    if (!state_ || !state_->holding) {
        return;
    }
    job::FileStore* jobs = store();
    if (jobs == nullptr) {
        return;
    }
    try {
        // A polite exit frees the job in seconds instead of after the expiry.
        // Nothing depends on it — that is the point of leasing by epoch.
        jobs->release(state_->id, state_->epoch);
    } catch (const std::exception&) {
    }
}

bool Tracker::active() const { return state_ && state_->holding; }

std::int64_t Tracker::verified_prefix() const { return state_ ? state_->verified_prefix : 0; }

// take_over_if_free claims a job whose previous owner has let its lease lapse.
//
// Rate limited, because the alternative is a store read on every progress
// callback. When it succeeds the dead owner's checkpoint is adopted, so what
// this process records continues that history rather than starting a new one.
void Tracker::take_over_if_free() {
    const auto now = std::chrono::steady_clock::now();
    if (now < state_->next_claim_attempt) {
        return;
    }
    state_->next_claim_attempt = now + kCheckpointInterval;

    job::FileStore* jobs = store();
    if (jobs == nullptr || state_->id.empty()) {
        return;
    }
    try {
        const Record claimed = jobs->claim(state_->id, owner_name(), kLeaseTtl);
        state_->epoch = claimed.lease.epoch;
        state_->holding = true;
        state_->retry_claim = false;
        state_->last_checkpoint = now - kCheckpointInterval;
        if (claimed.checkpoint && claimed.checkpoint->is_object() &&
            claimed.checkpoint->contains("verified_prefix")) {
            const Json& prefix = claimed.checkpoint->at("verified_prefix");
            if (prefix.is_number_integer()) {
                state_->verified_prefix = prefix.get<std::int64_t>();
            }
        }
        LOG(INFO, "DownloadJob") << "took over " << state_->id
                                 << " from an owner that had gone" << std::endl;
    } catch (const std::exception&) {
        // Still held, or unreadable. Try again at the next checkpoint.
    }
}

void Tracker::observe(std::size_t downloaded, std::size_t total) {
    if (state_ && !state_->holding && state_->retry_claim) {
        take_over_if_free();
    }
    if (!active()) {
        return;
    }
    const auto now = std::chrono::steady_clock::now();
    const auto done = static_cast<std::int64_t>(downloaded);
    const bool enough_bytes = done - state_->last_checkpoint_bytes >= kCheckpointBytes;
    const bool enough_time = now - state_->last_checkpoint >= kCheckpointInterval;
    if (!enough_bytes && !enough_time) {
        return;
    }

    job::FileStore* jobs = store();
    if (jobs == nullptr) {
        return;
    }
    try {
        const Record current = jobs->load(state_->id);
        const std::string partial = spec_string(current, "sink.partial");
        // Checkpoint what the filesystem can vouch for, never what the
        // transport says it handed over: the bytes in flight when a process is
        // killed are exactly the ones a successor must not trust.
        const std::int64_t on_disk = file_size_or_zero(partial);
        const std::int64_t proven = (std::min)(done, on_disk);

        jobs->update(state_->id, state_->epoch, [&](Record& r) {
            r.progress.done = done;
            if (total > 0) {
                r.progress.total = static_cast<std::int64_t>(total);
            }
            r.progress.updated_at = job::Clock::now();
            Json checkpoint = Json::object();
            checkpoint["verified_prefix"] = proven;
            r.checkpoint = std::move(checkpoint);
            // Renewing inside the same write keeps one owner's liveness,
            // progress and resume point on a single channel.
            r.lease.expires_at =
                job::Clock::now() + std::chrono::duration_cast<job::TimePoint::duration>(kLeaseTtl);
        });
        state_->last_checkpoint = now;
        state_->last_checkpoint_bytes = done;
    } catch (const job::StaleEpoch& e) {
        // Someone else took the job over. Stop writing rather than fight for
        // it: two owners writing one file is the damage the epoch prevents.
        LOG(WARNING, "DownloadJob") << "lost the lease mid-transfer: " << e.what() << std::endl;
        state_->holding = false;
    } catch (const job::LeaseExpired& e) {
        LOG(WARNING, "DownloadJob") << "lease expired mid-transfer: " << e.what() << std::endl;
        state_->holding = false;
    } catch (const std::exception& e) {
        LOG(WARNING, "DownloadJob") << "checkpoint failed: " << e.what() << std::endl;
    }
}

void Tracker::mark_transferred(std::int64_t bytes) {
    if (!active()) {
        return;
    }
    job::FileStore* jobs = store();
    if (jobs == nullptr) {
        return;
    }
    try {
        jobs->update(state_->id, state_->epoch, [&](Record& r) {
            r.state = job::state::kTransferred;
            if (bytes > 0) {
                r.progress.done = bytes;
            }
            r.progress.updated_at = job::Clock::now();
            Json checkpoint = Json::object();
            checkpoint["verified_prefix"] = bytes > 0 ? bytes : r.progress.done;
            r.checkpoint = std::move(checkpoint);
            r.error.clear();
        });
    } catch (const std::exception& e) {
        LOG(WARNING, "DownloadJob") << "could not record completion: " << e.what() << std::endl;
    }
}

void Tracker::mark_failed(const std::string& message) {
    if (!active()) {
        return;
    }
    job::FileStore* jobs = store();
    if (jobs == nullptr) {
        return;
    }
    try {
        // Not `failed`: the partial file is still on disk and still resumable,
        // and a job nobody can pick up again is how a 40 GB download is lost.
        // The record stays claimable and carries why the last owner stopped.
        jobs->update(state_->id, state_->epoch, [&](Record& r) { r.error = message; });
    } catch (const std::exception& e) {
        LOG(WARNING, "DownloadJob") << "could not record failure: " << e.what() << std::endl;
    }
}

void adopt_orphans() {
    job::FileStore* jobs = store();
    if (jobs == nullptr) {
        return;
    }

    std::vector<Record> orphans;
    try {
        orphans = jobs->orphans();
    } catch (const std::exception& e) {
        LOG(WARNING, "DownloadJob") << "could not scan for orphaned downloads: " << e.what()
                                    << std::endl;
        return;
    }

    int resumable = 0;
    int delivered = 0;
    int abandoned = 0;
    for (const Record& orphan : orphans) {
        if (orphan.kind != kKind) {
            continue;  // a kind this layer does not know is not ours to touch
        }
        const std::string final_path = spec_string(orphan, "sink.final");
        const std::string partial_path = spec_string(orphan, "sink.partial");
        const bool have_final = !final_path.empty() && file_exists(final_path);
        const std::int64_t partial_bytes = file_size_or_zero(partial_path);

        try {
            const Record claimed = jobs->claim(orphan.id, owner_name(), kLeaseTtl);
            jobs->update(orphan.id, claimed.lease.epoch, [&](Record& r) {
                if (have_final) {
                    // The file is where it was meant to end up, so delivery has
                    // in fact been taken; the process that would have said so
                    // is gone.
                    r.state = job::state::kComplete;
                } else if (partial_bytes > 0) {
                    // Leave it claimable. The next transfer of this artifact
                    // finds it, inherits the checkpoint and resumes.
                    r.state = job::state::kPending;
                    r.progress.done = partial_bytes;
                    r.lease.owner.clear();
                    r.lease.expires_at = job::Clock::now();
                } else {
                    r.state = job::state::kCancelled;
                    r.error = "no bytes on disk when adopted";
                }
                r.progress.updated_at = job::Clock::now();
            });
            if (have_final) {
                ++delivered;
            } else if (partial_bytes > 0) {
                ++resumable;
            } else {
                ++abandoned;
            }
        } catch (const std::exception& e) {
            LOG(WARNING, "DownloadJob") << "could not adopt " << orphan.id << ": " << e.what()
                                        << std::endl;
        }
    }

    if (delivered + resumable + abandoned > 0) {
        LOG(INFO, "DownloadJob") << "adopted orphaned downloads: " << resumable << " resumable, "
                                 << delivered << " already delivered, " << abandoned
                                 << " with nothing on disk" << std::endl;
    }
}

std::vector<Record> take_delivery() {
    job::FileStore* jobs = store();
    if (jobs == nullptr) {
        return {};
    }

    std::vector<Record> all;
    try {
        all = jobs->list();
    } catch (const std::exception& e) {
        LOG(WARNING, "DownloadJob") << "could not scan for finished downloads: " << e.what()
                                    << std::endl;
        return {};
    }

    std::vector<Record> delivered;
    for (const Record& r : all) {
        if (r.kind != kKind || r.state != job::state::kTransferred) {
            continue;
        }

        const std::string final_path = spec_string(r, "sink.final");
        const bool have_final = !final_path.empty() && file_exists(final_path);

        if (have_final) {
            // Size is the cheap half of the proof and the half that catches a
            // truncated or replaced file. It does NOT re-hash: the digest was
            // checked when these bytes were proven, and rehashing gigabytes on
            // every start would spend real time re-answering that.
            const std::int64_t want = r.progress.total;
            if (want > 0 && file_size_or_zero(final_path) != want) {
                continue;
            }
        } else if (r.delegated() && !r.delegation->delivered) {
            // A delegate still holds these bytes and they have yet to cross.
            // That is a real pending transition and the one case a person
            // should see, because something still has to happen.
            continue;
        }
        // Otherwise: delivered, and since moved or consumed by whoever wanted
        // it. TRANSFERRED is only ever set after the bytes reach their final
        // path, so a missing file does not mean the delivery failed -- it means
        // it succeeded and something then used the result. A backend zip is
        // downloaded, extracted and deleted, which is correct behaviour that no
        // file-existence test could ever satisfy. Requiring the file to still be
        // there confuses "did this arrive" with "is it still where it landed",
        // and only the first is this layer's business.

        try {
            const Record claimed = jobs->claim(r.id, owner_name(), kLeaseTtl);
            jobs->update(r.id, claimed.lease.epoch, [](Record& rec) {
                rec.state = job::state::kComplete;
                rec.progress.updated_at = job::Clock::now();
                rec.error.clear();
            });
            delivered.push_back(jobs->load(r.id));
        } catch (const job::LeaseHeld&) {
            // Somebody is on it. Not a failure and not worth a warning: the
            // owner that proved these bytes may only just have stopped, and its
            // lease lapses on its own within the TTL. The next start closes the
            // job out.
        } catch (const std::exception& e) {
            LOG(WARNING, "DownloadJob") << "could not take delivery of " << r.id << ": " << e.what()
                                        << std::endl;
        }
    }

    if (!delivered.empty()) {
        LOG(INFO, "DownloadJob") << "took delivery of " << delivered.size()
                                 << " download(s) that finished while nobody was watching"
                                 << std::endl;
    }
    return delivered;
}

int intend(const std::string& group_id, const std::string& want, const std::string& by) {
    job::FileStore* jobs = store();
    if (jobs == nullptr || group_id.empty()) {
        return 0;
    }
    std::vector<Record> all;
    try {
        all = jobs->list();
    } catch (const std::exception& e) {
        LOG(WARNING, "DownloadJob") << "could not read the store to ask for " << want << ": "
                                    << e.what() << std::endl;
        return 0;
    }

    int asked = 0;
    for (const Record& r : all) {
        if (r.kind != kKind || r.terminal()) {
            continue;
        }
        if (spec_string(r, "group_id") != group_id) {
            continue;
        }
        try {
            jobs->set_intent(r.id, want, by);
            ++asked;
        } catch (const std::exception& e) {
            // Worth saying, not worth failing the request: the other files in
            // the group still deserve to be asked.
            LOG(WARNING, "DownloadJob") << "could not ask " << r.id << " to " << want << ": "
                                        << e.what() << std::endl;
        }
    }
    if (asked > 0) {
        LOG(INFO, "DownloadJob") << "asked " << asked << " transfer(s) in " << group_id << " to "
                                 << want << std::endl;
    }
    return asked;
}

std::vector<Record> unfinished_jobs() {
    job::FileStore* jobs = store();
    if (jobs == nullptr) {
        return {};
    }
    std::vector<Record> out;
    try {
        for (Record& r : jobs->list()) {
            if (r.kind != kKind || spec_string(r, "group_id").empty()) {
                continue;
            }
            // Recently COMPLETE jobs belong here too, and leaving them out was
            // the wrong half of a fix.
            //
            // These are downloads that finished while this application was not
            // running — the whole point of putting them in a durable store. If
            // the list only ever shows unfinished work, then closing the app
            // during a download and coming back to an empty download manager is
            // indistinguishable from the download never having happened. The
            // user is entitled to see that it finished.
            //
            // Bounded by age so the list does not become a permanent history:
            // the question this answers is "what happened while I was away",
            // not "what have I ever downloaded".
            if (r.terminal()) {
                if (r.state != job::state::kComplete) {
                    continue;  // failed and cancelled are not news
                }
                const auto age = job::Clock::now() - r.updated_at;
                if (age > std::chrono::hours(24)) {
                    continue;
                }
            }
            out.push_back(std::move(r));
        }
    } catch (const std::exception&) {
    }
    return out;
}

// ---------------------------------------------------------------- offload ---

bool offload_available() {
    const std::string root = job::machine_store();
    return !root.empty() && job::supervisor_of(root).alive;
}

bool offload(const Context& ctx,
             const std::function<bool(std::size_t, std::size_t)>& on_progress,
             std::string& error_out) {
    job::FileStore* jobs = store();
    if (jobs == nullptr || ctx.final_path.empty()) {
        return false;
    }
    const std::string root = job::machine_store();
    const job::Supervisor sup = root.empty() ? job::Supervisor{} : job::supervisor_of(root);
    if (!sup.alive) {
        return false;  // nothing better here; the caller fetches it itself
    }

    // A caller that will not resume locally must not resume remotely either.
    // The supervisor resumes from the record's checkpoint, so honouring the
    // intent means clearing it -- and removing the partial, or the supervisor
    // would find bytes on disk that no checkpoint vouches for.
    if (!ctx.may_resume) {
        std::error_code ec;
        fs::remove(utils::path_from_utf8(ctx.partial_path), ec);
    }

    // Find or create the job. Same store, same identity rule, same record the
    // supervisor will read -- there is nothing to copy and no checkpoint to
    // carry, because there is only ever one record for this file.
    std::string id;
    try {
        for (const Record& candidate : jobs->list()) {
            if (candidate.kind == kKind && !candidate.terminal() &&
                spec_string(candidate, "sink.final") == ctx.final_path) {
                id = candidate.id;
            }
        }

        if (!id.empty() && !ctx.may_resume) {
            const Record held = jobs->claim(id, owner_name(), kLeaseTtl);
            jobs->update(id, held.lease.epoch, [](Record& r) {
                r.checkpoint = Json{{"verified_prefix", 0}};
                r.progress.done = 0;
            });
            jobs->release(id, held.lease.epoch);
        }
        if (id.empty()) {
            Record r;
            r.kind = kKind;
            r.progress.total = ctx.size;
            Json spec = Json::object();
            Json artifact = Json::object();
            if (!ctx.digest.empty()) artifact["digest"] = qualified_digest(ctx.digest);
            if (ctx.size > 0) artifact["size"] = ctx.size;
            spec["artifact"] = std::move(artifact);
            Json source = Json::object();
            source["scheme"] = ctx.url.rfind("http://", 0) == 0 ? "http" : "https";
            source["locator"] = ctx.url;
            spec["sources"] = Json::array({std::move(source)});
            Json sink = Json::object();
            sink["partial"] = ctx.partial_path;
            sink["final"] = ctx.final_path;
            spec["sink"] = std::move(sink);
            if (!ctx.group_id.empty()) {
                spec["group_id"] = ctx.group_id;
                spec["display_name"] = ctx.display_name;
                spec["file"] = ctx.file;
                spec["file_index"] = ctx.file_index;
                spec["total_files"] = ctx.total_files;
            }
            r.spec = std::move(spec);
            id = jobs->submit(std::move(r));
        }
    } catch (const std::exception& e) {
        LOG(WARNING, "DownloadJob") << "could not record the download (" << e.what()
                                    << "); fetching it here instead" << std::endl;
        return false;
    }

    // And now the whole of "handing it over": do not claim it. The lease is what
    // says who is working, so declining to take one leaves the job claimable and
    // the supervisor adopts it on its next sweep. The nudge only makes that
    // sooner.
    LOG(INFO, "Download") << "left for the system downloader (" << sup.owner << "), job " << id
                          << std::endl;
    job::nudge(root);

    // Wait, reading the record. This process is not moving bytes, but it is
    // still the one that promised a file, so it stays until there is one. Killed
    // here, the transfer does not stop.
    for (;;) {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));

        Record now;
        try {
            now = jobs->load(id);
        } catch (const std::exception& e) {
            error_out = std::string("the download record went missing: ") + e.what();
            return true;
        }

        if (on_progress) {
            const std::size_t done =
                now.progress.done < 0 ? 0 : static_cast<std::size_t>(now.progress.done);
            const std::size_t total =
                now.progress.total < 0 ? 0 : static_cast<std::size_t>(now.progress.total);
            if (!on_progress(done, total)) {
                try {
                    const Record held = jobs->claim(id, owner_name(), kLeaseTtl);
                    jobs->update(id, held.lease.epoch,
                                 [](Record& r) { r.state = job::state::kCancelled; });
                } catch (const std::exception&) {
                }
                error_out = "cancelled";
                return true;
            }
        }

        // Somebody asked this to stop. The rule is the job layer's, not this
        // file's invention: an owner must check intent at least as often as it
        // checkpoints and move toward it.
        //
        // Waiting is what this process is doing, so honouring a pause means
        // giving up the wait -- NOT cancelling the job. The delegate stops on
        // its own side because it reads the same record, and the transfer stays
        // resumable by whoever comes back for it. Writing a terminal state here
        // is what turned a pause into a lost 3.1 GB download.
        if (now.wants() == job::want::kPause) {
            error_out = "paused";
            return true;
        }

        if (now.state == job::state::kTransferred || now.state == job::state::kComplete) {
            return true;
        }
        if (now.state == job::state::kFailed || now.state == job::state::kCancelled) {
            error_out = now.error.empty() ? "the system downloader gave up" : now.error;
            return true;
        }
    }
}

}  // namespace download
}  // namespace lemon
