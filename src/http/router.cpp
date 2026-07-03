// Claude — Date 06/19/2026
// Routes parser + matcher. Same small strict INI style as the services manifest.
#include "http/router.h"

#include <cctype>
#include <cstdlib>
#include <fstream>
#include <string>

namespace castle {

namespace {

std::string trim(const std::string& s) {
    size_t b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return "";
    size_t e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(c));
    return s;
}

}  // namespace

bool Router::load(const std::string& path, std::string& err) {
    std::ifstream f(path);
    if (!f) {
        err = "cannot open routes file: " + path;
        return false;
    }

    std::vector<Route> result;
    Route* cur = nullptr;
    std::string line;
    int lineno = 0;

    auto fail = [&](const std::string& msg) {
        // Built stepwise (not a chained a+b+c+d) to dodge a GCC -Wrestrict
        // false positive on the temporaries.
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

        if (t.front() == '[') {
            if (t.back() != ']') return fail("malformed section header");
            std::string name = trim(t.substr(1, t.size() - 2));
            if (name.empty()) return fail("empty section name");
            Route r;
            r.name = name;  // path_prefix defaults to "/" (matches all)
            result.push_back(std::move(r));
            cur = &result.back();
            continue;
        }

        size_t eq = t.find('=');
        if (eq == std::string::npos) return fail("expected 'key = value'");
        if (!cur) return fail("key outside of any [section]");

        std::string key = trim(t.substr(0, eq));
        std::string val = trim(t.substr(eq + 1));

        if (key == "host") {
            cur->host = lower(val);
        } else if (key == "path") {
            if (!val.empty()) cur->path_prefix = val;  // else keep default "/"
        } else if (key == "backend") {
            size_t colon = val.rfind(':');
            if (colon == std::string::npos)
                return fail("backend must be host:port");
            cur->backend.host = trim(val.substr(0, colon));
            long p = std::strtol(val.substr(colon + 1).c_str(), nullptr, 10);
            if (p <= 0 || p > 65535) return fail("backend port out of range");
            cur->backend.port = static_cast<uint16_t>(p);
        } else {
            return fail("unknown key '" + key + "'");
        }
    }

    for (const auto& r : result) {
        if (r.backend.host.empty() || r.backend.port == 0)
            return (err = "route '" + r.name + "' is missing a valid backend",
                    false);
    }

    routes_ = std::move(result);
    return true;
}

std::optional<Backend> Router::match(const std::string& host,
                                     const std::string& path) const {
    // Normalize the Host header: drop any :port, lowercase.
    std::string h = host;
    size_t colon = h.find(':');
    if (colon != std::string::npos) h = h.substr(0, colon);
    h = lower(h);

    const Route* best = nullptr;
    for (const auto& r : routes_) {
        if (!r.host.empty() && r.host != h) continue;
        if (path.rfind(r.path_prefix, 0) != 0) continue;  // starts_with
        if (!best || r.path_prefix.size() > best->path_prefix.size() ||
            (r.path_prefix.size() == best->path_prefix.size() &&
             best->host.empty() && !r.host.empty())) {
            best = &r;
        }
    }

    if (best) return best->backend;
    return std::nullopt;
}

}  // namespace castle
