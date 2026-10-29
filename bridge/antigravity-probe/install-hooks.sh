#!/bin/sh
# Install, print or remove the Antigravity lifecycle hooks that feed antigravity-probe.
#
# Target: ~/.gemini/config/hooks.json - Antigravity's machine-global customization
# root, so the hooks fire for the CLI, the app and the IDE alike. An existing file is
# merged (only the "pet-monitor" key is touched) and backed up first.
#
# Every hook is fail-open: curl is capped at 1 s, its exit status is ignored, and the
# JSON the agent loop needs ("{}" / "stop") is echoed locally, so a stopped probe can
# never block or slow Antigravity. --selftest proves that against a closed port.
#
# PreToolUse is NOT installed by default: its reply must carry a "decision", and
# "allow" would silently auto-approve every tool call. --pretool ask opts in with
# "ask" (defers to Antigravity's own permission flow) - verify interactively that it
# does not add prompts for tools that normally run unattended before keeping it.
#
# usage: install-hooks.sh [--port N] [--token T] [--target PATH] [--pretool ask] [--print|--uninstall|--selftest]
set -eu

PORT=17366
TOKEN=""
TARGET="$HOME/.gemini/config/hooks.json"
PRETOOL=""
MODE=install

usage() { sed -n '2,17p' "$0" | sed 's/^# \{0,1\}//'; }

while [ $# -gt 0 ]; do
  case "$1" in
    --port) PORT="$2"; shift 2 ;;
    --token) TOKEN="$2"; shift 2 ;;
    --target) TARGET="$2"; shift 2 ;;
    --pretool)
      [ "$2" = ask ] || { echo "--pretool only accepts 'ask' ('allow' would auto-approve every tool call)" >&2; exit 2; }
      PRETOOL=ask; shift 2 ;;
    --print) MODE=print; shift ;;
    --uninstall) MODE=uninstall; shift ;;
    --selftest) MODE=selftest; shift ;;
    -h|--help) usage; exit 0 ;;
    *) echo "unknown argument: $1" >&2; usage >&2; exit 2 ;;
  esac
done

command -v node >/dev/null 2>&1 || { echo "node is required" >&2; exit 1; }
command -v curl >/dev/null 2>&1 || { echo "curl is required" >&2; exit 1; }

# Hook definitions, built with node so the nested quoting stays sane. PreToolUse and
# PostToolUse are "grouped" (matcher + hooks), the rest are flat handler lists - see
# the CLI's builtin docs/hooks.md.
HOOK_JSON="$(node - "$PORT" "$TOKEN" "$PRETOOL" <<'EOF'
const [port, token, pretool] = process.argv.slice(2);
const query = token ? `?t=${encodeURIComponent(token)}` : "";
const authHeader = token ? "" : ' -H "Authorization: Bearer $(cat \\"$HOME/.codex-pet-bridge/token\\" 2>/dev/null)"';
const command = (event, reply) =>
  `[ -n "$PET_ANTIGRAVITY_INTERNAL" ] || curl -s -m 1${authHeader} --data-binary @- -o /dev/null "http://127.0.0.1:${port}/hook/${event}${query}" || true; echo '${reply}'`;
const flat = (event, reply) => [{ type: "command", command: command(event, reply), timeout: 3 }];
const grouped = (event, reply) => [{ matcher: "*", hooks: flat(event, reply) }];
const hooks = {
  SessionStart: flat("SessionStart", "{}"),
  PreInvocation: flat("PreInvocation", "{}"),
  PostInvocation: flat("PostInvocation", "{}"),
  PostToolUse: grouped("PostToolUse", "{}"),
  Stop: flat("Stop", '{"decision":"stop"}')
};
if (pretool === "ask") hooks.PreToolUse = grouped("PreToolUse", '{"decision":"ask"}');
process.stdout.write(JSON.stringify(hooks));
EOF
)"

if [ "$MODE" = selftest ]; then
  # Run the generated commands against port 9 (discard; closed on macOS, so the
  # connection is refused instantly): each must still print its reply, fast.
  run_down() { # run_down <json path expr> <expected reply> <label>
    CMD="$(node -e 'console.log(eval("JSON.parse(process.argv[1])." + process.argv[2]))' "$HOOK_JSON" "$1" | sed "s#127.0.0.1:${PORT}/#127.0.0.1:9/#")"
    START="$(node -e 'console.log(Date.now())')"
    OUT="$(printf '{"toolCall":{"name":"run_command"},"conversationId":"selftest"}' | sh -c "$CMD")"
    END="$(node -e 'console.log(Date.now())')"
    echo "probe-down $3 hook -> $OUT in $((END - START)) ms"
    [ "$OUT" = "$2" ] || { echo "FAIL: $3 must answer $2 when the probe is down" >&2; exit 1; }
    [ $((END - START)) -lt 1500 ] || { echo "FAIL: $3 hook took too long" >&2; exit 1; }
  }
  run_down 'PostToolUse[0].hooks[0].command' '{}' PostToolUse
  run_down 'Stop[0].command' '{"decision":"stop"}' Stop
  [ -z "$PRETOOL" ] || run_down 'PreToolUse[0].hooks[0].command' '{"decision":"ask"}' PreToolUse
  echo "selftest ok"
  exit 0
fi

node - "$TARGET" "$MODE" "$HOOK_JSON" <<'EOF'
const fs = require("node:fs");
const path = require("node:path");
const [target, mode, hookJson] = process.argv.slice(2);
let existing = {};
if (fs.existsSync(target)) {
  const text = fs.readFileSync(target, "utf8");
  if (text.trim()) {
    try {
      existing = JSON.parse(text);
    } catch (error) {
      console.error(`refusing to touch ${target}: not valid JSON (${error.message})`);
      process.exit(1);
    }
  }
  if (!existing || typeof existing !== "object" || Array.isArray(existing)) {
    console.error(`refusing to touch ${target}: top level is not an object`);
    process.exit(1);
  }
}
const next = { ...existing };
if (mode === "uninstall") {
  if (!("pet-monitor" in next)) {
    console.log(`pet-monitor is not in ${target}; nothing to do`);
    process.exit(0);
  }
  delete next["pet-monitor"];
} else {
  next["pet-monitor"] = JSON.parse(hookJson);
}
const out = `${JSON.stringify(next, null, 2)}\n`;
if (mode === "print") {
  process.stdout.write(out);
  process.exit(0);
}
fs.mkdirSync(path.dirname(target), { recursive: true });
if (fs.existsSync(target)) {
  const backup = `${target}.bak-${new Date().toISOString().replace(/[:.]/g, "-")}`;
  fs.copyFileSync(target, backup);
  console.log(`backup: ${backup}`);
}
fs.writeFileSync(target, out, { mode: 0o600 });
console.log(`${mode === "uninstall" ? "removed pet-monitor from" : "wrote pet-monitor hooks to"} ${target}`);
EOF
