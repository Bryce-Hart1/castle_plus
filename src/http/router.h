// Claude — Date 06/19/2026 last changed: 10/31/2026 by: Claude
// Host + longest-path-prefix routing table, loaded from routes.conf. Immutable
// after load(), so it's shared across all worker loops by const reference with
// no locking. (Hot-reload lands later and will swap the whole table atomically.)
// The one exception is each route's TokenSet, which locks internally so SIGHUP
// can swap tokens in place.
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "gateway/auth.h"

namespace castle {

struct Backend {
    std::string host;  // IPv4 literal for now (DNS resolution is a follow-up)
    uint16_t port = 0;
};

struct Route {
    std::string name;
    std::string host;  // lowercased; empty = match any Host
    std::string path_prefix = "/";  // request path must start with this

    Backend backend;

    // ---- per-route policy, applied before anything is forwarded ----
    std::optional<size_t> max_body;   // body cap; unset = global --max-body
    std::vector<std::string> allow;   // literal path prefixes; empty = all
    std::shared_ptr<TokenSet> auth;   // null = no credential required

    // Is `path` inside this route's allowlist?
    bool allows(const std::string& path) const;
};

class Router {
public:
    // Parse an INI-style routes file. On failure returns false + sets `err`.
    bool load(const std::string& path, std::string& err);

    // Pick the backend for (Host header, request path). Most specific (longest
    // path_prefix) wins; a host-specific route beats a wildcard at equal length.
    // Null when nothing matches.
    const Route* match(const std::string& host, const std::string& path) const;

    // Re-read every route's token file (SIGHUP). A file that fails to parse
    // keeps its current tokens. Returns false + sets err if any failed.
    bool reload_auth(std::string& err) const;

    size_t size() const { return routes_.size(); }
    const std::vector<Route>& routes() const { return routes_; }

private:
    std::vector<Route> routes_;
};

}  // namespace castle
