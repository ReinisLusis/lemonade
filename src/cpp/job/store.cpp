#include <lemon/job/store.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <random>
#include <sstream>
#include <system_error>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;

// How long a claim token may exist without its record having caught up before
// the claimant that made it is presumed gone. The two writes are microseconds
// apart in the same function, so anything on this scale is a crash rather than a
// slow disk -- and generous even for a store on a share.
constexpr int kClaimHandoverSeconds = 10;

namespace lemon {
namespace job {

namespace {

fs::path path_of(const std::string& utf8) {
#if defined(_WIN32) && defined(__cpp_lib_char8_t)
    return fs::path(std::u8string(utf8.begin(), utf8.end()));
#elif defined(_WIN32)
    return fs::u8path(utf8);
#else
    return fs::path(utf8);
#endif
}

std::string utf8_of(const fs::path& p) {
    const auto s = p.u8string();
    return std::string(s.begin(), s.end());
}

std::string random_hex(int bytes) {
    std::random_device rd;
    std::string out;
    out.reserve(static_cast<std::size_t>(bytes) * 2);
    static const char* kHex = "0123456789abcdef";
    for (int i = 0; i < bytes; ++i) {
        const unsigned value = rd() & 0xFFu;
        out.push_back(kHex[(value >> 4) & 0xF]);
        out.push_back(kHex[value & 0xF]);
    }
    return out;
}

// Exclusive create. Returns false when the file already existed, which is the
// answer the claim path needs; anything else throws.
bool create_exclusive(const fs::path& p, const std::string& contents) {
#ifdef _WIN32
    HANDLE h = ::CreateFileW(p.wstring().c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                             FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        const DWORD err = ::GetLastError();
        if (err == ERROR_FILE_EXISTS || err == ERROR_ALREADY_EXISTS) {
            return false;
        }
        throw JobError("cannot create " + utf8_of(p) + ": windows error " + std::to_string(err));
    }
    DWORD written = 0;
    const BOOL ok = contents.empty()
                        ? TRUE
                        : ::WriteFile(h, contents.data(), static_cast<DWORD>(contents.size()),
                                      &written, nullptr);
    ::CloseHandle(h);
    if (!ok || (!contents.empty() && written != contents.size())) {
        throw JobError("short write to " + utf8_of(p));
    }
    return true;
#else
    const int fd = ::open(p.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0644);
    if (fd < 0) {
        if (errno == EEXIST) {
            return false;
        }
        throw JobError("cannot create " + utf8_of(p) + ": " + std::strerror(errno));
    }
    std::size_t offset = 0;
    while (offset < contents.size()) {
        const ssize_t n = ::write(fd, contents.data() + offset, contents.size() - offset);
        if (n <= 0) {
            ::close(fd);
            throw JobError("short write to " + utf8_of(p));
        }
        offset += static_cast<std::size_t>(n);
    }
    ::close(fd);
    return true;
#endif
}

// Replace one file with another so a reader opening it at any moment sees the
// old record or the new one, never half of one — and one of those readers may
// be deciding right now whether this job is an orphan.
void replace_atomically(const fs::path& from, const fs::path& to) {
#ifdef _WIN32
    if (!::MoveFileExW(from.wstring().c_str(), to.wstring().c_str(),
                       MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        const DWORD err = ::GetLastError();
        std::error_code ignored;
        fs::remove(from, ignored);
        throw JobError("cannot replace " + utf8_of(to) + ": windows error " + std::to_string(err));
    }
#else
    if (::rename(from.c_str(), to.c_str()) != 0) {
        std::error_code ignored;
        fs::remove(from, ignored);
        throw JobError("cannot replace " + utf8_of(to) + ": " + std::strerror(errno));
    }
#endif
}

std::int64_t process_id() {
#ifdef _WIN32
    return static_cast<std::int64_t>(::GetCurrentProcessId());
#else
    return static_cast<std::int64_t>(::getpid());
#endif
}

}  // namespace

std::string new_id() {
    const auto millis = std::chrono::duration_cast<std::chrono::milliseconds>(
                            Clock::now().time_since_epoch())
                            .count();
    return std::to_string(millis) + "-" + random_hex(10);
}

FileStore::FileStore(std::string root)
    : root_(std::move(root)), now_([]() { return Clock::now(); }) {
    std::error_code ec;
    for (const char* sub : {"jobs", "work"}) {
        fs::create_directories(path_of(root_) / sub, ec);
        if (ec) {
            throw JobError("cannot create job store under " + root_ + ": " + ec.message());
        }
    }
}

std::string FileStore::record_path(const std::string& id) const {
    return utf8_of(path_of(root_) / "jobs" / (id + ".json"));
}

std::string FileStore::epoch_path(const std::string& id, std::int64_t epoch) const {
    return utf8_of(path_of(root_) / "jobs" / (id + ".epoch." + std::to_string(epoch)));
}

std::string FileStore::work_path(const std::string& id) const {
    return utf8_of(path_of(root_) / "work" / id);
}

std::string FileStore::submit(Record r) {
    if (r.id.empty()) {
        r.id = new_id();
    }
    const TimePoint moment = now();
    r.describe();
    if (r.state.empty()) {
        r.state = state::kPending;
    }
    r.created_at = moment;
    r.updated_at = moment;
    r.progress.updated_at = moment;

    // Exclusive: submitting the same id twice is a caller bug, not something to
    // paper over by overwriting a job that may be running right now.
    if (!create_exclusive(path_of(record_path(r.id)), r.encode())) {
        throw JobError("job " + r.id + " already exists");
    }
    return r.id;
}

Record FileStore::load(const std::string& id) const {
    const fs::path p = path_of(record_path(id));
    std::ifstream in(p, std::ios::binary);
    if (!in) {
        throw NotFound(id);
    }
    std::ostringstream body;
    body << in.rdbuf();
    return Record::decode(body.str());
}

std::vector<Record> FileStore::list() const {
    std::vector<std::string> ids;
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(path_of(root_) / "jobs", ec)) {
        const std::string name = utf8_of(entry.path().filename());
        if (name.size() > 5 && name.compare(name.size() - 5, 5, ".json") == 0) {
            ids.push_back(name.substr(0, name.size() - 5));
        }
    }
    std::sort(ids.begin(), ids.end());

    std::vector<Record> out;
    out.reserve(ids.size());
    for (const auto& id : ids) {
        try {
            out.push_back(load(id));
        } catch (const JobError&) {
            continue;  // one unreadable record must not hide every other job
        }
    }
    return out;
}

bool FileStore::claimable(const Record& r) const {
    return !r.terminal() && !r.lease.held(now());
}

// Transferred is not terminal, so claimable() says yes to a finished job once
// its lease lapses. It is waiting for the requester to take delivery, not for a
// supervisor to redo it.
std::vector<Record> FileStore::orphans() const {
    std::vector<Record> out;
    for (auto& r : list()) {
        // A paused job looks abandoned and is not. The lease was released
        // deliberately, so adopting it here would restart work seconds after a
        // person stopped it.
        if (claimable(r) && r.state != state::kTransferred && !r.paused()) {
            out.push_back(std::move(r));
        }
    }
    return out;
}

Record FileStore::claim(const std::string& id, const std::string& owner,
                        std::chrono::milliseconds ttl) {
    if (owner.find_first_not_of(" \t\r\n") == std::string::npos) {
        throw JobError("claim requires an owner");
    }
    Record r = load(id);
    if (r.terminal()) {
        throw TerminalState(id + " is " + r.state);
    }
    const TimePoint moment = now();
    if (r.lease.held(moment) && r.lease.owner != owner) {
        throw LeaseHeld(r.lease.owner + " holds it until " + format_rfc3339(r.lease.expires_at));
    }

    // The atomic step. Whoever creates this file is the owner at this epoch and
    // nobody else can be, because the filesystem will not create it twice.
    const std::int64_t next = take_epoch(id, r.lease.epoch + 1, owner);

    r.lease.owner = owner;
    r.lease.epoch = next;
    r.lease.expires_at = moment + std::chrono::duration_cast<TimePoint::duration>(ttl);
    if (r.state == state::kPending || r.state == state::kRunning) {
        r.state = state::kRunning;
    }
    write_atomically(r);

    // The previous epoch's token can go now. Nobody will ever ask for it again:
    // a claimant derives its epoch from the RECORD, which now says `next`. Left
    // alone these accumulate one file per claim, forever -- a real store reached
    // 1069 files for 17 jobs.
    if (next > 1) {
        std::error_code ec;
        fs::remove(path_of(epoch_path(id, next - 1)), ec);
    }
    return r;
}

// take_epoch creates the claim token for the first epoch at or after `first`
// that nobody holds, and returns which one it got.
//
// # Why this is not simply "create the next one"
//
// It was, and a job could be bricked by it. The token is created BEFORE the
// record is written, so a process that dies in between leaves a token for an
// epoch the record never reached. Every later claim then computes the same next
// epoch, finds that token, and fails -- permanently. The job cannot be claimed,
// so it cannot be updated, cancelled, adopted or finished by anyone, ever.
//
// Seen on a live store: record at epoch 216, a token for 217, and a supervisor
// reporting a healthy sweep every five seconds while that job silently failed
// its claim. Setting an intent on it did nothing either, because honouring an
// intent requires claiming first.
//
// # Why skipping is safe, and when it is not
//
// Skipping past a HELD epoch would destroy the exclusivity the token exists to
// provide, so freshness decides. A claimant that is genuinely mid-flight wrote
// its token moments ago and is about to write the record; this claim must lose
// to it. A token that has sat there while the record stayed behind belongs to
// nobody, and the epoch is unusable rather than held.
//
// Compared against real time rather than the store's clock: an injected test
// clock says nothing about when the filesystem wrote a file.
std::int64_t FileStore::take_epoch(const std::string& id, std::int64_t first,
                                   const std::string& owner) {
    constexpr int kMaxSkip = 64;
    for (std::int64_t next = first; next < first + kMaxSkip; ++next) {
        const fs::path token = path_of(epoch_path(id, next));
        if (create_exclusive(token, owner + "\n")) {
            return next;
        }
        std::error_code ec;
        const auto written = fs::last_write_time(token, ec);
        if (ec) {
            throw LeaseHeld("epoch " + std::to_string(next) + " was taken by someone else");
        }
        const auto age = decltype(written)::clock::now() - written;
        if (age < std::chrono::seconds(kClaimHandoverSeconds)) {
            throw LeaseHeld("epoch " + std::to_string(next) + " was taken by someone else");
        }
        // Abandoned. Take the epoch after it.
    }
    throw LeaseHeld(std::to_string(kMaxSkip) + " epochs from " + std::to_string(first) +
                    " are all spoken for");
}

Record FileStore::renew(const std::string& id, std::int64_t epoch, std::chrono::milliseconds ttl) {
    Record r = load(id);
    if (r.lease.epoch != epoch) {
        throw StaleEpoch("record is at epoch " + std::to_string(r.lease.epoch) +
                         ", caller holds " + std::to_string(epoch));
    }
    if (!r.lease.held(now())) {
        throw LeaseExpired("expired at " + format_rfc3339(r.lease.expires_at) +
                           ", re-claim instead");
    }
    r.lease.expires_at = now() + std::chrono::duration_cast<TimePoint::duration>(ttl);
    write_atomically(r);
    return r;
}

void FileStore::release(const std::string& id, std::int64_t epoch) {
    update(id, epoch, [this](Record& r) {
        r.lease.expires_at = now();
        r.lease.owner.clear();
        // Releasing a delegated job means "no longer watching it", not
        // "stopped": the work continues in the external system, and demoting it
        // to pending would invite a second tier to start it again.
        if (r.state == state::kRunning && !r.delegated()) {
            r.state = state::kPending;
        }
    });
}

Record FileStore::update(const std::string& id, std::int64_t epoch,
                         const std::function<void(Record&)>& mutate) {
    Record r = load(id);
    if (r.lease.epoch != epoch) {
        throw StaleEpoch("record is at epoch " + std::to_string(r.lease.epoch) +
                         ", caller holds " + std::to_string(epoch));
    }
    if (!r.lease.held(now())) {
        throw LeaseExpired("expired at " + format_rfc3339(r.lease.expires_at));
    }
    mutate(r);
    r.updated_at = now();
    write_atomically(r);
    return r;
}

Record FileStore::set_intent(const std::string& id, const std::string& want,
                             const std::string& by) {
    if (!is_valid_want(want)) {
        throw Invalid("intent \"" + want + "\"");
    }
    Record r = load(id);
    if (r.terminal()) {
        throw TerminalState(id + " is " + r.state);
    }
    Intent in;
    in.want = want;
    in.by = by;
    in.at = now();
    r.intent = in;
    r.updated_at = now();
    write_atomically(r);
    return r;
}

void FileStore::write_atomically(const Record& r) const {
    const std::string body = r.encode();
    const fs::path dir = path_of(root_) / "jobs";
    const fs::path tmp =
        dir / path_of(r.id + ".tmp-" + std::to_string(process_id()) + "-" + random_hex(4));

    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) {
            throw JobError("cannot write " + utf8_of(tmp));
        }
        out.write(body.data(), static_cast<std::streamsize>(body.size()));
        if (!out) {
            out.close();
            std::error_code ignored;
            fs::remove(tmp, ignored);
            throw JobError("short write to " + utf8_of(tmp));
        }
    }
    replace_atomically(tmp, path_of(record_path(r.id)));
}

}  // namespace job
}  // namespace lemon
