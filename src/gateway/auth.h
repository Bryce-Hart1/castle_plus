// Claude — Date 10/31/2026
// Per-route credential check, run on the request head before anything is
// forwarded. Built for the vault route: swiftSearch has no auth of its own, so
// castle is the only thing standing between the internet and its data.
//
// A token file holds SHA-256 hashes, never the tokens themselves:
//
//   # class  sha256(token), hex
//   rw  9f86d081884c7d659a2feaa0c55ad015a3bf4f1b2b0b822cd15d6c15b0f00a08
//   ro  60303ae22b998861bce3b28f33eec1be758a213c86c93c076dbe9f558c11c752
//
// Clients send the token as `Authorization: Bearer <token>`, or as the password
// of `Authorization: Basic` (the username is ignored) so a browser can prompt
// for it and then play a video straight from its URL.
//
// Shared by every worker loop. check() takes a shared lock and reload() an
// exclusive one, the same arrangement as TlsContext, so SIGHUP can revoke a
// token without restarting castle (which would also bounce the backends).
#pragma once

#include <cstddef>
#include <shared_mutex>
#include <string>
#include <vector>

namespace castle {

enum class Access {
    None,       // no credential, or one that matches no entry
    ReadOnly,   // GET / HEAD only
    ReadWrite,  // every method
};

class TokenSet {
public:
    // Read the token file. Returns false + sets err on any malformed line, or if
    // the file is writable by group/other (anyone who can write it can mint a
    // credential).
    bool load(const std::string& path, std::string& err);

    // Re-read the same file and swap it in live. On failure the current set is
    // kept. Returns false + sets err on failure.
    bool reload(std::string& err);

    // Decide what the raw Authorization header value grants. Every entry is
    // compared, in constant time, whatever matches first.
    Access check(const char* value, size_t len) const;

    const std::string& path() const { return path_; }

private:
    struct Entry {
        Access access;
        unsigned char hash[32];
    };

    static bool parse(const std::string& path, std::vector<Entry>& out,
                      std::string& err);

    std::string path_;
    std::vector<Entry> entries_;
    mutable std::shared_mutex mutex_;
};

}  // namespace castle
