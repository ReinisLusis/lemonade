// Does the storage layer actually stop an application inventing a destination?
//
// The behaviour under test is not "does place() return a string". It is that a
// Ref carries NO path an application can concatenate, so the line that caused
// all of this —
//
//     std::string partial_path = output_path + ".partial";
//
// has nothing to be written against. Most of that guarantee is enforced by the
// compiler and cannot be asserted at runtime; what this file checks is the part
// that can be: the two-phase commit, that a placed blob is not findable until it
// is committed, and that digests are compared by meaning rather than spelling.

#include <lemon/storage/storage.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

namespace fs = std::filesystem;
using namespace lemon::storage;

static int failures = 0;

static void ok(const std::string& what) { std::printf("  PASS  %s\n", what.c_str()); }

static void bad(const std::string& what, const std::string& why) {
    std::printf("  FAIL  %s\n        %s\n", what.c_str(), why.c_str());
    ++failures;
}

static void check(bool cond, const std::string& what, const std::string& why = "") {
    if (cond) {
        ok(what);
    } else {
        bad(what, why);
    }
}

static const char* kDigest = "sha256:2fc237e65e1f963310e9c961d8e71e932734a72f90e3216a972d83edd1feb756";

int main() {
#ifdef _WIN32
    _setmode(_fileno(stdout), _O_BINARY);
#endif
    std::printf("storage\n");

    const fs::path root = fs::temp_directory_path() / "lemon-storage-test";
    std::error_code ec;
    fs::remove_all(root, ec);

    try {
        Content store("test", root.u8string());

        // Nothing is here yet.
        Ref found;
        check(!store.find(kDigest, found), "bytes that were never placed are not found");

        // Placing settles a location and writes nothing.
        const Ref placed = store.place(kDigest, 4096);
        check(!placed.empty(), "place returns a usable ref");
        check(placed.digest() == kDigest, "the ref carries the digest it was placed for");
        check(placed.store() == "test", "the ref says which store it came from");

        const std::string incoming = store.incoming_path(placed);
        const std::string blob = store.path(placed);
        check(incoming != blob, "a reservation is not the committed location",
              "incoming and blob are the same path, so a half-written file would sit "
              "under the name every other tool trusts");
        check(!fs::exists(fs::u8path(incoming)), "placing writes nothing");

        // Still not findable: a reservation is not bytes.
        check(!store.find(kDigest, found), "a placed but uncommitted blob is not findable",
              "Find returned bytes that have not arrived; every tool on this machine "
              "would now trust a blob named by a digest it does not have");

        // Committing nothing must refuse rather than invent success.
        bool refused = false;
        try {
            store.commit(placed);
        } catch (const NotFound&) {
            refused = true;
        }
        check(refused, "committing bytes that were never written is refused");

        // Write the bytes where the store said, and commit.
        {
            std::ofstream out(fs::u8path(incoming), std::ios::binary);
            out << std::string(4096, 'x');
        }
        store.commit(placed);

        check(store.find(kDigest, found), "committed bytes are findable");
        check(found.size() == 4096, "the store reports the size it actually holds");
        check(!fs::exists(fs::u8path(incoming)), "committing moves the reservation, it does not copy");

        // A second commit is a retry, not an error.
        bool second_ok = true;
        try {
            store.commit(placed);
        } catch (const std::exception&) {
            second_ok = false;
        }
        check(second_ok, "committing twice is a retry rather than a failure");

        // Placing something already here returns the bytes, not a new
        // reservation -- asking twice must not start a second transfer.
        const Ref again = store.place(kDigest, 4096);
        check(again.size() == 4096, "placing bytes the store already has returns them");

        // Meaning, not spelling. The bare hex names the same artifact.
        const std::string bare = std::string(kDigest).substr(std::string("sha256:").size());
        check(store.find(bare, found), "a bare digest finds what a labelled one placed",
              "this is the comparison that deleted a correct 1.5 GB download");

        // A digest nobody can read is refused rather than given a location.
        bool no_guessing = false;
        try {
            store.place("not-a-digest", 1);
        } catch (const StorageError&) {
            no_guessing = true;
        }
        check(no_guessing, "a store refuses to name a location for unidentifiable bytes",
              "inventing a name here is what produced 116 GB of duplicates");

    } catch (const std::exception& e) {
        bad("the store threw", e.what());
    }

    fs::remove_all(root, ec);

    std::printf("\n  %s\n", failures == 0 ? "all passed" : "FAILURES");
    return failures == 0 ? 0 : 1;
}
