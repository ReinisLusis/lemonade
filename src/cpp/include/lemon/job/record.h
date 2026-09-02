#pragma once

// job — a handle to work that is happening somewhere else.
//
// This is the C++ side of an abstraction that already exists in Go and in
// Python. The three are not ports of one another and are not generated from a
// shared IDL. They agree about one thing: the record on disk, and the rules for
// taking ownership of it. The API here looks like C++; the bytes are the
// contract.
//
// WHAT THIS DOES NOT KNOW: what the work IS. `spec` and `checkpoint` are opaque
// and `kind` says who may read them. A download's artifact, sources and sink
// live in a download's spec, not here — which is why downloading can grow
// mirrors and chunk manifests without the record every language must agree
// about changing underneath them.

#include <chrono>
#include <cstdint>
#include <nlohmann/json.hpp>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace lemon {
namespace job {

// Key order is part of the on-disk contract — Go decodes with
// DisallowUnknownFields and the file is read by humans in diffs — so this must
// be the insertion-ordered specialisation, never the std::map-backed default
// that would silently alphabetise every object it touches, spec included.
using Json = nlohmann::ordered_json;

using Clock = std::chrono::system_clock;
using TimePoint = Clock::time_point;

// The data models this implementation understands, and writes.
//
// A name is namespaced and versioned: "abstraction.job/base@1". The version is
// part of the name rather than a separate field, so an incompatible change to
// one model is a NEW name that old readers correctly fail to recognise, while
// every other model in the record stays readable.
//
// This replaced an integer version. A version conflates WHAT CHANGED with WHAT
// YOU MUST UNDERSTAND: "schema 5" cannot say which part a reader is missing, so
// refusing the whole record was the only safe answer even when the addition was
// decoration. See kCritical below for the half that makes this more than a
// rename.
namespace model {
// The record every reader must understand. Always present, always critical.
constexpr const char* kBase = "abstraction.job/base@1";
// What somebody WANTS to happen. Critical whenever present: a reader that
// ignores it works on a job somebody asked to stop.
constexpr const char* kIntent = "abstraction.job/intent@1";
// The work is being done by an external system. Critical: a reader that misses
// it would adopt a job BITS is already running and fetch the bytes twice.
constexpr const char* kDelegation = "abstraction.job/delegation@1";
// Which phase of multi-phase work is happening now. NEVER critical: advisory,
// for telling a person what is going on.
constexpr const char* kStep = "abstraction.job/step@1";
}  // namespace model

// Whether this implementation can read a model named in a record's `critical`.
bool is_known_model(const std::string& name);

// The models a record carries, derived from what is actually in it.
void derive_models(const struct Record& r, std::vector<std::string>& out_content,
                   std::vector<std::string>& out_critical);

// The first critical model this implementation cannot read, or "" if it can
// read them all. Only `critical` is consulted: a name in `content` that nobody
// here knows is data to carry, not a reason to stop.
std::string first_unknown_critical(const std::vector<std::string>& critical);

// The integer this format used to carry, mapped onto the models each version
// implied. No longer written; still read, because stores full of version 3 and
// 4 records exist on real disks and on a NAS. The mapping is exact rather than
// a guess: those versions are frozen and it is known what each could contain.
std::vector<std::string> models_for_legacy_schema(int schema, bool* ok);

// What somebody WANTS to happen, as against what is happening.
namespace want {
constexpr const char* kRun = "run";      // the default, and what absence means
constexpr const char* kPause = "pause";  // stop, keep everything, stay out of sweeps
constexpr const char* kCancel = "cancel";  // abandon on purpose
}  // namespace want

bool is_valid_want(const std::string& w);

namespace state {
constexpr const char* kPending = "pending";
constexpr const char* kRunning = "running";
// Finished and proven, but the result has not been taken delivery of. Not
// bureaucracy: it is the only way to express "the service finished this while
// the application was closed".
constexpr const char* kTransferred = "transferred";
constexpr const char* kComplete = "complete";
constexpr const char* kFailed = "failed";
constexpr const char* kCancelled = "cancelled";
}  // namespace state

bool is_valid_state(const std::string& s);
bool is_terminal_state(const std::string& s);

class JobError : public std::runtime_error {
public:
    explicit JobError(const std::string& what) : std::runtime_error(what) {}
    virtual const char* name() const noexcept { return "JobError"; }
};

class NotFound : public JobError {
public:
    explicit NotFound(const std::string& what) : JobError(what) {}
    const char* name() const noexcept override { return "NotFound"; }
};

class Invalid : public JobError {
public:
    explicit Invalid(const std::string& what) : JobError(what) {}
    const char* name() const noexcept override { return "Invalid"; }
};

class UnknownSchema : public Invalid {
public:
    explicit UnknownSchema(const std::string& what) : Invalid(what) {}
    const char* name() const noexcept override { return "UnknownSchema"; }
};

class LeaseHeld : public JobError {
public:
    explicit LeaseHeld(const std::string& what) : JobError(what) {}
    const char* name() const noexcept override { return "LeaseHeld"; }
};

class StaleEpoch : public JobError {
public:
    explicit StaleEpoch(const std::string& what) : JobError(what) {}
    const char* name() const noexcept override { return "StaleEpoch"; }
};

class LeaseExpired : public JobError {
public:
    explicit LeaseExpired(const std::string& what) : JobError(what) {}
    const char* name() const noexcept override { return "LeaseExpired"; }
};

class TerminalState : public JobError {
public:
    explicit TerminalState(const std::string& what) : JobError(what) {}
    const char* name() const noexcept override { return "TerminalState"; }
};

// Always UTC, always exactly six fractional digits, always a trailing Z.
//
// Six and not nine because Python's datetime cannot hold nanoseconds, and the
// contract is set by the least precise participant. Trailing zeros are NOT
// trimmed: Go trimmed them for a while and wrote ".22275Z" where Python wrote
// ".222750Z" for the same instant, so a job's history diffed as changed every
// time the two took turns on it.
std::string format_rfc3339(TimePoint t);

// Permissive on the way in, because another implementation may be less careful
// than this one and refusing to read a job over a timezone suffix would be
// absurd. Accepts any fractional precision and either Z or a numeric offset.
TimePoint parse_rfc3339(const std::string& s);

// Deliberately thin: two numbers and a timestamp, in units the kind defines.
// Best-effort and explicitly NOT monotonic — a job resuming from a checkpoint
// after a crash can legitimately report a smaller `done` than before. Nothing
// may decide anything on it. Anything richer belongs in the kind's checkpoint.
// Which phase of a multi-phase job is happening now.
//
// In the record and not the kind's checkpoint, because a checkpoint is opaque:
// only a reader that knows the kind could render it. Progress is the one thing a
// GENERIC reader must be able to display -- a supervisor's status output, a
// download manager listing work of every kind -- without knowing what the work
// is.
//
// What made it necessary: a download delegated to a NAS is two transfers, and
// only the first was ever visible. The far side fetched 40 GB and reported done;
// then this machine copied those 40 GB back across a share and re-hashed them,
// with the record still showing the first transfer's numbers throughout.
//
// ADVISORY ONLY, the same rule progress already has. Nothing may decide anything
// on a step. The moment something branches on ordinal == 2, a workflow engine
// has been smuggled into a record whose value is that it describes work without
// prescribing it. `name` is opaque here, exactly like kind and spec.
struct Step {
    std::string name;
    int ordinal = 0;  // counts from one
    int of = 0;       // 0 when the writer cannot say how many
    // This phase's own units, which need not be the job's: hashing counts the
    // same bytes a second time, and the overall numbers must not double for it.
    std::int64_t done = 0;
    std::int64_t total = 0;
};

struct Progress {
    std::int64_t done = 0;
    std::int64_t total = 0;  // 0 means unknown
    TimePoint updated_at{};
    // Absent means what every record before this meant: one unnamed phase.
    std::optional<Step> step;
};

// The right to work on a job, for a bounded time.
//
// `epoch` is the part that matters: it rises by one on every claim, and every
// write must present the epoch it holds. A process that slept through its own
// expiry wakes believing it is still the owner; its writes carry a stale epoch
// and are refused. Without that, two owners work one job and each believes the
// result is correct.
struct Lease {
    std::string owner;
    std::int64_t epoch = 0;
    TimePoint expires_at{};

    bool held(TimePoint now) const { return !owner.empty() && now < expires_at; }
};

// The work has been handed to something outside this process — a system
// service, a daemon on a NAS — which is now doing it. When this is set,
// `progress` is a CACHE of what the external system last reported; the external
// system is the truth.
struct Delegation {
    std::string system;
    std::string external_id;
    bool delivered = false;
};

// The desired state, separate from the observed one.
//
// Why this exists, which is not "so there can be a pause button": every write
// to a record needs the lease, and whoever wants a change is almost never
// holding it — a person clicks cancel in this server's UI while a supervisor on
// another machine moves the bytes. Before this field that was inexpressible,
// and the only alternative was to steal the job, which is the single thing the
// lease exists to prevent.
//
// Every system that has met this problem separates desired from observed:
// Kubernetes has spec against status with a deletionTimestamp anyone may set,
// Temporal records cancellation-requested apart from the run state, BITS
// exposes a state its own service polls.
//
// The rules, which are the contract rather than this implementation's habits:
//
//   1. Anyone may write it, lease or no lease. It is the ONE field exempt.
//   2. Only the lease holder may write `state`. Unchanged.
//   3. An owner MUST check it at least as often as it checkpoints and move
//      toward it. An owner that reads a record and ignores this is not an
//      implementation of this abstraction.
//   4. Cancel must be honoured by everything; stopping is universal.
//   5. Pause must be honoured by implementations that advertise it. One that
//      cannot must FAIL the job with a reason rather than carry on, because a
//      pause that quietly does nothing is worse than a refusal.
//   6. A paused job is NOT an orphan — see FileStore::orphans.
//   7. Once `state` is terminal this is history.
struct Intent {
    std::string want = want::kRun;
    // Who asked. Not decoration: a job sitting against somebody's wish is one
    // of the few things that cannot be worked out from outside, and "which
    // process asked for this" is the first question anyone has.
    std::string by;
    std::optional<TimePoint> at;
};

// The whole job, and the cross-language contract. Everything a different
// process — in a different language, after a reboot — needs in order to
// continue this work has to be in here, because nothing else survives.
struct Record {
    // What this record carries, and the subset a reader MUST understand or
    // refuse it entirely. Not knowing the step model is harmless; not knowing
    // the intent model means working on a job somebody asked to stop. X.509
    // settled this with critical certificate extensions.
    std::vector<std::string> content;
    std::vector<std::string> critical;
    // Data this layer does not understand, keyed by a name that says who does.
    // A reader that cannot read one MUST preserve it on write: dropping it
    // destroys another participant's data invisibly, because nobody here can
    // see what was lost.
    Json extensions = Json::object();
    std::string id;
    std::string kind;
    std::string state = state::kPending;
    Json spec = Json::object();
    std::optional<Json> checkpoint;
    Progress progress;
    Lease lease;
    std::optional<Delegation> delegation;
    // The JSON key is "requires"; the member cannot be, because `requires` is a
    // keyword from C++20 on and this header has to compile under both standards.
    std::vector<std::string> requires_capabilities;
    std::string error;
    // What somebody WANTS to happen, as against `state`, which is what IS
    // happening. Absent means run. Anyone may write it, lease or no lease; the
    // owner honours it. See Intent.
    std::optional<Intent> intent;
    TimePoint created_at{};
    TimePoint updated_at{};

    void validate() const;
    // Validate against a content declaration that has not been stored yet, so
    // encode() can derive and check in one pass while staying const.
    void validate_with(const std::vector<std::string>& check_content,
                       const std::vector<std::string>& check_critical) const;

    // Fill in content and critical from what this record actually carries.
    // Derived rather than remembered, so the declaration cannot drift from the
    // data. Caller-declared criticals are preserved.
    void describe();
    bool terminal() const { return is_terminal_state(state); }
    bool delegated() const { return delegation.has_value(); }

    // The desired state, which is "run" unless somebody said otherwise.
    //
    // Callers use this rather than testing `intent` for a value, so that
    // "nobody asked for anything" and "somebody asked for it to run" are the
    // same answer everywhere — including for version 3 records, which have no
    // intent at all.
    const std::string& wants() const {
        static const std::string kDefault = want::kRun;
        if (!intent.has_value() || intent->want.empty()) return kDefault;
        return intent->want;
    }

    // Somebody asked this to stop and it has not finished. Sweeps use it: a
    // paused job must not be adopted, or it would be restarted seconds after a
    // person stopped it.
    bool paused() const { return wants() == want::kPause && !terminal(); }

    // The form every implementation agrees on: 2-space indent, trailing
    // newline, keys in this order.
    std::string encode() const;

    // Refuses anything it cannot safely continue, including a record carrying a
    // key this version does not know — far more likely to be a newer writer
    // than a typo, and continuing a job whose description we only partly
    // understand is exactly the risk not worth taking.
    static Record decode(const std::string& text);
};

}  // namespace job
}  // namespace lemon
