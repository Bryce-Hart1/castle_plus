#!/usr/bin/env bash
# Claude — Date 10/31/2026
# Acceptance checks for the swiftSearch vault behind castle+. Runs entirely
# through castle, so it exercises auth, the allowlist, the body cap, Expect,
# and pass-through of Range/ETag — the same list whether castle is in a test
# container or behind the Cloudflare tunnel.
#
#   H=https://vault.example.com RW=<rw token> RO=<ro token> scripts/vault_acceptance.sh
#
# Optional: CURL_EXTRA="-k" for a self-signed cert. Leaves no keys behind.
set -u

: "${H:?set H to the vault base URL}"
: "${RW:?set RW to a read-write token}"
: "${RO:?set RO to a read-only token}"
read -r -a EXTRA <<< "${CURL_EXTRA:-}"

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
pass=0
fail=0

check() {  # name expected actual
    if [ "$2" = "$3" ]; then
        printf '  ok    %s\n' "$1"
        pass=$((pass + 1))
    else
        printf '  FAIL  %s (expected %s, got %s)\n' "$1" "$2" "$3"
        fail=$((fail + 1))
    fi
}

code() {  # curl args... -> status code only
    curl -s "${EXTRA[@]}" -o /dev/null -w '%{http_code}' "$@"
}

AW="Authorization: Bearer $RW"
AR="Authorization: Bearer $RO"
key="acceptance/$$/photo.jpg"

head -c 5000000 /dev/urandom > "$work/photo.jpg"
head -c 600000 /dev/urandom > "$work/600k.bin"
head -c 70000000 /dev/zero > "$work/big.bin"

echo "auth"
check "no credential -> 401" 401 "$(code "$H/v1/blobs/x")"
check "wrong token -> 401" 401 "$(code -H 'Authorization: Bearer nope' "$H/v1/blobs/x")"
challenge=$(curl -s "${EXTRA[@]}" -D - -o /dev/null "$H/v1/blobs/x" |
    grep -ci '^www-authenticate: basic')
check "401 carries a Basic challenge" 1 "$challenge"
check "401 to a client sending a body without Expect" 401 \
    "$(code -X PUT -H 'Expect:' --data-binary @"$work/600k.bin" "$H/v1/blobs/x")"

echo "read-write"
check "PUT 5 MB with Expect: 100-continue -> 201" 201 \
    "$(code -H "$AW" -T "$work/photo.jpg" "$H/v1/blobs/$key")"
curl -s "${EXTRA[@]}" -H "$AW" -o "$work/out.jpg" "$H/v1/blobs/$key"
check "GET round-trips byte for byte" same \
    "$(cmp -s "$work/photo.jpg" "$work/out.jpg" && echo same || echo differ)"
check "Basic auth (token as password) -> 200" 200 \
    "$(code -u "anyone:$RW" "$H/v1/blobs/$key")"

hdrs=$(curl -s "${EXTRA[@]}" -H "$AW" -H 'Range: bytes=0-99' -D - -o "$work/range" \
    "$H/v1/blobs/$key")
check "Range -> 206" 206 "$(printf '%s' "$hdrs" | head -1 | awk '{print $2}')"
check "Range -> Content-Range header" 1 "$(printf '%s' "$hdrs" | grep -ci '^content-range: bytes 0-99/')"
check "Range -> 100 bytes" 100 "$(wc -c < "$work/range" | tr -d ' ')"

etag=$(curl -s "${EXTRA[@]}" -H "$AW" -I "$H/v1/blobs/$key" |
    awk 'tolower($1)=="etag:" {print $2}' | tr -d '\r')
check "If-None-Match -> 304" 304 "$(code -H "$AW" -H "If-None-Match: $etag" "$H/v1/blobs/$key")"

echo "read-only"
check "ro GET -> 200" 200 "$(code -H "$AR" "$H/v1/blobs/$key")"
check "ro PUT -> 403" 403 "$(code -H "$AR" -T "$work/600k.bin" "$H/v1/blobs/acceptance/$$/ro")"
check "ro DELETE -> 403" 403 "$(code -H "$AR" -X DELETE "$H/v1/blobs/$key")"

echo "edge policy"
check "/v1/admin/limits with rw token -> 404" 404 "$(code -H "$AW" "$H/v1/admin/limits")"
check "non-allowlisted /v1/boxes/ -> 404" 404 "$(code -H "$AW" "$H/v1/boxes/x")"
check "70 MB upload -> 413 at the edge" 413 \
    "$(code -H "$AW" -T "$work/big.bin" "$H/v1/blobs/acceptance/$$/big")"

echo "cleanup"
check "DELETE -> 204" 204 "$(code -H "$AW" -X DELETE "$H/v1/blobs/$key")"
check "deleted key -> 404" 404 "$(code -H "$AW" "$H/v1/blobs/$key")"

echo
echo "$pass passed, $fail failed"
[ "$fail" -eq 0 ]
