# Claude — Date 06/19/2026
# Lets you build/run the Linux-only reactor from a Mac without a Linux box handy.
#   docker build -t castle .
#   docker run --rm -p 8080:8080 castle
# Use --platform linux/amd64 to match the x86-64 target server if you like.
FROM debian:bookworm-slim AS build

RUN apt-get update && apt-get install -y --no-install-recommends \
        g++ cmake make libssl-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY . .
RUN cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j

FROM debian:bookworm-slim
# Claude — Date 06/19/2026
# Test-only conveniences (drop for a lean production image):
#   netcat-openbsd — poke the Unix control socket (`nc -U /tmp/castle.sock`)
#   busybox        — provides `busybox httpd`, a tiny real HTTP backend to proxy
RUN apt-get update && apt-get install -y --no-install-recommends \
        netcat-openbsd busybox libssl3 openssl \
    && rm -rf /var/lib/apt/lists/*
# A page for the demo backend (busybox httpd -h /srv) to serve.
RUN mkdir -p /srv && printf '<h1>hello from a castle++ backend</h1>\n' > /srv/index.html
COPY --from=build /src/build/castle /usr/local/bin/castle
EXPOSE 8080
ENTRYPOINT ["/usr/local/bin/castle"]
CMD ["--port", "8080"]
