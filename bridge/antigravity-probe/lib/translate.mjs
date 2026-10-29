// Pure translation logic shared by server.mjs and the tests. No I/O here.
//
// Antigravity hook payloads (camelCase protojson, see the CLI's builtin
// docs/hooks.md) are turned into the event shape codex-pet-bridge's
// normalizeEvent() consumes directly: source/type/status/tool/sessionId/
// workspace/message. Prompt text (lastUserInput) and tool arguments are
// deliberately dropped.

export const HOOK_EVENTS = new Set(["SessionStart", "PreInvocation", "PreToolUse", "PostToolUse", "PostInvocation", "Stop"]);
export const TOOL_STATUSES = new Set(["tool-use", "searching"]);
export const TERMINAL_STATUSES = new Set(["completed", "failed"]);

export function str(value) {
  if (typeof value === "string") return value.trim();
  if (typeof value === "number" || typeof value === "boolean") return String(value);
  return "";
}

export function num(value) {
  const parsed = Number(value);
  return Number.isFinite(parsed) ? parsed : null;
}

// Same heuristics as toolStatusForName() in codex-pet-bridge/src/bridge-server.js.
export function toolStatusForName(value) {
  const tool = str(value).toLowerCase();
  if (!tool) return "tool-use";
  if (tool.includes("search") || tool.includes("grep") || tool.includes("find") || tool.includes("glob") || tool === "rg") {
    return "searching";
  }
  return "tool-use";
}

export function translateHook(event, raw = {}, now = new Date()) {
  const payload = raw && typeof raw === "object" ? raw : {};
  const tool = str(payload.toolCall?.name);
  const reason = str(payload.terminationReason);
  let status;
  switch (event) {
    case "SessionStart":
      status = "started";
      break;
    case "PreInvocation":
      status = "thinking";
      break;
    case "PreToolUse":
    case "PostToolUse":
      status = toolStatusForName(tool);
      break;
    case "PostInvocation":
      status = "working";
      break;
    case "Stop":
      status = reason === "ERROR" ? "failed" : "completed";
      break;
    default:
      status = "event";
  }
  const messageParts = [event];
  if (tool) messageParts.push(tool);
  if (event === "Stop" && reason) messageParts.push(reason);
  if (event === "PostToolUse" && payload.error) messageParts.push("error");
  return {
    time: now.toISOString(),
    source: "antigravity",
    type: event,
    status,
    tool,
    sessionId: str(payload.conversationId),
    workspace: Array.isArray(payload.workspacePaths) ? str(payload.workspacePaths[0]) : "",
    message: messageParts.join(" "),
    model: str(payload.modelName),
    stepIdx: num(payload.stepIdx)
  };
}

// Ages the last event out the same way claude-session-status.js does: tool
// steps may legitimately run for a long time, so they get a longer window;
// a finished run is shown as completed for a hold period, then idle.
export function effectiveStatus(last, lastAt, now, windows) {
  if (!last) return { status: "idle", tool: "", reason: "no hook events yet" };
  const ageMs = now - lastAt;
  const ageS = Math.round(ageMs / 1000);
  if (TERMINAL_STATUSES.has(last.status)) {
    if (ageMs > windows.completedHoldMs) return { status: "idle", tool: "", reason: `${last.status} ${ageS}s ago` };
    return { status: last.status, tool: "", reason: "" };
  }
  const window = TOOL_STATUSES.has(last.status) ? windows.toolWindowMs : windows.activeWindowMs;
  if (ageMs > window) return { status: "idle", tool: "", reason: `stale: ${last.type} ${ageS}s ago` };
  return { status: last.status, tool: last.tool || "", reason: "" };
}

// Parses `agy -p /usage --output-format json` into plain groups/buckets.
export function parseQuotaDocument(doc) {
  return (doc?.command?.data?.groups ?? []).map((group) => ({
    name: str(group.name),
    buckets: (group.buckets ?? []).map((bucket) => ({
      id: str(bucket.id),
      window: str(bucket.window),
      remainingFraction: num(bucket.remaining_fraction),
      remainingPercent: Math.round((num(bucket.remaining_fraction) ?? 0) * 100),
      resetTime: str(bucket.reset_time)
    }))
  }));
}

// Compact, device-friendly line, e.g. "GEM WK 96% | 3P WK 100%" or
// "GEM 5H 80% WK 96% | 3P ..." when a 5-hour bucket is present.
export function quotaSummary(groups) {
  return groups
    .map((group) => {
      const tag = /gemini/i.test(group.name) ? "GEM" : /claude|gpt/i.test(group.name) ? "3P" : group.name.slice(0, 3).toUpperCase();
      const parts = group.buckets.map((bucket) => {
        const window = /5h/i.test(bucket.id) || /5h/i.test(bucket.window) ? "5H" : bucket.window === "weekly" ? "WK" : bucket.window.toUpperCase();
        return `${window} ${bucket.remainingPercent}%`;
      });
      return `${tag} ${parts.join(" ")}`;
    })
    .join(" | ");
}
