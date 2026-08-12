# castle++

A from-scratch reverse proxy and process supervisor in C++20, built on a raw
`epoll` reactor. No frameworks, no dependencies beyond OpenSSL — a single
~5,000-line binary that fronts every service on a machine.

It is the *buffer* between the public internet and the apps behind it: it
terminates TLS, routes requests to backends, launches and babysits those
backends, and gives you a control socket to ask what's going on.

> **Linux only, by design.** The core is an epoll reactor using `signalfd`,
> `timerfd`, `eventfd` and `accept4`. There is no architecture-specific code —
> it builds on x86-64 and arm64, against both glibc and musl. It does not build
> on macOS or Windows.

---

## What it does on my system

A Lenovo ThinkCentre M900 sitting in my house runs every site and service I
host. It has no public IP. A free VPS forwards raw TCP `:443` over a WireGuard
tunnel to the M900, where castle++ is the only thing listening.

From there castle++ does all of it:

- **terminates TLS** with a Let's Encrypt cert — the VPS forwards bytes, it
  never sees plaintext, and never holds a key
- **routes** each request to the right backend by `Host` and path
- **launches and supervises** every backend process, restarting them when they
  die and quarantining them when they die too often
- **absorbs abuse** at the front door: connection caps and per-IP rate limits
  are applied at `accept()`, before a single byte of TLS or HTTP is parsed
- **answers questions** over a Unix socket — uptime, failure history, load,
  memory, live counters — without me needing to SSH around and read logs

One binary, one systemd/OpenRC unit, one config directory. The backends behind
it are ordinary programs that bind `127.0.0.1` and know nothing about the
internet.

---

## Features

### Networking core
- **One event loop per core.** Each worker thread opens its *own* listening
  socket on the same port with `SO_REUSEPORT`, so the kernel load-balances new
  connections across cores with no shared accept lock and no thundering herd.
- **Edge-triggered epoll**, fully non-blocking. Every handler drains to `EAGAIN`.
- **Deferred close.** A callback can request its own teardown mid-dispatch; the
  loop destroys handlers at the end of the batch, so nothing is freed underfoot.
- **Cross-thread task posting** via `eventfd` — other threads hand work to a
  loop and get an async ack back, instead of taking locks on loop state.
- **Idle timeouts** on a per-loop `timerfd` sweep, so slow-loris clients and
  hung backends get reaped (`--timeout`, default 30s).

### HTTP reverse proxy
- Requests parsed with vendored **picohttpparser**.
- Routed by `Host` + **longest path-prefix**, most specific match wins.
- **Streaming bodies in both directions with backpressure.** Request bodies are
  forwarded to the backend as they arrive — never buffered whole — and reads are
  paused when a peer's send buffer crosses 256 KiB, resuming below 64 KiB. An
  upload or download of any size runs in bounded memory, which is what lets
  castle++ front a file or photo vault.
- Synthesizes `404`, `413`, `431`, `502`, `501` itself when it must.
- **Request-smuggling defenses**: `Transfer-Encoding` + `Content-Length` is
  rejected before framing, duplicate `Content-Length` headers are rejected even
  when identical, and `Content-Length` is parsed strictly (no `strtol`
  permissiveness, no silent overflow).
- Hop-by-hop headers stripped before forwarding.

### TLS termination
- Non-blocking **OpenSSL driven inside the event loop** through a `Transport`
  abstraction — a TLS read that needs to *write* re-arms epoll in the right
  direction, and vice versa.
- TLS 1.2 / 1.3, server cipher preference, renegotiation off.
- **Live certificate reload on `SIGHUP`** — point certbot's deploy hook at it
  and renewals land with no restart and no dropped connections. A broken cert is
  rejected while the current one keeps serving.

### Process supervisor
- Launches backends from a manifest, `fork`/`execv` with optional working dir.
- Reaps children through **`signalfd`**, not a signal handler — no async-signal
  safety hazards, no self-pipe.
- **Exponential restart backoff** with a stability reset: a backend that stayed
  up a good while is treated as a fresh failure, not a loop.
- Periodic **TCP health checks** per service.
- **Failure circuit breaker** — more than **5 failures in 24 hours** quarantines
  a backend: it is logged at ERROR and *stops being restarted*, because a
  service failing that often is broken in a way restarting won't fix. The window
  slides on a monotonic clock, so an occasionally-flaky service keeps running and
  only a real crash loop trips it. Clear it with `restart <name>`.
- Graceful drain on shutdown: `SIGTERM`, grace period, then `SIGKILL`.

### Hardening
- `--max-conn` — global concurrent connection cap.
- `--rate` / `--rate-burst` — per-IP token bucket, memory-bounded with a shared
  overflow bucket so a diverse-IP flood can't grow the table without limit.
- Both are enforced **at accept time**, before any TLS or HTTP work is spent.
- `--bind` an address with `IP_FREEBIND`, so castle++ can bind a WireGuard IP
  that doesn't exist yet at boot and hide itself from the LAN entirely.
- Every fd is `CLOEXEC`; nothing leaks into a spawned backend.

### Control plane
A Unix-domain socket — deliberately not reachable from the network — served on
its own thread, off the data path.

| command | what it does |
|---|---|
| `help` | list commands |
| `ping` | liveness check → `pong` |
| `status` | aggregated counters across all loops (lock-free reads) |
| `health` | probe every event loop via the async task round-trip |
| `services` | supervised backends: state / pid / health / restarts |
| `backendstatus` | per-backend uptime, failure history, quarantine details |
| `systemstatus` | host vitals: threads, load, CPU temp, memory |
| `restart <name>` | restart a backend (and clear its quarantine) |
| `errors` | log lines since the last pull |
| `shutdown` | graceful stop |

Typo a command and it suggests the right one, via a QWERTY-aware autocorrect
built for the purpose.

```
$ printf 'backendstatus\n' | nc -U /run/castle/castle.sock
NAME                 STATE        UPTIME       FAILURES: 1h  24h  TOTAL   LAST FAILURE
web                  running      2h 13m       0             0    0       -
flaky                quarantined  -            6             6    6       2s ago (exit 3)

PERMANENTLY DOWN — not being restarted:
  'flaky' quarantined 2s ago (at 2026-08-05 16:47:32)
      tripped by 6 failures within 24h (limit 5)
      last failure: exit 3
      recover with: restart flaky
```

### Logging
Everything goes to stderr (journald / OpenRC log files pick it up). `--log-file`
additionally mirrors WARN and ERROR into a file that the `errors` command reads
back **incrementally** — each pull returns only what's new since the last one.

---

## Getting it on your system

### Requirements

A Linux box, a C++20 compiler, CMake ≥ 3.16, and OpenSSL headers.

```sh
# Debian / Ubuntu
sudo apt install -y build-essential cmake libssl-dev

# Alpine
sudo apk add build-base cmake openssl-dev linux-headers
```

### Build

```sh
git clone https://github.com/<you>/castleplus.git
cd castleplus
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

That produces a single binary at `build/castle`.

### Try it in 30 seconds

With no `--routes`, castle++ is a plain echo server — useful for confirming the
reactor works before you configure anything:

```sh
./build/castle --port 8080
printf 'hello castle\n' | nc localhost 8080     # -> hello castle
```

Then point it at the bundled example config to run as a real proxy:

```sh
./build/castle --port 8080 \
  --routes   config/routes.conf \
  --services config/services.conf

curl -H 'Host: web.local' http://localhost:8080/
```

`config/services.conf` ships an example backend using `busybox httpd`. On Alpine
that applet isn't available — use `darkhttpd` (`apk add darkhttpd`) or your own
program instead.

### Configure

**`routes.conf`** — where requests go:

```ini
[web]
host    = example.com
path    = /
backend = 127.0.0.1:9001
```

**`services.conf`** — what castle++ launches and supervises:

```ini
[web]
exec        = /usr/bin/darkhttpd
args        = /srv --addr 127.0.0.1 --port 9001
autorestart = true
health_tcp  = 127.0.0.1:9001
```

### Serve HTTPS

```sh
# self-signed, for local testing
openssl req -x509 -newkey rsa:2048 -nodes -days 365 \
  -keyout key.pem -out cert.pem -subj '/CN=localhost'

./build/castle --port 8443 --routes config/routes.conf \
  --tls-cert cert.pem --tls-key key.pem

curl -k https://localhost:8443/
```

In production the cert comes from Let's Encrypt; `systemctl reload castle` (or
`rc-service castle reload`) sends `SIGHUP` and swaps it in live.

### Run it as a service

An OpenRC service script is included at [`openrc/castle.initd`](openrc/castle.initd)
— it runs castle++ under `supervise-daemon` as a dedicated non-root user with
restart backoff, an fd limit, and a `reload` that triggers the live cert swap.

```sh
sudo adduser -S -D -H -s /sbin/nologin castle
sudo mkdir -p /opt/castle && sudo cp build/castle /opt/castle/
sudo cp -r config /opt/castle/ && sudo chown -R castle:castle /opt/castle
sudo cp openrc/castle.initd /etc/init.d/castle && sudo chmod +x /etc/init.d/castle
sudo rc-update add castle default && sudo rc-service castle start
```

On a systemd distro, write the equivalent unit — run as a non-root user on a
high port, `ExecReload=/bin/kill -HUP $MAINPID`, and `Restart=on-failure`. Since
the supervisor's backends are children in the same cgroup, systemd's default
`KillMode=control-group` cleans them all up with it.

### Options

```
--port N         TCP port to listen on (default 8080)
--bind ADDR      IPv4 address to listen on (default 0.0.0.0)
--threads N      worker loops; 0/omitted = one per core
--routes PATH    routes file -> HTTP proxy mode (omit = echo mode)
--services PATH  services manifest to supervise (omit = off)
--control PATH   unix control socket (default /tmp/castle.sock; "" disables)
--timeout N      idle connection timeout, seconds (default 30; 0 disables)
--tls-cert PATH  PEM certificate (with --tls-key => HTTPS)
--tls-key PATH   PEM private key
--max-body N     max request body bytes (default 1 GiB; 0 = unlimited)
--log-file PATH  mirror WARN/ERROR to this file
--max-conn N     max concurrent connections (0 = unlimited)
--rate N         per-IP connections/sec, token bucket (0 = off)
--rate-burst N   token bucket size (default = --rate)
```

---

## Layout

```
src/
  main.cpp          per-core loops, signal-driven shutdown
  net/              socket RAII, epoll loop, listener, transport, echo
  tls/              SSL_CTX + non-blocking OpenSSL transport
  http/             router, client connection, upstream connection
  supervisor/       fork/exec, signalfd reaping, backoff, health, breaker
  control/          admin thread on a Unix socket
  config/           services manifest parser
  util/             logging, hardware vitals, autocorrect
third_party/
  picohttpparser/   vendored HTTP request parser (MIT)
config/             example routes + services manifests
openrc/             OpenRC service script
```

## Scope and limits

Honest about what it isn't yet:

- **One request per client connection** — no HTTP keep-alive or backend
  connection pooling.
- **Chunked request bodies return 501.** `Content-Length` and bodiless requests
  are handled.
- **IPv4 only**, and backends are addressed by IP, not hostname.
- **One certificate** — no SNI-based multi-cert selection.
- Behind a tunnel, every request appears to come from the tunnel's IP, so the
  rate limiter acts as an aggregate cap until PROXY-protocol client-IP
  preservation lands.

## License

The vendored `third_party/picohttpparser` is MIT (see its header). Choose a
license for the rest before publishing — without a `LICENSE` file, others have
no rights to use it.
