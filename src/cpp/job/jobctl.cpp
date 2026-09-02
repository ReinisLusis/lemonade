// jobctl drives the job store from a shell, so the cross-language conformance
// test can be an actual script that runs three implementations against one
// directory rather than a mock of one talking to a mock of another.
//
// Deliberately ignorant of what a job IS: it takes a kind and a spec as raw
// JSON and never looks inside, which is the same contract the library keeps.

#include <cstdlib>
#include <iostream>
#include <lemon/job/store.h>
#include <map>
#include <string>
#include <vector>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

namespace {

using lemon::job::FileStore;
using lemon::job::Json;
using lemon::job::Record;

struct Args {
    std::string id;  // the first non-flag argument, in any position
    // Every non-flag argument after the id. `intent <id> <want>` needs a
    // second one, and dropping it silently would have made the want default
    // to whatever the handler chose rather than what the caller typed.
    std::vector<std::string> positional;
    std::map<std::string, std::string> flags;
};

Args parse_args(const std::vector<std::string>& argv) {
    Args a;
    for (std::size_t i = 0; i < argv.size(); ++i) {
        const std::string& token = argv[i];
        if (token.size() > 1 && token[0] == '-') {
            std::string name = token.substr(token[1] == '-' ? 2 : 1);
            const auto eq = name.find('=');
            if (eq != std::string::npos) {
                a.flags[name.substr(0, eq)] = name.substr(eq + 1);
                continue;
            }
            if (i + 1 < argv.size()) {
                a.flags[name] = argv[++i];
            } else {
                a.flags[name] = "";
            }
            continue;
        }
        if (a.id.empty()) {
            a.id = token;
        } else {
            a.positional.push_back(token);
        }
    }
    return a;
}

std::string flag(const Args& a, const std::string& name, const std::string& fallback = "") {
    const auto it = a.flags.find(name);
    return it == a.flags.end() ? fallback : it->second;
}

// Puts the checkpoint on one line. The record on disk is indented for humans,
// so a checkpoint read back out carries newlines — and this output is a
// conformance surface a harness parses, where "same value, different
// whitespace" counts as two implementations disagreeing.
std::string compact(const Record& r) {
    return r.checkpoint ? r.checkpoint->dump() : "none";
}

[[noreturn]] void fatal(const std::string& message) {
    std::cerr << "jobctl: " << message << std::endl;
    std::exit(1);
}

[[noreturn]] void usage() {
    std::cout << "X"
                 "   (JOB_STORE must be set)\n";
    std::cout << "  submit --kind K --spec '<json>' [--total N] [--requires a,b]\n";
    std::exit(2);
}

std::chrono::milliseconds ttl_from(const std::string& raw, double fallback_seconds) {
    if (raw.empty()) {
        return std::chrono::milliseconds(static_cast<long long>(fallback_seconds * 1000.0));
    }
    // Seconds as a plain number, matching Go and Python. A Go-style duration
    // suffix is accepted too, because an older harness may still pass "2s".
    std::size_t consumed = 0;
    double value = 0.0;
    try {
        value = std::stod(raw, &consumed);
    } catch (const std::exception&) {
        fatal("invalid --ttl \"" + raw + "\"");
    }
    const std::string suffix = raw.substr(consumed);
    double multiplier = 1000.0;
    if (suffix == "ms") {
        multiplier = 1.0;
    } else if (suffix == "m") {
        multiplier = 60000.0;
    } else if (suffix == "h") {
        multiplier = 3600000.0;
    } else if (!suffix.empty() && suffix != "s") {
        fatal("invalid --ttl \"" + raw + "\"");
    }
    return std::chrono::milliseconds(static_cast<long long>(value * multiplier));
}

Json parse_json_flag(const std::string& raw, const char* what) {
    try {
        return Json::parse(raw);
    } catch (const Json::exception& e) {
        fatal(std::string(what) + " is not valid JSON: " + e.what());
    }
}

std::vector<std::string> split_commas(const std::string& raw) {
    std::vector<std::string> out;
    std::size_t start = 0;
    for (;;) {
        const auto comma = raw.find(',', start);
        if (comma == std::string::npos) {
            out.push_back(raw.substr(start));
            return out;
        }
        out.push_back(raw.substr(start, comma - start));
        start = comma + 1;
    }
}

}  // namespace

int main(int argc, char** argv) {
#ifdef _WIN32
    // `show` must emit the record's own bytes. Text mode would turn every LF in
    // the canonical encoding into CRLF, so a harness diffing this output against
    // the file on disk would see two implementations disagreeing over nothing.
    _setmode(_fileno(stdout), _O_BINARY);
#endif
    if (argc < 2) {
        usage();
    }
    const std::string command = argv[1];
    const std::vector<std::string> rest(argv + 2, argv + argc);
    const Args args = parse_args(rest);

    const char* root = std::getenv("JOB_STORE");
    if (root == nullptr || *root == '\0') {
        fatal("JOB_STORE is not set");
    }

    try {
        FileStore store(root);

        if (command == "submit") {
            Record r;
            r.kind = flag(args, "kind");
            r.spec = parse_json_flag(flag(args, "spec", "{}"), "--spec");
            const std::string total = flag(args, "total");
            if (!total.empty()) {
                r.progress.total = std::stoll(total);
            }
            const std::string requires_flag = flag(args, "requires");
            if (!requires_flag.empty()) {
                r.requires_capabilities = split_commas(requires_flag);
            }
            std::cout << store.submit(std::move(r)) << std::endl;

        } else if (command == "claim") {
            if (args.id.empty()) {
                fatal("claim needs a job id");
            }
            const Record r =
                store.claim(args.id, flag(args, "owner"), ttl_from(flag(args, "ttl"), 30.0));
            // The epoch and the predecessor's checkpoint are what a new owner needs.
            std::cout << "epoch=" << r.lease.epoch << " state=" << r.state
                      << " checkpoint=" << compact(r) << std::endl;

        } else if (command == "progress") {
            if (args.id.empty()) {
                fatal("progress needs a job id");
            }
            const std::string done = flag(args, "done", "0");
            const std::string checkpoint = flag(args, "checkpoint");
            const Record r = store.update(
                args.id, std::stoll(flag(args, "epoch", "0")), [&](Record& rec) {
                    rec.progress.done = std::stoll(done);
                    rec.progress.updated_at = lemon::job::Clock::now();
                    if (!checkpoint.empty()) {
                        rec.checkpoint = parse_json_flag(checkpoint, "--checkpoint");
                    }
                });
            std::cout << "done=" << r.progress.done << " checkpoint=" << compact(r) << std::endl;

        } else if (command == "finish") {
            if (args.id.empty()) {
                fatal("finish needs a job id");
            }
            const std::string wanted = flag(args, "state", lemon::job::state::kTransferred);
            const Record r =
                store.update(args.id, std::stoll(flag(args, "epoch", "0")), [&](Record& rec) {
                    if (!lemon::job::is_valid_state(wanted)) {
                        throw lemon::job::Invalid("invalid state \"" + wanted + "\"");
                    }
                    rec.state = wanted;
                });
            std::cout << "state=" << r.state << std::endl;

        } else if (command == "show") {
            if (args.id.empty()) {
                fatal("show needs a job id");
            }
            std::cout << store.load(args.id).encode() << std::flush;

        } else if (command == "intent") {
            // A command rather than a flag on claim, because the whole point is
            // that the caller is NOT the worker: no epoch is presented, and
            // none is needed.
            if (args.id.empty() || args.positional.empty()) {
                fatal("usage: jobctl intent <id> <run|pause|cancel> [--by who]");
            }
            const std::string by = flag(args, "by", "jobctl-cpp");
            const Record r = store.set_intent(args.id, args.positional.front(), by);
            std::cout << r.id << " " << r.wants() << std::endl;

        } else if (command == "orphans") {
            for (const Record& r : store.orphans()) {
                std::cout << r.id << " kind=" << r.kind << " state=" << r.state
                          << " checkpoint=" << compact(r) << std::endl;
            }

        } else {
            usage();
        }
    } catch (const lemon::job::JobError& e) {
        fatal(std::string(e.name()) + ": " + e.what());
    } catch (const std::exception& e) {
        fatal(e.what());
    }
    return 0;
}
