// Which downloader this machine has, discovered rather than configured.
//
// The download layer moves bytes with libcurl, which stops when this process
// stops. On a machine where a system downloader is running, the same transfer
// can outlive Lemonade entirely — and on a machine where one is not, nothing
// should change.
//
// So this is a provider question, not a feature flag. The caller asks "is there
// a better downloader here?" and gets an answer derived from the machine, never
// from a setting a user had to find. Nothing in Lemonade names a NAS, a
// service, or a host: the supervisor decides where the work actually goes, one
// hop further down, and this layer does not know or care.
//
// Discovery has two steps and both are file reads:
//
//   1. The machine's store, from the location the OS designates for
//      configuration. Whatever set a system downloader up wrote it there once,
//      and every process afterwards reads it.
//   2. A heartbeat in that store. A supervisor refreshes it on every sweep, so
//      a timestamp -- not a pid -- is what proves one is alive now.
//
// A pid file would only prove a process existed when the file was written. The
// heartbeat also carries the interval it promises, so a reader can decide what
// stale means without guessing.

#pragma once

#include <cstdint>
#include <string>

namespace lemon {
namespace job {

// Where the machine keeps job records that tools share. Empty when nothing has
// been configured, which is the ordinary case and not an error.
std::string machine_store();

// What a live supervisor looks like. Absent means this machine has none, and
// the caller downloads for itself exactly as it always did.
struct Supervisor {
    std::string owner;  // program@host:pid, for a status line
    std::string tier;   // what IT delegates to, purely so a human can see the
                        // whole chain. Lemonade never acts on this.
    bool alive = false;
};

// Read the heartbeat and decide whether it is fresh. A supervisor that was
// killed leaves its heartbeat behind, so trusting the file's existence would
// hand work to a directory nobody is watching -- which looks exactly like a
// download that started and then never progressed.
Supervisor supervisor_of(const std::string& store_root);

// Ask a supervisor to sweep now instead of at its next tick.
//
// A hint, not a message: it carries no job id and no payload. If it is lost,
// refused, or unix sockets do not work here, the supervisor's own sweep finds
// the work anyway and the only cost is the wait. That is what makes it safe to
// add -- a channel that carried state would be a second source of truth.
void nudge(const std::string& store_root);

}  // namespace job
}  // namespace lemon
