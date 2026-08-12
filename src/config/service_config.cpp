// Bryce Hart Aug 12 26
// Small, strict INI parser for the services manifest. Strict on purpose: a typo
// in a manifest that launches processes should fail loudly, not silently.
#include "config/service_config.h"

#include <cctype>
#include <cstdlib>
#include <fstream>
#include <string>

namespace castle {

namespace {

std::string trim(const std::string& str) {
    size_t b = str.find_first_not_of(" \t\r\n");
    if (b == std::string::npos){
    return "";
    }
    size_t e = str.find_last_not_of(" \t\r\n");
    return str.substr(b, e - b + 1);
}

bool parse_bool(const std::string& v, bool& out) {
    std::string s;
    for (char c : v) s += static_cast<char>(std::tolower(c));
    if (s == "true" || s == "1" || s == "yes" || s == "on") {
        out = true;
        return true;
    }
    if (s == "false" || s == "0" || s == "no" || s == "off"){
        out = false;
        return true;
    }
    return false;
}

// Whitespace-split with minimal double-quote support so args with spaces work.
std::vector<std::string> split_args(const std::string& v) {
    std::vector<std::string> out;
    std::string cur;
    bool in_quotes = false;
    bool have = false;
    for (char c : v) {
        if (c == '"') {
            in_quotes = !in_quotes;
            have = true;
        } else if (!in_quotes && (c == ' ' || c == '\t')) {
            if (have) {
                out.push_back(cur);
                cur.clear();
                have = false;
            }
        } else {
            cur += c;
            have = true;
        }
    }

    if(have){
        out.push_back(cur);
    }
    return out;
}

}  // namespace

//parse service file and return if 
bool parse_services_file(const std::string& path, std::vector<ServiceConfig>& out, std::string& err) {
    std::ifstream f(path);
    if(!f) {
        err = "cannot open services manifest: " + path;
        return false;
    }

    std::vector<ServiceConfig> result;
    ServiceConfig* cur = nullptr;
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

    while(std::getline(f, line)){
        ++lineno;
        std::string t = trim(line);
        if (t.empty() || t[0] == '#' || t[0] == ';') continue;

        if (t.front() == '[') {
            if (t.back() != ']') return fail("malformed section header");
            std::string name = trim(t.substr(1, t.size() - 2));
            if (name.empty()) return fail("empty section name");
            result.push_back(ServiceConfig{});
            result.back().name = name;
            cur = &result.back();
            continue;
        }

        size_t eq = t.find('=');
        if (eq == std::string::npos) return fail("expected 'key = value'");
        if (!cur) return fail("key outside of any [section]");

        std::string key = trim(t.substr(0, eq));
        std::string val = trim(t.substr(eq + 1));

        if (key == "exec") {
            cur->exec = val;
        } else if (key == "args") {
            cur->args = split_args(val);
        } else if (key == "workdir") {
            cur->workdir = val;
        } else if (key == "autorestart") {
            if (!parse_bool(val, cur->autorestart))
                return fail("autorestart must be true/false");
        } else if (key == "health_tcp") {
            size_t colon = val.rfind(':');
            if (colon == std::string::npos)
                return fail("health_tcp must be host:port");
            cur->health_host = trim(val.substr(0, colon));
            long p = std::strtol(val.substr(colon + 1).c_str(), nullptr, 10);
            if (p <= 0 || p > 65535) return fail("health_tcp port out of range");
            cur->health_port = static_cast<uint16_t>(p);
        } else if (key == "health_interval") {
            cur->health_interval_sec = static_cast<int>(std::strtol(val.c_str(), nullptr, 10));
        } else if (key == "backoff_min") {
            cur->backoff_min_sec = static_cast<int>(std::strtol(val.c_str(), nullptr, 10));
        } else if (key == "backoff_max") {
            cur->backoff_max_sec = static_cast<int>(std::strtol(val.c_str(), nullptr, 10));
        } else {
            return fail("unknown key '" + key + "'");
        }
    }

    for(const auto& c : result){
        if (c.exec.empty()){
            return (err = "service '" + c.name + "' is missing 'exec'", false);
        }
        if (c.exec.front() != '/')
            return (err = "service '" + c.name + "' exec must be an absolute path", false);
    }

    out = std::move(result);
    return true;
}

}  // namespace castle
