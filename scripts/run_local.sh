#!/usr/bin/env bash
# Runs the whole stack as separate processes and drives it with the mock exchange:
#   rtbkit_router (banker, router, OpenRTB connector :12350, post auction, ad server :12360,
#                  admin :12370), two fixed_price_agent processes (zmq to :12340),
#   then mock_exchange. Prints the stats and stops everything. Exit code 0 only if there
#   were bids, wins and banker spend.
#
#   scripts/run_local.sh [count] [qps]        defaults: 2000 500
set -u

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"
COUNT="${1:-2000}"
QPS="${2:-500}"
BIN="$ROOT/build/bin"
LOGS="$(mktemp -d "${TMPDIR:-/tmp}/rtbkit-local.XXXXXX")"
PIDS=()
MOCK_PID=

cleanup() {
    trap - EXIT INT TERM
    [[ -n "$MOCK_PID" ]] && PIDS+=("$MOCK_PID")
    for pid in ${PIDS[@]+"${PIDS[@]}"}; do kill -TERM "$pid" 2>/dev/null; done
    # Give them up to 5 s to exit, then kill -9 whatever is left.
    for _ in $(seq 1 50); do
        local alive=0
        for pid in ${PIDS[@]+"${PIDS[@]}"}; do kill -0 "$pid" 2>/dev/null && alive=1; done
        [[ $alive -eq 0 ]] && break
        sleep 0.1
    done
    for pid in ${PIDS[@]+"${PIDS[@]}"}; do kill -KILL "$pid" 2>/dev/null; done
    for pid in ${PIDS[@]+"${PIDS[@]}"}; do wait "$pid" 2>/dev/null; done
    echo "logs: $LOGS"
}
trap cleanup EXIT
trap 'cleanup; exit 130' INT
trap 'cleanup; exit 143' TERM

fail() { echo "FAILED: $*" >&2; for f in "$LOGS"/*.log; do echo "--- $f"; tail -n 15 "$f"; done >&2; exit 1; }

for tool in lsof curl python3; do
    command -v "$tool" >/dev/null 2>&1 || { echo "$tool is required but not installed" >&2; exit 1; }
done

if [[ ! -x "$BIN/rtbkit_router" || ! -x "$BIN/fixed_price_agent" || ! -x "$BIN/mock_exchange" ]]; then
    echo "building..."
    cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release >/dev/null || exit 1
    cmake --build build || exit 1
fi

for port in 12340 12350 12360 12370; do
    if lsof -nP -iTCP:"$port" -sTCP:LISTEN >/dev/null 2>&1; then
        echo "port $port is already in use" >&2
        exit 1
    fi
done

checkAlive() {   # fails fast if the router or an agent has died
    for pid in ${PIDS[@]+"${PIDS[@]}"}; do
        kill -0 "$pid" 2>/dev/null || fail "process $pid died"
    done
}

waitFor() {   # waitFor <description> <command...>   (up to 10 s)
    local what="$1"; shift
    for _ in $(seq 1 100); do
        "$@" >/dev/null 2>&1 && return 0
        checkAlive
        sleep 0.1
    done
    fail "$what did not become ready"
}

"$BIN/rtbkit_router" --config config/rtbkit.json >"$LOGS/router.log" 2>&1 &
PIDS+=($!)
waitFor "connector /ping on 12350" curl -sf http://127.0.0.1:12350/ping
waitFor "ad server /ping on 12360" curl -sf http://127.0.0.1:12360/ping
waitFor "admin on 12370" curl -sf http://127.0.0.1:12370/ping

"$BIN/fixed_price_agent" -c config/agents/agent_a.json -r tcp://127.0.0.1:12340 -p 2.0 -n agent_a >"$LOGS/agent_a.log" 2>&1 &
PIDS+=($!)
"$BIN/fixed_price_agent" -c config/agents/agent_b.json -r tcp://127.0.0.1:12340 -p 1.2 -n agent_b >"$LOGS/agent_b.log" 2>&1 &
PIDS+=($!)

# Both agents got GOTCONFIG when the router counts 2 agents.
agentsReady() { curl -sf http://127.0.0.1:12370/stats | grep -Eq '"agents": 2'; }
waitFor "both agents (GOTCONFIG)" agentsReady
echo "router and 2 agents are ready"

echo "running mock_exchange: $COUNT requests at $QPS qps"
# In the background + wait, so that INT / TERM are handled at once (bash runs a trap
# only after a foreground command ends).
"$BIN/mock_exchange" --url http://127.0.0.1:12350/auctions --count "$COUNT" --qps "$QPS" \
    --win-rate 0.3 --adserver http://127.0.0.1:12360 >"$LOGS/mock_exchange.log" 2>&1 &
MOCK_PID=$!
wait "$MOCK_PID"; MOCK_STATUS=$?
MOCK_PID=
cat "$LOGS/mock_exchange.log"
[[ $MOCK_STATUS -eq 0 ]] || fail "mock_exchange failed"
checkAlive

# Unclaimed bids become losses after router.lossTimeoutMs (3 s in config/rtbkit.json).
echo "waiting for loss timeouts..."
sleep 4.5 & wait $!
checkAlive

echo; echo "== adserver /stats =="
curl -sf http://127.0.0.1:12360/stats; echo
echo "== router admin /stats =="
STATS="$(curl -sf http://127.0.0.1:12370/stats)" || fail "no admin stats"
echo "$STATS"
echo "== agents =="
tail -n 1 "$LOGS/agent_a.log"
tail -n 1 "$LOGS/agent_b.log"

python3 - "$STATS" <<'PY'
import json, sys
s = json.loads(sys.argv[1])
def micros(v): return 0 if v == 0 else v[0]
spent = {":".join(a["account"]): micros(a["spent"]) for a in s["banker"]["accounts"]}
bids, wins = s["connector"]["bids"], s["router"]["wins"]
print(f"summary: bid responses={bids} wins={wins}")
for account, value in spent.items():
    print(f"  banker spent {account}: {value / 1e6:.6f} USD")
sys.exit(0 if bids > 0 and wins > 0 and any(v > 0 for v in spent.values()) else 1)
PY
RESULT=$?
if [[ $RESULT -ne 0 ]]; then fail "no bids, wins or banker spend"; fi
echo "run_local OK"
