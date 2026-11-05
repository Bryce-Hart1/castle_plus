// Claude — Date 11/05/2026
// See swiftsearch_status.hpp for what each line reads and why none of it needs
// swiftSearch's admin route.
#include "control/swiftsearch_status.hpp"

#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <exception>
#include <fstream>
#include <iterator>
#include <optional>
#include <sstream>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#include "net/socket.h"
#include "supervisor/supervisor.h"
#include "util/uptime.hpp"

namespace castle {

namespace {

namespace fs = std::filesystem;

constexpr int kHealthzTimeoutMs = 500;
constexpr auto kCpuSample = std::chrono::milliseconds(200);
constexpr std::uintmax_t kLogTailBytes = 64 * 1024;
constexpr std::size_t kLogLinesShown = 5;
constexpr char kUnavailable[] = "unavailable";
constexpr char kLogFile[] = "swiftSearchLogs";  // written into its working dir

// ---- formatting -----------------------------------------------------------

// "  label:    value\n", with the value in the same column as systemstatus.
void row(std::string& out, const char* label, const std::string& value) {
    char head[32];
    std::snprintf(head, sizeof(head), "  %-11s", (std::string(label) + ":").c_str());
    out += head;
    out += value;
    out += '\n';
}

// 1024-based, one decimal: "182.4 MB".
std::string human_bytes(std::uintmax_t b) {
    static const char* kUnits[] = {"B", "KB", "MB", "GB", "TB"};
    double v = static_cast<double>(b);
    int u = 0;
    while (v >= 1024.0 && u < 4) {
        v /= 1024.0;
        ++u;
    }
    char buf[32];
    if (u == 0) {
        std::snprintf(buf, sizeof(buf), "%ju B", b);
    } else {
        std::snprintf(buf, sizeof(buf), "%.1f %s", v, kUnits[u]);
    }
    return buf;
}

std::string plural(long long n, const char* one, const char* many) {
    return std::to_string(n) + " " + (n == 1 ? one : many);
}

std::string errno_reason(int e) {
    return e == ENOENT ? "not found" : std::strerror(e);
}

// ---- the service's own command line --------------------------------------

// What swiftSearch was told on its command line, with its own defaults (its
// README, "Options") for anything left out.
struct SwiftArgs {
    std::string store = "swiftsearch_data";
    std::string bind = "127.0.0.1";
    long port = 8080;
    std::string threads;  // as written; empty = one per CPU
    std::string mem_cap = "64M";
    bool admin = false;
};

SwiftArgs parse_swift_args(const std::vector<std::string>& args) {
    SwiftArgs a;
    for (std::size_t i = 0; i < args.size(); ++i) {
        const std::string& f = args[i];
        const bool has_value = i + 1 < args.size();
        if (f == "--store" && has_value) {
            a.store = args[++i];
        } else if (f == "--bind" && has_value) {
            a.bind = args[++i];
        } else if ((f == "-p" || f == "--port") && has_value) {
            a.port = std::strtol(args[++i].c_str(), nullptr, 10);
        } else if ((f == "-j" || f == "--threads") && has_value) {
            a.threads = args[++i];
        } else if (f == "--mem-cap" && has_value) {
            a.mem_cap = args[++i];
        } else if (f == "--admin") {
            a.admin = true;
        }
    }
    return a;
}

bool is_loopback(const std::string& addr) {
    return addr.rfind("127.", 0) == 0 || addr == "localhost" || addr == "::1";
}

// The folder swiftSearch runs in: its workdir, or castle's own (inherited
// across fork when workdir is unset).
fs::path run_dir(const ServiceConfig& cfg) {
    if (!cfg.workdir.empty()) return cfg.workdir;
    std::error_code ec;
    fs::path cwd = fs::current_path(ec);
    return ec ? fs::path(".") : cwd;
}

// ---- process --------------------------------------------------------------

std::string process_line(const Supervisor::ServiceSnapshot& s) {
    std::string out = to_string(s.state);
    if (s.state == ServiceState::Running && s.pid > 0) {
        out += ", pid " + std::to_string(s.pid);
        if (s.uptime_sec >= 0) out += ", up " + human_duration(s.uptime_sec);
    } else if (s.ever_failed) {
        out += ", last failure " + human_duration(s.last_failure_ago_sec) +
               " ago (" + s.last_failure_reason + ")";
    }
    out += ", " + plural(s.restart_count, "restart", "restarts");
    out += ", " + plural(static_cast<long long>(s.failures_24h), "failure", "failures") +
           " (24h)";
    if (s.state == ServiceState::Quarantined || s.state == ServiceState::Failed) {
        out += " — recover with: restart " + s.cfg.name;
    }
    return out;
}

// RSS (bytes) and thread count from /proc/<pid>/status.
struct ProcMem {
    std::optional<std::uintmax_t> rss;
    std::optional<long> threads;
};

ProcMem read_proc_mem(pid_t pid) {
    ProcMem m;
    std::ifstream f("/proc/" + std::to_string(pid) + "/status");
    std::string line;
    while (std::getline(f, line)) {
        if (line.rfind("VmRSS:", 0) == 0) {
            // "VmRSS:	  186724 kB"
            m.rss = std::strtoull(line.c_str() + 6, nullptr, 10) * 1024ull;
        } else if (line.rfind("Threads:", 0) == 0) {
            m.threads = std::strtol(line.c_str() + 8, nullptr, 10);
        }
    }
    return m;
}

std::optional<long> count_fds(pid_t pid) {
    std::error_code ec;
    fs::directory_iterator it("/proc/" + std::to_string(pid) + "/fd", ec);
    if (ec) return std::nullopt;
    long n = 0;
    // increment(ec), not range-for: the range form throws on a read error.
    for (; it != fs::directory_iterator(); it.increment(ec)) {
        if (ec) return std::nullopt;
        ++n;
    }
    if (ec) return std::nullopt;
    return n;
}

// utime + stime in clock ticks, from /proc/<pid>/stat. The command name in
// field 2 is in parentheses and may itself hold spaces or ')', so fields are
// counted from the last ')'.
std::optional<unsigned long long> read_cpu_ticks(pid_t pid) {
    std::ifstream f("/proc/" + std::to_string(pid) + "/stat");
    std::string all((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    const std::size_t paren = all.rfind(')');
    if (paren == std::string::npos) return std::nullopt;
    std::istringstream rest(all.substr(paren + 1));
    std::string tok;
    // After ')' come field 3 (state) onward; utime is field 14, stime 15.
    unsigned long long utime = 0, stime = 0;
    for (int field = 3; field <= 15 && (rest >> tok); ++field) {
        if (field == 14) utime = std::strtoull(tok.c_str(), nullptr, 10);
        if (field == 15) {
            stime = std::strtoull(tok.c_str(), nullptr, 10);
            return utime + stime;
        }
    }
    return std::nullopt;
}

// Share of one core used over a short sample, the way top counts it (a busy
// 4-thread process can read 400%).
std::optional<double> sample_cpu_percent(pid_t pid) {
    const long hz = ::sysconf(_SC_CLK_TCK);
    if (hz <= 0) return std::nullopt;
    const auto t0 = std::chrono::steady_clock::now();
    const auto a = read_cpu_ticks(pid);
    if (!a) return std::nullopt;
    std::this_thread::sleep_for(kCpuSample);
    const auto b = read_cpu_ticks(pid);
    const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    if (!b || *b < *a || secs <= 0) return std::nullopt;
    return static_cast<double>(*b - *a) / static_cast<double>(hz) / secs * 100.0;
}

// ---- http -----------------------------------------------------------------

std::string http_line(const SwiftArgs& a) {
    if (a.port <= 0 || a.port > 65535) {
        return "not checked (port " + std::to_string(a.port) +
               " is picked at startup, castle can't know it)";
    }
    // A wildcard or named bind is still reachable on loopback.
    const std::string host =
        (a.bind == "0.0.0.0" || a.bind == "localhost") ? "127.0.0.1" : a.bind;
    const std::string where = host + ":" + std::to_string(a.port);

    std::string err;
    const auto t0 = std::chrono::steady_clock::now();
    const auto code = http_get_status(host, static_cast<uint16_t>(a.port), "/healthz",
                                      kHealthzTimeoutMs, err);
    const double ms =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();

    char buf[160];
    if (!code) {
        std::snprintf(buf, sizeof(buf), "DOWN (%s) on %s", err.c_str(), where.c_str());
    } else if (*code == 200) {
        std::snprintf(buf, sizeof(buf), "ok, /healthz 200 in %.1f ms on %s", ms, where.c_str());
    } else {
        std::snprintf(buf, sizeof(buf), "HTTP %d from /healthz on %s", *code, where.c_str());
    }
    return buf;
}

// ---- store ----------------------------------------------------------------

std::string created_line(const fs::path& store) {
    struct stat st{};
    if (::stat((store / "store.meta").c_str(), &st) != 0) {
        return std::string(kUnavailable) + " (store.meta: " + errno_reason(errno) + ")";
    }
    // store.meta is written once, when the store is made, and never again.
    return wall_timestamp(std::chrono::system_clock::from_time_t(st.st_mtime));
}

std::string segments_line(const fs::path& store) {
    std::error_code ec;
    fs::directory_iterator it(store, ec);
    if (ec) return std::string(kUnavailable) + " (" + ec.message() + ")";
    long files = 0;
    std::uintmax_t bytes = 0;
    for (; it != fs::directory_iterator(); it.increment(ec)) {
        if (ec) break;
        const std::string name = it->path().filename().string();
        if (name.rfind("segment-", 0) != 0 || name.size() < 12 ||
            name.compare(name.size() - 4, 4, ".dat") != 0) {
            continue;
        }
        std::error_code fec;
        const auto size = it->file_size(fec);
        if (fec) continue;
        ++files;
        bytes += size;
    }
    if (ec) return std::string(kUnavailable) + " (" + ec.message() + ")";
    return plural(files, "file", "files") + ", " + human_bytes(bytes);
}

std::string disk_line(const fs::path& store) {
    struct statvfs vs{};
    if (::statvfs(store.c_str(), &vs) != 0) {
        return std::string(kUnavailable) + " (" + errno_reason(errno) + ")";
    }
    const std::uintmax_t frsize = vs.f_frsize ? vs.f_frsize : vs.f_bsize;
    const std::uintmax_t total = static_cast<std::uintmax_t>(vs.f_blocks) * frsize;
    const std::uintmax_t avail = static_cast<std::uintmax_t>(vs.f_bavail) * frsize;
    const std::uintmax_t used =
        static_cast<std::uintmax_t>(vs.f_blocks - vs.f_bfree) * frsize;
    // Same percentage df prints: used / (used + what a normal user can still get).
    const std::uintmax_t denom = used + avail;
    const int pct = denom ? static_cast<int>((used * 100 + denom - 1) / denom) : 0;
    return human_bytes(avail) + " free of " + human_bytes(total) + " (" +
           std::to_string(pct) + "% used)";
}

// Counts keys by access level. Each line is
//   <id> <read|write|admin> <salt> <hash> [name]
// and only the first two fields are read: the salt and hash are never kept or
// printed.
std::string access_line(const fs::path& store) {
    const fs::path file = store / "access.keys";
    std::error_code ec;
    if (!fs::exists(file, ec)) {
        if (ec) return std::string(kUnavailable) + " (" + ec.message() + ")";
        return "none (every request gets 401)";
    }
    std::ifstream f(file);
    if (!f) return std::string(kUnavailable) + " (access.keys could not be opened)";

    long read = 0, write = 0, admin = 0, other = 0;
    std::string line;
    while (std::getline(f, line)) {
        const std::size_t b = line.find_first_not_of(" \t\r");
        if (b == std::string::npos || line[b] == '#') continue;
        std::istringstream fields(line);
        std::string id, access;
        if (!(fields >> id >> access)) continue;
        if (access == "read") ++read;
        else if (access == "write") ++write;
        else if (access == "admin") ++admin;
        else ++other;
    }
    const long total = read + write + admin + other;
    if (total == 0) return "none (every request gets 401)";

    std::string out = plural(total, "key", "keys") + " (";
    bool first = true;
    auto part = [&](long n, const char* label) {
        if (n == 0) return;
        if (!first) out += ", ";
        out += std::to_string(n) + " " + label;
        first = false;
    };
    part(admin, "admin");
    part(write, "write");
    part(read, "read");
    part(other, "unrecognised");
    out += ")";
    return out;
}

// ---- log ------------------------------------------------------------------

// swiftSearch's log lines are "[n] Y-M-D H:M:S LABEL: message" (date and time
// in UTC). Returns the label, or "" for a line that doesn't look like that.
std::string log_label(const std::string& line) {
    if (line.empty() || line.front() != '[') return "";
    const std::size_t bracket = line.find("] ");
    if (bracket == std::string::npos) return "";
    std::istringstream rest(line.substr(bracket + 2));
    std::string tok;
    // Date and time come first when enabled, so look a few tokens in.
    for (int i = 0; i < 3 && (rest >> tok); ++i) {
        if (tok == "WARN:" || tok == "LOW:" || tok == "MED:" || tok == "HIGH:") {
            return tok.substr(0, tok.size() - 1);
        }
    }
    return "";
}

void append_warnings(std::string& out, const fs::path& log) {
    out += "recent warnings and errors (" + log.string() + ", UTC)\n";

    std::error_code ec;
    if (!fs::exists(log, ec)) {
        out += ec ? "  " + std::string(kUnavailable) + " (" + ec.message() + ")\n"
                  : std::string("  no log file there yet\n");
        return;
    }
    std::ifstream f(log, std::ios::binary);
    if (!f) {
        out += "  " + std::string(kUnavailable) + " (could not open the log)\n";
        return;
    }
    f.seekg(0, std::ios::end);
    const std::streamoff size = f.tellg();
    const std::streamoff start =
        size > static_cast<std::streamoff>(kLogTailBytes)
            ? size - static_cast<std::streamoff>(kLogTailBytes)
            : 0;
    f.seekg(start);

    std::vector<std::string> hits;
    std::string line;
    bool partial = start > 0;  // the first line read may be cut in half
    while (std::getline(f, line)) {
        if (partial) {
            partial = false;
            continue;
        }
        // swiftSearch logs config problems (no keys, replay trouble) at WARN
        // and failed requests, corrupt records and failed compaction at HIGH.
        // MED is its startup settings and LOW is client mistakes: both noise.
        const std::string label = log_label(line);
        if (label == "WARN" || label == "HIGH") hits.push_back(line);
    }

    if (hits.empty()) {
        out += "  none\n";
        return;
    }
    const std::size_t from = hits.size() > kLogLinesShown ? hits.size() - kLogLinesShown : 0;
    if (from > 0) {
        out += "  (" + std::to_string(from) + " earlier in the last " +
               human_bytes(kLogTailBytes) + " not shown)\n";
    }
    for (std::size_t i = from; i < hits.size(); ++i) out += "  " + hits[i] + '\n';
}

// ---- one instance ---------------------------------------------------------

std::string render(const Supervisor::ServiceSnapshot& s) {
    const SwiftArgs a = parse_swift_args(s.cfg.args);
    const fs::path dir = run_dir(s.cfg);
    fs::path store = a.store;
    if (store.is_relative()) store = dir / store;

    std::string out = "swiftSearch '" + s.cfg.name + "'\n";
    row(out, "process", process_line(s));
    row(out, "http", http_line(a));

    if (s.state == ServiceState::Running && s.pid > 0) {
        const ProcMem m = read_proc_mem(s.pid);
        const auto fds = count_fds(s.pid);
        std::string mem = m.rss ? "rss " + human_bytes(*m.rss) : std::string("rss ") + kUnavailable;
        if (m.threads) mem += ", " + plural(*m.threads, "thread", "threads");
        mem += fds ? ", " + plural(*fds, "open fd", "open fds") : ", open fds " + std::string(kUnavailable);
        row(out, "memory", mem);

        const auto cpu = sample_cpu_percent(s.pid);
        char buf[48];
        if (cpu) std::snprintf(buf, sizeof(buf), "%.1f%% of one core", *cpu);
        row(out, "cpu", cpu ? std::string(buf) : std::string(kUnavailable));
    }

    out += "store " + store.string() + "\n";
    std::error_code ec;
    if (!fs::is_directory(store, ec)) {
        // Without this, a wrong --store would read as "no access keys".
        row(out, "folder", std::string(kUnavailable) + " (" +
                               (ec ? ec.message() : std::string("no such folder")) + ")");
    } else {
        row(out, "created", created_line(store));
        row(out, "segments", segments_line(store));
        row(out, "disk", disk_line(store));
        row(out, "access", access_line(store));
    }

    out += "config\n";
    row(out, "threads",
        (a.threads.empty() ? std::string("one per CPU") : "-j " + a.threads) +
            "   mem-cap: " + a.mem_cap + "   bind: " + a.bind + ":" +
            std::to_string(a.port));
    if (a.admin) {
        row(out, "WARNING",
            "started with --admin; keep it off behind castle (swiftSearch handoff)");
    }
    if (!is_loopback(a.bind)) {
        row(out, "WARNING",
            "bound to " + a.bind + ", not loopback; castle should be its only way in");
    }

    append_warnings(out, dir / kLogFile);
    return out;
}

}  // namespace

std::string swiftsearchStatusHelper(Supervisor* supervisor, const std::string& name) {
    if (!supervisor) return "no supervisor configured (start castle with --services)\n";

    const auto snaps = supervisor->snapshot();
    if (!snaps) return "supervisor not responding\n";

    std::vector<const Supervisor::ServiceSnapshot*> tagged;
    for (const auto& s : *snaps) {
        if (s.cfg.kind == "swiftsearch") tagged.push_back(&s);
    }

    if (!name.empty()) {
        for (const auto* s : tagged) {
            if (s->cfg.name == name) return render(*s);
        }
        for (const auto& s : *snaps) {
            if (s.cfg.name == name) {
                return "'" + name + "' is not tagged \"kind = swiftsearch\" in services.conf\n";
            }
        }
        std::string have;
        for (const auto* s : tagged) have += (have.empty() ? "" : ", ") + s->cfg.name;
        return "no swiftsearch service named '" + name + "'" +
               (have.empty() ? std::string() : " (have: " + have + ")") + "\n";
    }

    if (tagged.empty()) {
        return "no swiftsearch service (add \"kind = swiftsearch\" to its section in "
               "services.conf)\n";
    }
    std::string out;
    for (std::size_t i = 0; i < tagged.size(); ++i) {
        if (i > 0) out += '\n';
        out += render(*tagged[i]);
    }
    return out;
}

std::string swiftsearchStatus(Supervisor* supervisor, const std::string& name) {
    // Everything above reads files and sockets the vault owns. An exception
    // escaping the control thread would terminate castle (and every backend
    // with it), so a failed read must never be worse than an error line.
    try {
        return swiftsearchStatusHelper(supervisor, name);
    } catch (const std::exception& e) {
        return std::string("swiftsearch: could not read status: ") + e.what() + "\n";
    } catch (...) {
        return "swiftsearch: could not read status\n";
    }
}

}  // namespace castle
