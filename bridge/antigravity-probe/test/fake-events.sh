#!/bin/sh
# Smoke test: start the probe on a scratch port, drive every hook event type through
# POST /hook/<Event> with synthetic payloads and check the derived status after each,
# then make sure prompt text and tool arguments never leak into /events or the log,
# and that the generated hook command stays fail-open when the probe is down.
set -eu
PORT="${PROBE_TEST_PORT:-8799}"
DIR="$(cd "$(dirname "$0")/.." && pwd)"
LOG="${TMPDIR:-/tmp}/antigravity-probe-smoke.log"
BASE="http://127.0.0.1:$PORT"

PROBE_PORT="$PORT" PROBE_QUOTA_INTERVAL_MS=0 node "$DIR/server.mjs" >"$LOG" 2>&1 &
PID=$!
trap 'kill "$PID" 2>/dev/null || true' EXIT INT TERM
i=0
until curl -sf "$BASE/health" >/dev/null 2>&1; do
  i=$((i + 1))
  [ "$i" -lt 50 ] || { echo "FAIL: probe did not start; log:"; cat "$LOG"; exit 1; }
  sleep 0.1
done

COMMON='"conversationId":"smoke-conv-0001","workspacePaths":["/tmp/probe-ws"],"modelName":"auto","lastUserInput":"SECRET-PROMPT-MUST-NOT-LEAK"'
FAILED=0

current() {
  curl -s "$BASE/status" | node -e 'let d="";process.stdin.on("data",c=>d+=c).on("end",()=>{const j=JSON.parse(d);console.log(j.status+" "+(j.tool||"-"))})'
}
post() { curl -s -o /dev/null -w '%{http_code}' --data-binary "$2" "$BASE/hook/$1"; }
check() {
  got="$(current)"
  if [ "$got" = "$1" ]; then echo "ok   $2 -> $got"; else echo "FAIL $2 -> expected '$1', got '$got'"; FAILED=1; fi
}
check_code() {
  if [ "$2" = "$1" ]; then echo "ok   $3 -> HTTP $2"; else echo "FAIL $3 -> expected HTTP $1, got $2"; FAILED=1; fi
}

check "idle -" "initial state"
check_code 202 "$(post SessionStart "{$COMMON}")" "POST SessionStart"
check "started -" "after SessionStart"
post PreInvocation "{\"invocationNum\":1,\"initialNumSteps\":1,$COMMON}" >/dev/null
check "thinking -" "after PreInvocation"
post PreToolUse "{\"toolCall\":{\"name\":\"run_command\",\"args\":{\"CommandLine\":\"ls -la\"}},\"stepIdx\":3,$COMMON}" >/dev/null
check "tool-use run_command" "after PreToolUse run_command"
post PostToolUse "{\"toolCall\":{\"name\":\"run_command\",\"args\":{}},\"stepIdx\":3,$COMMON}" >/dev/null
check "tool-use run_command" "after PostToolUse (tool shown until next invocation)"
post PreInvocation "{\"invocationNum\":2,\"initialNumSteps\":5,$COMMON}" >/dev/null
check "thinking -" "after second PreInvocation"
post PreToolUse "{\"toolCall\":{\"name\":\"search_web\",\"args\":{}},\"stepIdx\":6,$COMMON}" >/dev/null
check "searching search_web" "after PreToolUse search_web"
post PostInvocation "{\"invocationNum\":2,\"initialNumSteps\":5,$COMMON}" >/dev/null
check "working -" "after PostInvocation"
post Stop "{\"executionNum\":1,\"terminationReason\":\"NO_TOOL_CALL\",\"fullyIdle\":true,$COMMON}" >/dev/null
check "completed -" "after Stop NO_TOOL_CALL"
post Stop "{\"executionNum\":2,\"terminationReason\":\"ERROR\",\"error\":\"boom\",$COMMON}" >/dev/null
check "failed -" "after Stop ERROR"
check_code 400 "$(post Bogus '{}')" "unknown event rejected"
check_code 202 "$(post PreInvocation 'this is not json')" "malformed body tolerated"
check "thinking -" "malformed body still yields a status"

EVENTS="$(curl -s "$BASE/events")"
if printf '%s' "$EVENTS" | grep -q "SECRET-PROMPT"; then echo "FAIL prompt text leaked into /events"; FAILED=1; else echo "ok   /events holds no prompt text"; fi
if printf '%s' "$EVENTS" | grep -q "ls -la"; then echo "FAIL tool arguments leaked into /events"; FAILED=1; else echo "ok   /events holds no tool arguments"; fi
if grep -q "SECRET-PROMPT" "$LOG"; then echo "FAIL prompt text leaked into the log"; FAILED=1; else echo "ok   log holds no prompt text"; fi

if sh "$DIR/install-hooks.sh" --selftest --port "$PORT" | tail -1 | grep -q "selftest ok"; then
  echo "ok   hook command stays fail-open with the probe down"
else
  echo "FAIL hook fail-open selftest"; FAILED=1
fi

if [ "$FAILED" -eq 0 ]; then echo "ALL OK"; else echo "SOME CHECKS FAILED (log: $LOG)"; exit 1; fi
