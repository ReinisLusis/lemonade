#pragma once

// storage — where bytes live, addressed by what they are rather than by what
// somebody decided to call them.
//
// This is the C++ side of the abstraction the Go package implements. As with
// job, the two are not ports of one another: they agree about the layout on
// disk and about what an operation MEANS, and each API looks like its own
// language.
//
// # The problem, measured rather than assumed
//
// One ordinary machine, four tools, four stores that know nothing about each
// other:
//
//     LM Studio          44 GB   names files by filename only   NOT dedupable
//     Lemonade           26 GB   content hash
//     Ollama             23 GB   content hash
//     HuggingFace cache  23 GB   content hash
//
// 116 GB, with the same weights in more than one of them under different names.
// Not because the problem is hard — because there is no shared interface.
//
// # Why this exists in C++ specifically, and what it is meant to break
//
// The download layer was adopted here and the storage layer was not, and the
// consequence was not theoretical. Lemonade builds its own destinations:
//
//     std::string partial_path = output_path + ".partial";
//
// nineteen times in model_manager.cpp alone. It then hands those strings DOWN
// into a download spec — so the application tells the abstraction where bytes
// go, which is the defect at the other end of the same call the download layer
// was supposed to fix. Everything downstream follows: code that scans for
// `.partial` files, code that deletes them, code that skips them when listing
// models. A delete raced a supervisor that owned the very bytes it was removing,
// and only a Windows file lock turned silent corruption into an error message.
//
// None of that could be caught by review, because there was nothing to catch it
// WITH: no C++ type existed that an application could be required to hold
// instead of a path. That is what this header is. A Ref carries no path a caller
// can concatenate, so the line above stops compiling rather than stops working.
//
// # What this must not become
//
// Not a filesystem. A Ref is opaque, and a store that happens to be a directory
// says so through the Local capability rather than by returning paths from the
// interface — the same shape job::LocalStore has, for the same reason: the
// binding must not be able to name itself upward.

#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace lemon {
namespace storage {

class StorageError : public std::runtime_error {
public:
    explicit StorageError(const std::string& what) : std::runtime_error(what) {}
    virtual const char* name() const noexcept { return "StorageError"; }
};

// Thrown by place() on a store that can only be read. The foreign stores are
// all like this: we look inside Ollama's blobs, and we do not write there,
// because their layout is theirs to change.
class ReadOnly : public StorageError {
public:
    explicit ReadOnly(const std::string& what) : StorageError(what) {}
    const char* name() const noexcept override { return "ReadOnly"; }
};

class NotFound : public StorageError {
public:
    explicit NotFound(const std::string& what) : StorageError(what) {}
    const char* name() const noexcept override { return "NotFound"; }
};

// Ref is a handle to bytes in a store. Opaque on purpose.
//
// The application that asked for a download does not learn a path from it, and
// could not act on one if it did — the process that finally writes those bytes
// may be a Windows service under its own account, or a daemon on a NAS that
// mounts this store somewhere else entirely.
//
// The locator is private and there is no accessor on the class. A binding
// recovers it through locator_of() below, which is deliberately awkward to
// reach for and named so that it is obvious in review when somebody has.
class Ref {
public:
    Ref() = default;

    // Which store this came from, so a caller holding several refs can say
    // where each one is without asking.
    const std::string& store() const { return store_; }
    // What the bytes are, "sha256:<hex>", when it is known. A store may hold
    // bytes whose digest nobody has computed.
    const std::string& digest() const { return digest_; }
    // How big, or 0 when unknown.
    std::int64_t size() const { return size_; }

    bool empty() const { return locator_.empty(); }

private:
    friend Ref make_ref(const std::string&, const std::string&, const std::string&, std::int64_t);
    friend const std::string& locator_of(const Ref&);

    std::string store_;
    std::string digest_;
    std::int64_t size_ = 0;
    // How the store's own binding finds these bytes again. Private: a caller
    // that could read it would start joining paths onto it, which is the whole
    // behaviour this type exists to prevent.
    std::string locator_;
};

// make_ref is how a Store constructs a Ref. For bindings, not applications.
Ref make_ref(const std::string& store, const std::string& digest, const std::string& locator,
             std::int64_t size);

// locator_of returns a ref's internal location, for the store that produced it.
//
// Exported for bindings, not for applications: it is how a Local implementation
// turns a ref back into something it can act on. An application that reaches
// for this is doing the thing this header exists to stop.
const std::string& locator_of(const Ref& r);

// Store is somewhere bytes live.
//
// Deliberately small. Everything a store must be able to do is here; everything
// only SOME stores can do is a capability below.
class Store {
public:
    virtual ~Store() = default;

    // How this store is identified to a person. "ollama", "nas".
    virtual std::string name() const = 0;

    // Where these bytes already are. The digest is authoritative; a store that
    // cannot answer by digest cannot participate, and LM Studio — the largest
    // store on the machine that was measured — is exactly that case.
    //
    // It must not hash anything. A hit is trusted only as far as the store's own
    // naming convention goes, which is why whoever copies from it still
    // verifies: Ollama's blobs hash to their own filenames, and Ollama does not
    // check them, so a corrupt blob there is a real possibility.
    virtual bool find(const std::string& digest, Ref& out) const = 0;

    // Reserve somewhere for bytes this store does not have yet.
    //
    // Reserving is not writing. The caller may be handing this to a service that
    // will do the writing much later, under a different account, after this
    // process has exited — so place() settles WHERE and nothing else.
    virtual Ref place(const std::string& digest, std::int64_t size) = 0;
};

// Local is an OPTIONAL capability: a store whose binding is a filesystem can
// name a location on it.
//
// It exists because bytes are not always moved by us. BITS writes under its own
// service account and hands the file over on completion; a NAS daemon writes on
// the far side of a share. Neither can be given a stream, so a destination they
// can act on has to be expressible — and that destination is a path.
//
// A capability rather than part of Store for the same reason job::LocalStore is
// separate: a store backed by a service answers no, and a caller then has to
// have a real answer for that case instead of assuming a directory exists.
class Local {
public:
    virtual ~Local() = default;

    // Where this ref is on this machine's filesystem, once it is committed.
    virtual std::string path(const Ref& r) const = 0;

    // Where bytes may be written while they are still arriving.
    //
    // This is the ONLY supported way to obtain such a location. It is here, on
    // a capability that has to be asked for, rather than spelled `final +
    // ".partial"` at nineteen call sites — because a partial is not a filename
    // convention, it is a reservation that belongs to whoever holds the job.
    virtual std::string incoming_path(const Ref& r) const = 0;
};

// Writable is an OPTIONAL capability: a store that can take bytes directly, for
// a caller holding them rather than delegating the transfer.
class Writable {
public:
    virtual ~Writable() = default;

    // Move already-written bytes into the store under their digest, making them
    // findable. Until this is called, a placed ref is a reservation and nothing
    // more — the same two-phase shape the job layer uses for TRANSFERRED, and
    // for the same reason: the writer and the consumer are not the same process.
    virtual void commit(const Ref& r) = 0;
};

// Content is a store of our own, addressed by digest.
//
// Layout:
//
//     <root>/blobs/sha256-<hex>       the bytes, once committed
//     <root>/incoming/sha256-<hex>    a reservation, until it is
//
// The same convention Ollama uses, deliberately: this is not a new format, it is
// the one two of the four measured stores already agreed on independently. A
// tool that reads Ollama's blobs directory can read this one.
//
// Placing and committing are separate because the process that eventually puts
// bytes there may be a Windows service under its own account, running long after
// the process that asked has exited — so a reservation has to be a location
// rather than an open file. And because a half-written blob under its final name
// is a corrupt blob that every other tool on the machine will now trust: it is
// named by a digest it does not have.
class Content : public Store, public Local, public Writable {
public:
    Content(const std::string& name, const std::string& root);

    std::string name() const override { return name_; }
    bool find(const std::string& digest, Ref& out) const override;
    Ref place(const std::string& digest, std::int64_t size) override;
    std::string path(const Ref& r) const override;
    std::string incoming_path(const Ref& r) const override;
    void commit(const Ref& r) override;

private:
    std::string name_;
    std::string root_;
};

// A digest reduced to the part that carries the meaning, "sha256:<hex>", or
// empty when it is not one.
//
// The same normalisation the download spec readers perform, and for the same
// reason: one implementation writes "sha256:<hex>" and another the bare hex,
// and comparing the spelling instead of the meaning once deleted a correct
// 1.5 GB download.
std::string normal_digest(const std::string& raw);

}  // namespace storage
}  // namespace lemon
