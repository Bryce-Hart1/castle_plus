// Claude — Date 06/19/2026
// Parsed manifest entry for one supervised backend, plus a tiny dependency-free
// INI-style parser. Kept zero-dependency on purpose: config parsing isn't the
// interesting/risky part, and a small parser avoids dragging a TOML/JSON library
// onto the server. The struct is the seam — swap the parser later without touching
// the supervisor.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace castle {

struct ServiceConfig {
    std::string name;                 // section header [name]
    std::string exec;                 // absolute path to the binary (required)
    std::vector<std::string> args;    // argv after exec
    std::string workdir;              // optional chdir before exec

    bool autorestart = true;          // relaunch on exit?

    // Optional TCP health check (health_tcp = host:port). port == 0 disables.
    std::string health_host;
    uint16_t health_port = 0;
    int health_interval_sec = 5;

    // Exponential restart backoff bounds.
    int backoff_min_sec = 1;
    int backoff_max_sec = 30;
};

// Parse an INI-style services manifest. On success fills `out` and returns true;
// on any error returns false and sets `err` to a human-readable reason.
//
// Format:
//   [name]
//   exec = /usr/bin/foo
//   args = --flag value "quoted arg"
//   workdir = /srv/foo
//   autorestart = true
//   health_tcp = 127.0.0.1:9001
//   health_interval = 5
//   backoff_min = 1
//   backoff_max = 30
bool parse_services_file(const std::string& path,std::vector<ServiceConfig>& out, std::string& err);

}  // namespace castle
