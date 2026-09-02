#include <lemon/storage/storage.h>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <system_error>

namespace fs = std::filesystem;

namespace lemon {
namespace storage {

Ref make_ref(const std::string& store, const std::string& digest, const std::string& locator,
             std::int64_t size) {
    Ref r;
    r.store_ = store;
    r.digest_ = digest;
    r.locator_ = locator;
    r.size_ = size;
    return r;
}

const std::string& locator_of(const Ref& r) { return r.locator_; }

std::string normal_digest(const std::string& raw) {
    std::string s;
    for (char c : raw) {
        if (c != ' ' && c != '\t' && c != '\n' && c != '\r') {
            s.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
        }
    }
    for (const char* prefix : {"sha256:", "sha256-"}) {
        const std::string p(prefix);
        if (s.rfind(p, 0) == 0) {
            s = s.substr(p.size());
            break;
        }
    }
    if (s.size() != 64) {
        return "";
    }
    for (char c : s) {
        const bool hex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
        if (!hex) {
            return "";
        }
    }
    return "sha256:" + s;
}

namespace {

// "sha256:<hex>" as a filename. Hyphen and not colon, because a colon is not a
// legal character in an NTFS filename — and because this is the spelling Ollama
// already uses, so its blobs directory and this one are the same shape.
std::string blob_name(const std::string& normalized) {
    std::string out = normalized;
    const std::size_t colon = out.find(':');
    if (colon != std::string::npos) {
        out[colon] = '-';
    }
    return out;
}

}  // namespace

Content::Content(const std::string& name, const std::string& root) : name_(name), root_(root) {
    if (root.empty()) {
        throw StorageError("storage: a content store needs a root");
    }
    std::error_code ec;
    fs::create_directories(fs::u8path(root) / "blobs", ec);
    fs::create_directories(fs::u8path(root) / "incoming", ec);
    if (ec) {
        throw StorageError("storage: could not prepare " + root + ": " + ec.message());
    }
}

bool Content::find(const std::string& digest, Ref& out) const {
    const std::string normalized = normal_digest(digest);
    if (normalized.empty()) {
        return false;
    }
    const fs::path blob = fs::u8path(root_) / "blobs" / blob_name(normalized);
    std::error_code ec;
    const auto size = fs::file_size(blob, ec);
    if (ec) {
        return false;
    }
    out = make_ref(name_, normalized, blob.u8string(), static_cast<std::int64_t>(size));
    return true;
}

Ref Content::place(const std::string& digest, std::int64_t size) {
    const std::string normalized = normal_digest(digest);
    if (normalized.empty()) {
        // A content-addressed store cannot name a location for bytes nobody can
        // identify. Refusing is the point: inventing a name here is exactly the
        // behaviour that produced 116 GB of duplicates across four tools.
        throw StorageError("storage: place needs a digest, got " + digest);
    }

    // Already here: return the bytes rather than a reservation, so asking twice
    // does not start a second transfer. Same rule the download service applies
    // to jobs, one layer down.
    Ref existing;
    if (find(normalized, existing)) {
        return existing;
    }
    const fs::path incoming = fs::u8path(root_) / "incoming" / blob_name(normalized);
    return make_ref(name_, normalized, incoming.u8string(), size);
}

std::string Content::path(const Ref& r) const {
    const std::string normalized = normal_digest(r.digest());
    if (normalized.empty()) {
        throw StorageError("storage: this ref carries no digest");
    }
    return (fs::u8path(root_) / "blobs" / blob_name(normalized)).u8string();
}

std::string Content::incoming_path(const Ref& r) const {
    const std::string normalized = normal_digest(r.digest());
    if (normalized.empty()) {
        throw StorageError("storage: this ref carries no digest");
    }
    return (fs::u8path(root_) / "incoming" / blob_name(normalized)).u8string();
}

void Content::commit(const Ref& r) {
    const std::string normalized = normal_digest(r.digest());
    if (normalized.empty()) {
        throw StorageError("storage: this ref carries no digest");
    }
    const fs::path incoming = fs::u8path(incoming_path(r));
    const fs::path blob = fs::u8path(path(r));

    std::error_code ec;
    if (!fs::exists(incoming, ec)) {
        // Already committed is success: a second commit of the same bytes is a
        // retry, not an error, and the caller cannot always know which it is.
        if (fs::exists(blob, ec)) {
            return;
        }
        throw NotFound("storage: nothing placed at " + incoming.u8string() + " to commit");
    }

    // Rename, so the blob appears under its final name only once it is whole.
    // Anything less and a half-written blob sits there named by a digest it does
    // not have, and every other tool on this machine trusts the name.
    fs::rename(incoming, blob, ec);
    if (ec) {
        throw StorageError("storage: committing " + blob.u8string() + ": " + ec.message());
    }
}

}  // namespace storage
}  // namespace lemon
