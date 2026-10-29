# antigravity-probe

Standalone monitor for Google Antigravity's working state, context window, session
token totals and quota. It is the pre-merge testbed for giving codex-pet-bridge an
`antigravity` source: every hook is translated into the exact event shape the bridge's
`normalizeEvent()` already accepts, so merging is a routing change, not a data-model one.

Zero npm dependencies. Node >= 20, `curl`, `/usr/bin/sqlite3`, and the `agy` CLI for quota.

## Quick start

```sh
cd bridge/antigravity-probe
sh install-hooks.sh            # writes ~/.gemini/config/hooks.json (merged, backed up)
node server.mjs                # http://127.0.0.1:8791
# in another shell: use Antigravity (CLI, app or IDE), then
curl -s localhost:8791/status | python3 -m json.tool
```

Environment: `PROBE_PORT` (8791), `PROBE_TOKEN` (optional; hooks then send `?t=`),
`PROBE_FORWARD_URL` + `PROBE_FORWARD_TOKEN` (forward every event to the bridge's
`POST /events`, off by default), `PROBE_QUOTA_INTERVAL_MS` (600000; 0 disables),
`PROBE_ACTIVE_WINDOW_MS` (180000), `PROBE_TOOL_WINDOW_MS` (900000),
`PROBE_COMPLETED_HOLD_MS` (120000).

## Endpoints

| Route | Purpose |
|---|---|
| `POST /hook/<Event>` | Receives the stdin payload of an Antigravity lifecycle hook. The event name travels in the URL because the payload does not carry it. Also accepted as `POST /events?event=<Event>`. |
| `GET /status` | Effective status (after ageing), last event, model, context window, session tokens, active conversation, quota. `?refreshQuota` forces a quota refresh. |
| `GET /events` | Last 50 translated events. |
| `GET /health` | Liveness. |

## Push path: hooks -> status

Antigravity's lifecycle hooks (`~/.gemini/antigravity-cli/builtin/skills/agy-customizations/docs/hooks.md`)
are synchronous shell commands that get the payload on stdin and must print JSON on stdout.
`install-hooks.sh` installs one `curl -s -m 1 ... || true; echo '<reply>'` command per event under
the `pet-monitor` key of `~/.gemini/config/hooks.json` (the machine-global customization root, so
CLI, app and IDE all fire them). The reply is echoed locally and curl's exit status is ignored, so
a stopped probe costs ~40 ms per event and can never block Antigravity (`--selftest` proves it).

| Hook | Status emitted | Bridge treats as |
|---|---|---|
| `SessionStart` | `started` (also carries `modelName`) | running |
| `PreInvocation` (before each model call) | `thinking` | running |
| `PostToolUse` (`toolCall.name`) | `tool-use`, or `searching` for search/grep/find/glob tools | running |
| `PostInvocation` | `working` | running |
| `Stop` (`terminationReason`) | `completed`, or `failed` on `ERROR` | done / error |
| `PreToolUse` (opt-in only, see below) | same as PostToolUse, but before the tool runs | running |

Ageing mirrors claude-session-status.js: thinking/working go idle after 3 min without events,
tool states after 15 min, completed/failed are held 2 min then idle.

Verified on this machine with a real `agy -p` run (print mode, global hooks.json):
`SessionStart -> PreInvocation -> PostToolUse read_file -> PostInvocation -> PreInvocation ->
PostInvocation -> Stop NO_TOOL_CALL`, all delivered within the same second they happened.

### PreToolUse is opt-in

A `PreToolUse` reply must carry a `decision`, and `"allow"` would silently auto-approve every
tool call, bypassing Antigravity's own permission prompts. The installer refuses `allow`.
`sh install-hooks.sh --pretool ask` installs it with `"ask"` (defer to the normal permission
flow); verify in an interactive session that it does not add prompts for tools that normally run
unattended before keeping it. Without it, a long-running tool shows as `thinking` until it ends.

## Pull path: SQLite + quota

`lib/antigravity-db.mjs` reads `~/.gemini/antigravity-cli|antigravity|antigravity-ide/conversations/*.db`
read-only through the sqlite3 CLI. `gen_metadata` holds one protobuf per model generation; the
decoded usage (input / cacheRead / thinking / output) matched the CLI's own `usage` JSON exactly
on a real run. The field numbers are internal and undocumented, so the decoder is tolerant and
skips rows it cannot parse. Closed, checkpointed databases have no `-wal/-shm` sidecars and
cannot be opened `-readonly` the usual way; they are read through a `file:...?immutable=1` URI
instead, which never writes to `~/.gemini`. Presence `.lock` files are reported but are not
trusted as "open now": the CLI leaves them behind after exit. "Today" bucketing is not
implemented (only per-conversation totals); `history.jsonl` carries millisecond timestamps if
that is needed later.

Quota comes from `agy -p /usage --output-format json` (0 tokens, ~10 s, spawns the CLI) every
10 min by default, summarised as e.g. `GEM WK 96% | 3P WK 100%` (a 5-hour bucket shows as `5H`).

## Tests

```sh
npm test   # unit (lib/translate), DB integration (read-only, real ~/.gemini), smoke (synthetic hooks on port 8799)
sh install-hooks.sh --selftest   # generated hook commands stay fail-open against a closed port
```

## Privacy and safety

Loopback only. The hook payload's `lastUserInput` and `toolCall.args` are dropped at translation;
only event name, tool name, conversation id, workspace path and model name are kept, logged or
forwarded. `hooks.json` is written with mode 0600 and always backed up before being touched.

## Uninstall

```sh
sh install-hooks.sh --uninstall   # removes only the pet-monitor key, backs up first
lsof -ti :8791 | xargs kill       # stop the probe
```

## Merge plan into codex-pet-bridge

1. Fastest: run the probe as a sidecar with `PROBE_FORWARD_URL=http://127.0.0.1:<bridge>/events`
   (+ `PROBE_FORWARD_TOKEN`). Events arrive pre-normalized with `source: "antigravity"`; the
   bridge's status vocabulary already covers every status emitted. Needed in the bridge:
   `sourceFamily()` must recognize `antigravity`, and `loadUsageSummary()` needs a branch that
   reports context / session tokens (from `lib/antigravity-db.mjs`) and the quota line.
2. Proper: move `lib/translate.mjs` and `lib/antigravity-db.mjs` into `codex-pet-bridge/src`,
   add a `POST /hook/antigravity/<Event>` route (or `?event=` on `/events`) that calls
   `translateHook`, point `install-hooks.sh` at the bridge port, and schedule the quota refresh
   from the bridge. The firmware needs no change: usage.today / context / quota are passthrough
   strings.
