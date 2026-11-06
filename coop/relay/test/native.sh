#!/bin/sh
# The PC socket (port/src/moonmod/net_transport_curl.cpp) and session against a relay: a local wrangler dev, or
# RELAY_URL. Needs clang++, libcurl's headers and nlohmann/json (NLOHMANN_INCLUDE, if it isn't on the include path).
set -e
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../../.." && pwd)
out=$(mktemp -d)
clang++ -std=c++23 -O1 -Wall -Wextra -DDC_HAVE_CURL -I"$root/port/include" ${NLOHMANN_INCLUDE:+-I"$NLOHMANN_INCLUDE"} \
    "$here/native_session_test.cpp" "$root/port/src/moonmod/net_session.cpp" "$root/port/src/moonmod/net_transport_curl.cpp" \
    -lcurl -lpthread -o "$out/native_test"
if [ -z "$RELAY_URL" ]; then
    # A process group of its own (Node's detached), so stopping the group stops wrangler's workerd too.
    dev=$(cd "$here/.." && node -e "const c = require('child_process').spawn('npx', ['wrangler', 'dev', '--port', '8787',
        '--ip', '127.0.0.1'], {detached: true, stdio: 'ignore'}); console.log(c.pid); c.unref();")
    for i in $(seq 1 60); do curl -sf http://127.0.0.1:8787/health > /dev/null && break; sleep 0.5; done
fi
status=0
"$out/native_test" || status=$?
[ -n "$dev" ] && kill -- "-$dev" 2> /dev/null || true
rm -rf "$out"
exit $status
