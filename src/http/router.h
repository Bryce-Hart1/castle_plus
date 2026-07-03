// Claude — Date 06/19/2026
// Host + longest-path-prefix routing table, loaded from routes.conf. Immutable
// after load(), so it's shared across all worker loops by const reference with
// no locking. (Hot-reload lands later and will swap the whole table atomically.)
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace castle {

struct Backend {
    std::string host;  // IPv4 literal for now (DNS resolution is a follow-up)
    uint16_t port = 0;
};

struct Route {
    std::string name;
    std::string host;              // lowercased; empty = match any Host
    std::string path_prefix = "/";  // request path must start with this

    Backend backend;
};

class Router {
public:
    // Parse an INI-style routes file. On failure returns false + sets `err`.
    bool load(const std::string& path, std::string& err);

    // Pick the backend for (Host header, request path). Most specific (longest
    // path_prefix) wins; a host-specific route beats a wildcard at equal length.
    std::optional<Backend> match(const std::string& host,
                                 const std::string& path) const;

    size_t size() const { return routes_.size(); }

private:
    std::vector<Route> routes_;
};

}  // namespace castle
