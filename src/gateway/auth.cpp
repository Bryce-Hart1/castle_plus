// Claude — Date 10/31/2026
// Token file parser + Authorization check. Strict like the other config
// parsers: a typo in the file that decides who can delete the vault should
// fail loudly.
#include "gateway/auth.h"

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <sys/stat.h>

#include <cctype>
#include <fstream>
#include <mutex>
#include <string>

namespace castle {

namespace {

std::string trim(const std::string& s) {
    size_t b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return "";
    size_t e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

int hex_value(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

int base64_value(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

// Standard base64 with optional '=' padding. Anything else is a failure, not
// something to skip over.
bool base64_decode(const char* in, size_t len, std::string& out) {
    while (len > 0 && in[len - 1] == '=') --len;
    out.clear();
    unsigned acc = 0;
    int bits = 0;
    for (size_t i = 0; i < len; ++i) {
        int v = base64_value(in[i]);
        if (v < 0) return false;
        acc = (acc << 6) | static_cast<unsigned>(v);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out += static_cast<char>((acc >> bits) & 0xFF);
        }
    }
    return true;
}

// Case-insensitive "<scheme> " prefix match; on success `rest` is what follows
// the spaces.
bool take_scheme(const char* v, size_t len, const char* scheme,
                 const char*& rest, size_t& rest_len) {
    size_t n = std::char_traits<char>::length(scheme);
    if (len <= n || v[n] != ' ') return false;
    for (size_t i = 0; i < n; ++i)
        if (std::tolower(static_cast<unsigned char>(v[i])) != scheme[i])
            return false;
    size_t i = n;
    while (i < len && v[i] == ' ') ++i;
    rest = v + i;
    rest_len = len - i;
    return true;
}

}  // namespace

bool TokenSet::parse(const std::string& path, std::vector<Entry>& out,
                     std::string& err) {
    struct stat st{};
    if (::stat(path.c_str(), &st) != 0) {
        err = "cannot open token file: " + path;
        return false;
    }
    if (st.st_mode & (S_IWGRP | S_IWOTH)) {
        err = "token file is writable by group/other: " + path;
        return false;
    }

    std::ifstream f(path);
    if (!f) {
        err = "cannot open token file: " + path;
        return false;
    }

    std::vector<Entry> result;
    std::string line;
    int lineno = 0;

    auto fail = [&](const std::string& msg) {
        // Stepwise concat to dodge a GCC -Wrestrict false positive.
        err = path;
        err += ':';
        err += std::to_string(lineno);
        err += ": ";
        err += msg;
        return false;
    };

    while (std::getline(f, line)) {
        ++lineno;
        std::string t = trim(line);
        if (t.empty() || t[0] == '#' || t[0] == ';') continue;

        size_t sp = t.find_first_of(" \t");
        if (sp == std::string::npos) return fail("expected '<rw|ro> <sha256 hex>'");
        std::string cls = t.substr(0, sp);
        std::string hex = trim(t.substr(sp));

        Entry e{};
        if (cls == "rw") {
            e.access = Access::ReadWrite;
        } else if (cls == "ro") {
            e.access = Access::ReadOnly;
        } else {
            return fail("token class must be 'rw' or 'ro'");
        }

        if (hex.size() != 64)
            return fail("expected a 64-character sha256 hex digest");
        for (size_t i = 0; i < 32; ++i) {
            int hi = hex_value(hex[2 * i]);
            int lo = hex_value(hex[2 * i + 1]);
            if (hi < 0 || lo < 0) return fail("sha256 digest is not hex");
            e.hash[i] = static_cast<unsigned char>((hi << 4) | lo);
        }
        result.push_back(e);
    }

    out = std::move(result);
    return true;
}

bool TokenSet::load(const std::string& path, std::string& err) {
    std::vector<Entry> fresh;
    if (!parse(path, fresh, err)) return false;
    path_ = path;
    entries_ = std::move(fresh);
    return true;  // no lock: load runs before any worker thread starts
}

bool TokenSet::reload(std::string& err) {
    // Parse WITHOUT the lock — a broken edit must never disturb the live set.
    std::vector<Entry> fresh;
    if (!parse(path_, fresh, err)) return false;
    std::unique_lock<std::shared_mutex> lock(mutex_);
    entries_ = std::move(fresh);
    return true;
}

Access TokenSet::check(const char* value, size_t len) const {
    if (value == nullptr || len == 0) return Access::None;

    const char* rest = nullptr;
    size_t rest_len = 0;
    std::string token;
    if (take_scheme(value, len, "bearer", rest, rest_len)) {
        token.assign(rest, rest_len);
    } else if (take_scheme(value, len, "basic", rest, rest_len)) {
        std::string decoded;
        if (!base64_decode(rest, rest_len, decoded)) return Access::None;
        size_t colon = decoded.find(':');
        if (colon == std::string::npos) return Access::None;
        token = decoded.substr(colon + 1);  // username is ignored
    } else {
        return Access::None;
    }
    if (token.empty()) return Access::None;

    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int digest_len = 0;
    if (EVP_Digest(token.data(), token.size(), digest, &digest_len,
                   EVP_sha256(), nullptr) != 1 ||
        digest_len != 32)
        return Access::None;
    OPENSSL_cleanse(token.data(), token.size());

    // Walk every entry and keep the strongest match: no early exit, so the
    // time taken doesn't say which entry (if any) matched.
    Access best = Access::None;
    std::shared_lock<std::shared_mutex> lock(mutex_);
    for (const Entry& e : entries_) {
        if (CRYPTO_memcmp(digest, e.hash, 32) == 0 && e.access > best)
            best = e.access;
    }
    return best;
}

}  // namespace castle
