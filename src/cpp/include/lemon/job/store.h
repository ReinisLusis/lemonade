#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <lemon/job/record.h>
#include <string>
#include <vector>

namespace lemon {
namespace job {

// An identifier that sorts by creation time, so a directory listing comes back
// in roughly submission order without anything having to record that.
std::string new_id();

// Store is where jobs live, and it is the interface this server can implement
// for itself.
//
// # Why this is an interface at all
//
// It is the SEMANTICS of the lease protocol and nothing else: a claim is
// exclusive, an epoch only ever increases, every write presents the epoch it
// holds, and a successor continues only from what a predecessor proved. Not one
// clause of that mentions a byte, a file or a directory.
//
// # What this is NOT for, having checked
//
// The plan was that Lemonade's own engine (include/lemon/jobs/) would implement
// this and there would be one job system instead of two. It cannot, and the
// reason is worth writing down rather than rediscovering.
//
// That engine has named steps, a cursor, ops in a pluggable registry, and a
// pause/interrupt/resume lifecycle persisted across a restart. What it does not
// have, anywhere in its headers, is a lease, an epoch, an owner or a claim —
// zero occurrences of any of them. It is a SINGLE-PROCESS orchestrator: this
// server owns every job it knows about, so there is nothing to be exclusive
// against and no need for any of that.
//
// Our contract is strictly stronger, and the extra part is the whole point: work
// survives the process that asked for it and can be adopted by a supervisor, or
// by a daemon on a NAS. An adapter would have to fabricate a lease it could not
// honour, and a fabricated lease is exactly the two-owners damage the lease
// exists to prevent.
//
// So the layering is the other way round from what integrating.md guessed:
//
//     their engine   the ORCHESTRATOR — steps, ops, the pause button, the UI
//     this Store     the SUBSTRATE    — durable, cross-process, adoptable
//
// A download op in their engine submits here and observes; their pause and
// resume become set_intent, which is precisely why schema 4 added it. That is
// one executor, which was the requirement, without pretending a single-process
// manager can arbitrate between machines.
//
// The interface still earns its place: it is what lets a service binding replace
// the file one later without a caller noticing. But in C++ it has ONE
// implementation today, and calling that an abstraction would be premature.
class Store {
public:
    virtual ~Store() = default;

    virtual std::string submit(Record r) = 0;
    virtual Record load(const std::string& id) const = 0;
    virtual std::vector<Record> list() const = 0;
    virtual bool claimable(const Record& r) const = 0;
    virtual std::vector<Record> orphans() const = 0;
    virtual Record claim(const std::string& id, const std::string& owner,
                         std::chrono::milliseconds ttl) = 0;
    virtual Record renew(const std::string& id, std::int64_t epoch,
                         std::chrono::milliseconds ttl) = 0;
    virtual void release(const std::string& id, std::int64_t epoch) = 0;
    virtual Record update(const std::string& id, std::int64_t epoch,
                          const std::function<void(Record&)>& mutate) = 0;

    // Says what should happen, WITHOUT holding the lease. The only write that
    // presents no epoch: whoever wants a job stopped is not the process doing
    // it — a person clicking cancel in this server's UI while a supervisor on
    // another machine moves the bytes — and requiring a lease would mean
    // stealing the job in order to stop it.
    virtual Record set_intent(const std::string& id, const std::string& want,
                              const std::string& by) = 0;
};

// LocalStore is an OPTIONAL capability: a store whose binding happens to be a
// filesystem can offer an area on it.
//
// Separate from Store on purpose. A store backed by this server's own engine
// answers no, and a caller then has to have a real answer for that rather than
// assuming a directory exists. Reaching through a concrete store for its root
// is what let a binding name itself all the way up in the Go implementation.
class LocalStore {
public:
    virtual ~LocalStore() = default;
    virtual const std::string& root() const = 0;
    virtual std::string work_path(const std::string& id) const = 0;
};

// FileStore is the FILE BINDING of Store: jobs as files in a directory.
//
// The bottom tier, not the substrate. It is the default because it needs no
// daemon, no database, no server and no port — a directory is the one thing a Go
// process, a Python process, a Windows service and a machine that has just
// rebooted can all agree on without any of them running at the same time.
//
//     <root>/jobs/<id>.json        the record
//     <root>/jobs/<id>.epoch.<n>   claim token for epoch n, created O_EXCL
//     <root>/work/<id>             scratch space for a job that needs it
//
// The claim tokens are the mutual exclusion. Exclusive create is atomic on both
// NTFS and POSIX, so exactly one process can create `<id>.epoch.7` and
// therefore exactly one process can own epoch 7. Because each generation gets
// its own filename, a token left behind by a process that was killed blocks
// nothing — the next claimant takes epoch 8. A single lockfile would instead
// have to be broken by timeout, and breaking locks by timeout is how two owners
// end up working one job.
//
// Everything below this line — the layout, the epoch tokens, the JSON — is
// private to this binding. Nothing written against Store may depend on any of it.
class FileStore : public Store, public LocalStore {
public:
    explicit FileStore(std::string root);

    const std::string& root() const override { return root_; }

    // Scratch space a job may use while it runs. What goes there is the kind's
    // business; the store only guarantees the path is derived from the id, so a
    // successor can find what a predecessor left.
    std::string work_path(const std::string& id) const override;

    // Records a new job and returns its id. The id is the handle: a plain
    // string that can be written to a config file or handed to another program,
    // and the process that submitted the job need not be alive for it to remain
    // meaningful.
    std::string submit(Record r) override;

    // Any process may do this, including one that holds no lease and never
    // will. That is what makes progress observable from outside — a callback
    // cannot, because a callback is bound to the lifetime of the process that
    // registered it, and that lifetime is the one that fails.
    Record load(const std::string& id) const override;

    std::vector<Record> list() const override;

    bool claimable(const Record& r) const override;

    // The jobs nobody is working on. This is the primary reclamation path, not
    // a fallback: a process that is killed, or a machine that loses power,
    // never gets to hand anything over.
    std::vector<Record> orphans() const override;

    // Takes ownership for ttl and returns the record carrying the caller's new
    // epoch. Every later write must present that epoch.
    Record claim(const std::string& id, const std::string& owner,
                 std::chrono::milliseconds ttl) override;

    // Extends a lease the caller still holds. Refuses once expired even when
    // the epoch still matches and nobody else has claimed: a process suspended
    // for an hour wakes believing it is still the owner, and forcing it to
    // re-claim bumps the epoch so anything it had in flight is refused rather
    // than landing on top of work a different owner may since have done.
    Record renew(const std::string& id, std::int64_t epoch,
                 std::chrono::milliseconds ttl) override;

    // Gives up a lease early so the job can be taken immediately. A courtesy:
    // everything still works without it, just more slowly.
    void release(const std::string& id, std::int64_t epoch) override;

    // The single gate every change passes through, so staleness is checked in
    // exactly one place instead of once per call site.
    Record update(const std::string& id, std::int64_t epoch,
                  const std::function<void(Record&)>& mutate) override;

    // Says what should happen, WITHOUT holding the lease.
    //
    // The only write here that presents no epoch, and deliberately so: whoever
    // wants a job stopped is not the process doing it — a person clicks cancel
    // in this server's UI while a supervisor on another machine moves the bytes
    // — and requiring a lease would mean stealing the job in order to stop it,
    // which is the one thing the lease exists to prevent.
    //
    // Idempotent, and refused once the job is terminal, because nothing reopens
    // finished work. Asking for something the current owner cannot do is NOT an
    // error here: only the owner knows what it can do.
    Record set_intent(const std::string& id, const std::string& want,
                      const std::string& by) override;

    // Injectable so lease tests are neither slow nor flaky.
    void set_clock(std::function<TimePoint()> now) { now_ = std::move(now); }

private:
    std::string record_path(const std::string& id) const;
    std::string epoch_path(const std::string& id, std::int64_t epoch) const;
    // Creates the claim token for the first free epoch at or after `first`,
    // stepping over tokens abandoned by a process that died between writing one
    // and writing its record. Without that step such a token bricks the job.
    std::int64_t take_epoch(const std::string& id, std::int64_t first, const std::string& owner);
    void write_atomically(const Record& r) const;
    TimePoint now() const { return now_(); }

    std::string root_;
    std::function<TimePoint()> now_;
};

}  // namespace job
}  // namespace lemon
