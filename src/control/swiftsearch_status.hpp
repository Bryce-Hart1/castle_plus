// Claude — Date 11/05/2026
// `swiftsearch [name]` on the control socket: what a supervised swiftSearch (the
// vault) is doing, read entirely from the outside. Nothing here needs
// swiftSearch's --admin route or one of its access keys:
//
//   process   the supervisor's own record (state, pid, uptime, failures)
//   http      GET /healthz, which swiftSearch serves without a key
//   memory    /proc/<pid>/status and /proc/<pid>/fd
//   cpu       /proc/<pid>/stat, sampled twice
//   store     the store folder: store.meta's mtime, segment-*.dat, statvfs,
//             and access.keys (only the id and access fields are looked at)
//   config    the service's own args in services.conf
//   warnings  WARN and HIGH lines from the tail of swiftSearchLogs in the
//             service's workdir
//
// Services are found by `kind = swiftsearch` in services.conf. Every reader is
// optional: a missing file or a denied read prints "unavailable" for that line
// and the command carries on. Runs on the control thread, which may block; the
// worst case is about 0.7 s per instance (CPU sample + /healthz timeout).
#pragma once

#include <string>

namespace castle {

class Supervisor;

// Every `kind = swiftsearch` service when `name` is empty, or just that one.
// May throw (std::filesystem, allocation); call swiftsearchStatus() instead.
std::string swiftsearchStatusHelper(Supervisor* supervisor, const std::string& name);

// The same, but never throws: any error becomes a one-line reply. This is what
// the control server calls.
std::string swiftsearchStatus(Supervisor* supervisor, const std::string& name);

}  // namespace castle
