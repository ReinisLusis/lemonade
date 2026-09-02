// The record and the lease are a cross-language contract, so most of what
// matters is only provable by running three implementations against one
// directory (scripts/conformance.sh). What is worth pinning here is everything
// a single process can prove on its own: the exact bytes written, the refusals,
// and the epoch fencing.

#include "lemon/job/store.h"

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;
using namespace lemon::job;

static int g_failures = 0;

static void check(const char* name, bool ok) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name);
    if (!ok) ++g_failures;
}

static std::string temp_root() {
    const auto stamp = std::chrono::duration_cast<std::chrono::nanoseconds>(
                           std::chrono::system_clock::now().time_since_epoch())
                           .count();
    const fs::path root = fs::temp_directory_path() / ("lemon-job-test-" + std::to_string(stamp));
    return root.string();
}

template <typename Fn>
static bool throws_job_error(Fn&& fn) {
    try {
        fn();
        return false;
    } catch (const JobError&) {
        return true;
    }
}

static void test_timestamp_format() {
    // Six fractional digits, always, trailing zeros included: Go trimmed them
    // for a while and disagreed with Python about the same instant.
    const TimePoint epoch{};
    check("timestamp: epoch", format_rfc3339(epoch) == "1970-01-01T00:00:00.000000Z");

    const TimePoint t = parse_rfc3339("2026-08-20T09:35:07.829869Z");
    check("timestamp: round trip", format_rfc3339(t) == "2026-08-20T09:35:07.829869Z");

    check("timestamp: trailing zeros kept",
          format_rfc3339(parse_rfc3339("2026-08-20T09:35:07.100000Z")) ==
              "2026-08-20T09:35:07.100000Z");

    // Go's zero time is year 1, which is a negative time_t that gmtime rejects
    // on Windows.
    check("timestamp: year one",
          format_rfc3339(parse_rfc3339("0001-01-01T00:00:00Z")) == "0001-01-01T00:00:00.000000Z");

    // Nanoseconds from a Go writer truncate rather than round: a resume point
    // must never move forward because of a rounding rule.
    check("timestamp: nanoseconds truncate",
          format_rfc3339(parse_rfc3339("2026-08-20T09:35:07.829869999Z")) ==
              "2026-08-20T09:35:07.829869Z");

    check("timestamp: offset normalised to UTC",
          format_rfc3339(parse_rfc3339("2026-08-20T11:35:07.829869+02:00")) ==
              "2026-08-20T09:35:07.829869Z");
}

static void test_encoding() {
    Record r;
    r.id = "1787202430967-a752f9a9c2c77b123ffd";
    r.kind = "download";
    r.state = state::kRunning;
    r.spec = Json::parse(R"({"zebra":1,"apple":{"nested":[1,2,3]}})");
    r.checkpoint = Json::parse(R"({"verified_prefix":400})");
    r.progress.done = 460;
    r.progress.total = 1000;
    r.progress.updated_at = parse_rfc3339("2026-08-20T05:07:14.951609Z");
    r.lease.owner = "cpp-worker";
    r.lease.epoch = 2;
    r.lease.expires_at = parse_rfc3339("2026-08-20T05:08:14.635068Z");
    r.created_at = parse_rfc3339("2026-08-20T05:07:10.967343Z");
    r.updated_at = parse_rfc3339("2026-08-20T05:07:15.134811Z");

    const std::string encoded = r.encode();
    const std::string expected =
        "{\n"
        "  \"schema\": 4,\n"
        "  \"id\": \"1787202430967-a752f9a9c2c77b123ffd\",\n"
        "  \"kind\": \"download\",\n"
        "  \"state\": \"running\",\n"
        "  \"spec\": {\n"
        "    \"zebra\": 1,\n"
        "    \"apple\": {\n"
        "      \"nested\": [\n"
        "        1,\n"
        "        2,\n"
        "        3\n"
        "      ]\n"
        "    }\n"
        "  },\n"
        "  \"checkpoint\": {\n"
        "    \"verified_prefix\": 400\n"
        "  },\n"
        "  \"progress\": {\n"
        "    \"done\": 460,\n"
        "    \"total\": 1000,\n"
        "    \"updated_at\": \"2026-08-20T05:07:14.951609Z\"\n"
        "  },\n"
        "  \"lease\": {\n"
        "    \"owner\": \"cpp-worker\",\n"
        "    \"epoch\": 2,\n"
        "    \"expires_at\": \"2026-08-20T05:08:14.635068Z\"\n"
        "  },\n"
        "  \"created_at\": \"2026-08-20T05:07:10.967343Z\",\n"
        "  \"updated_at\": \"2026-08-20T05:07:15.134811Z\"\n"
        "}\n";
    check("encode: byte-identical to the agreed form", encoded == expected);
    if (encoded != expected) {
        std::printf("---- got ----\n%s---- want ----\n%s", encoded.c_str(), expected.c_str());
    }

    // An alphabetising JSON container would reorder the opaque spec, which is
    // the caller's data and not ours to touch.
    check("encode: spec key order preserved",
          encoded.find("\"zebra\"") < encoded.find("\"apple\""));

    const Record back = Record::decode(encoded);
    check("decode: round trips", back.encode() == encoded);
    check("decode: spec is returned untouched", back.spec == r.spec);
    check("decode: checkpoint is returned untouched", back.checkpoint == r.checkpoint);

    Record thin;
    thin.id = "x";
    thin.kind = "download";
    const std::string thin_encoded = thin.encode();
    check("encode: zero total omitted", thin_encoded.find("\"total\"") == std::string::npos);
    check("encode: absent checkpoint omitted",
          thin_encoded.find("\"checkpoint\"") == std::string::npos);
    check("encode: empty requires omitted",
          thin_encoded.find("\"requires\"") == std::string::npos);
    check("encode: empty error omitted", thin_encoded.find("\"error\"") == std::string::npos);
    check("encode: trailing newline", !thin_encoded.empty() && thin_encoded.back() == '\n');
}

static void test_refusals() {
    Record r;
    r.id = "x";
    check("validate: kind is required", throws_job_error([&] { r.encode(); }));

    const std::string good = [] {
        Record ok;
        ok.id = "x";
        ok.kind = "download";
        return ok.encode();
    }();

    // A version this implementation genuinely cannot read. It used to say 4,
    // which was "one past current" at the time and stopped being unknown the
    // moment intent landed — at which point find() returned npos and replace()
    // threw std::out_of_range, which is not a JobError, so it escaped main
    // uncaught and the whole run died at 0xC0000409 with no output.
    std::string future = good;
    future.replace(future.find("\"schema\": 4"), 11, "\"schema\": 9");
    check("decode: unknown schema refused", throws_job_error([&] { Record::decode(future); }));

    // Version 3 stays readable, and that is a guarantee rather than an accident:
    // an absent intent means run, which is exactly what version 3 always meant,
    // and stores full of version 3 records exist on disks and on a NAS.
    std::string older = good;
    older.replace(older.find("\"schema\": 4"), 11, "\"schema\": 3");
    check("decode: version 3 is still readable", !throws_job_error([&] { Record::decode(older); }));

    std::string extra = good;
    extra.replace(extra.find("\"id\""), 4, "\"invented_key\": 1,\n  \"id\"");
    check("decode: unknown field refused", throws_job_error([&] { Record::decode(extra); }));

    std::string no_kind = good;
    no_kind.replace(no_kind.find("\"download\""), 10, "\"\"");
    check("decode: missing kind refused", throws_job_error([&] { Record::decode(no_kind); }));

    check("decode: garbage refused", throws_job_error([] { Record::decode("not json"); }));
}

// Intent: what somebody WANTS, written by a party holding no lease.
//
// The case this exists for is a person clicking cancel in this server's UI while
// a supervisor on another machine moves the bytes. That person has no lease and
// cannot get one without stealing the job, which is the single thing the lease
// exists to prevent.
static void test_intent() {
    const std::string root = temp_root();
    FileStore store(root);

    Record r;
    r.kind = "download";
    r.spec = Json::parse(R"({"sink":{"final":"model.gguf"}})");
    const std::string id = store.submit(r);

    check("intent: absent means run", store.load(id).wants() == want::kRun);
    check("intent: work with no intent is available", store.orphans().size() == 1);

    // No epoch is presented. That absence is the feature.
    const Record paused = store.set_intent(id, want::kPause, "a-person");
    check("intent: settable without a lease", paused.wants() == want::kPause);
    check("intent: who asked is recorded", paused.intent && paused.intent->by == "a-person");
    check("intent: paused is not terminal", !paused.terminal() && paused.paused());

    // A paused job looks abandoned and is not. A sweep that adopted it would
    // restart work seconds after a person stopped it.
    check("intent: a paused job is not an orphan", store.orphans().empty());

    store.set_intent(id, want::kRun, "a-person");
    check("intent: resuming makes it available again", store.orphans().size() == 1);

    // Survives the round trip another language will read it through.
    const Record back = Record::decode(store.load(id).encode());
    check("intent: round trips", back.wants() == want::kRun);

    check("intent: an unknown want is refused",
          throws_job_error([&] { store.set_intent(id, "nonsense", "a-person"); }));

    // Nothing reopens finished work.
    const Record claimed = store.claim(id, "worker", std::chrono::seconds(30));
    store.update(id, claimed.lease.epoch, [](Record& rec) { rec.state = state::kComplete; });
    check("intent: refused once terminal",
          throws_job_error([&] { store.set_intent(id, want::kCancel, "a-person"); }));

    std::error_code ec;
    fs::remove_all(root, ec);
}

static void test_lease_rules() {
    const std::string root = temp_root();
    FileStore store(root);

    Record r;
    r.kind = "download";
    r.spec = Json::parse(R"({"sink":{"final":"model.gguf"}})");
    r.progress.total = 1000;
    const std::string id = store.submit(std::move(r));

    check("submit: id is returned", !id.empty());
    check("submit: pending", store.load(id).state == state::kPending);
    check("submit: a fresh job is an orphan", store.orphans().size() == 1);

    const Record claimed = store.claim(id, "cpp-worker", std::chrono::seconds(30));
    check("claim: epoch starts at one", claimed.lease.epoch == 1);
    check("claim: state becomes running", claimed.state == state::kRunning);
    check("claim: token file created",
          fs::exists(fs::path(root) / "jobs" / (id + ".epoch.1")));
    check("claim: a held job is not an orphan", store.orphans().empty());
    check("claim: another owner is refused",
          throws_job_error([&] { store.claim(id, "other", std::chrono::seconds(30)); }));

    store.update(id, 1, [](Record& rec) {
        rec.progress.done = 460;
        rec.checkpoint = Json::parse(R"({"verified_prefix":400})");
    });
    check("update: stale epoch refused",
          throws_job_error([&] { store.update(id, 0, [](Record&) {}); }));
    check("update: future epoch refused",
          throws_job_error([&] { store.update(id, 7, [](Record&) {}); }));

    // The sleep case: a process suspended past its own expiry wakes up still
    // believing it owns the job. Renew must refuse and force a re-claim, which
    // bumps the epoch and invalidates anything it had in flight.
    store.set_clock([] { return Clock::now() + std::chrono::hours(1); });
    check("renew: an expired lease may not be renewed",
          throws_job_error([&] { store.renew(id, 1, std::chrono::seconds(30)); }));
    check("update: an expired lease may not write",
          throws_job_error([&] { store.update(id, 1, [](Record&) {}); }));
    check("orphans: an expired lease is claimable", store.orphans().size() == 1);

    const Record adopted = store.claim(id, "successor", std::chrono::seconds(30));
    check("claim: epoch rises on adoption", adopted.lease.epoch == 2);
    check("claim: successor inherits the proven checkpoint",
          adopted.checkpoint && (*adopted.checkpoint)["verified_prefix"] == 400);
    check("claim: the predecessor's epoch is now stale",
          throws_job_error([&] { store.update(id, 1, [](Record&) {}); }));

    store.update(id, 2, [](Record& rec) { rec.state = state::kComplete; });
    check("claim: a terminal job is refused",
          throws_job_error([&] { store.claim(id, "late", std::chrono::seconds(30)); }));

    check("load: an unknown id is not found",
          throws_job_error([&] { store.load("no-such-job"); }));

    std::error_code ec;
    fs::remove_all(root, ec);
}

static void test_atomic_replacement() {
    const std::string root = temp_root();
    FileStore store(root);

    Record r;
    r.kind = "download";
    const std::string id = store.submit(std::move(r));
    store.claim(id, "cpp-worker", std::chrono::seconds(30));
    store.update(id, 1, [](Record& rec) { rec.progress.done = 1; });

    int leftovers = 0;
    for (const auto& entry : fs::directory_iterator(fs::path(root) / "jobs")) {
        if (entry.path().filename().string().find(".tmp-") != std::string::npos) {
            ++leftovers;
        }
    }
    check("write: no temp files left behind", leftovers == 0);

    std::error_code ec;
    fs::remove_all(root, ec);
}

// A finished job is waiting for the requester, not for a supervisor to redo it.
static void test_transferred_is_not_an_orphan() {
    const std::string root = temp_root();
    FileStore store(root);

    Record r;
    r.kind = "download";
    const std::string id = store.submit(std::move(r));
    Record held = store.claim(id, "worker", std::chrono::seconds(30));
    store.update(id, held.lease.epoch,
                 [](Record& rec) { rec.state = state::kTransferred; });
    check("staging: the job really is transferred",
          store.load(id).state == state::kTransferred);

    // Let the lease lapse, as it would after a crash. The clock moves rather
    // than the test sleeping: a 1 ms TTL makes the staging update itself race
    // the expiry, which is how the Go version of this test once staged nothing
    // and passed anyway.
    store.set_clock([] { return Clock::now() + std::chrono::hours(1); });

    bool offered = false;
    for (const auto& o : store.orphans()) {
        if (o.id == id) offered = true;
    }
    check("orphans: a transferred job is not stranded work", !offered);

    // But taking delivery must still be possible. Only the rescue sweep leaves
    // it alone.
    bool can_take_delivery = true;
    try {
        store.claim(id, "consumer", std::chrono::seconds(30));
    } catch (const std::exception&) {
        can_take_delivery = false;
    }
    check("claim: a transferred job may still be taken delivery of",
          can_take_delivery);

    std::error_code ec;
    fs::remove_all(root, ec);
}

// Delegation releases the lease immediately, so a delegated job spends most of
// its life unleased. Demoting it to pending would invite a second tier to start
// the same work again.
static void test_releasing_delegated_job_keeps_it_running() {
    const std::string root = temp_root();
    FileStore store(root);

    Record r;
    r.kind = "download";
    const std::string id = store.submit(std::move(r));
    Record held = store.claim(id, "delegator", std::chrono::seconds(30));
    store.update(id, held.lease.epoch, [](Record& rec) {
        Delegation d;
        d.system = "nas";
        d.external_id = "remote-1";
        rec.delegation = d;
        rec.state = state::kRunning;
    });
    store.release(id, held.lease.epoch);

    Record got = store.load(id);
    check("release: a delegated job stays running", got.state == state::kRunning);
    check("release: the delegation handle survives", got.delegated());

    // An undelegated job still goes back to pending, which is what release is
    // for in the ordinary case.
    Record p;
    p.kind = "download";
    const std::string plain = store.submit(std::move(p));
    Record ph = store.claim(plain, "worker", std::chrono::seconds(30));
    store.release(plain, ph.lease.epoch);
    check("release: an ordinary job returns to pending",
          store.load(plain).state == state::kPending);

    std::error_code ec;
    fs::remove_all(root, ec);
}

int main() {
    // Unbuffered, so a crash still tells you which check it died after. When
    // stdout is a pipe it is fully buffered, so a test that dies mid-run prints
    // absolutely nothing and the failure looks like it happened before the
    // first line — which costs far more time than the buffering ever saved.
    std::setvbuf(stdout, nullptr, _IONBF, 0);

    test_timestamp_format();
    test_encoding();
    test_refusals();
    test_intent();
    test_lease_rules();
    test_atomic_replacement();
    test_transferred_is_not_an_orphan();
    test_releasing_delegated_job_keeps_it_running();

    std::printf("\n%s: %d failure(s)\n", g_failures == 0 ? "OK" : "FAILED", g_failures);
    return g_failures == 0 ? 0 : 1;
}
