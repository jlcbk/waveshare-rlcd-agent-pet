// Pure translation logic for Google Antigravity hook payloads, transcript
// JSONL steps, and `agy -p /usage --output-format json` quota output.
// No I/O here. Prompt text (lastUserInput) and tool arguments are dropped.

export const HOOK_EVENTS = new Set([
  "SessionStart",
  "PreInvocation",
  "PreToolUse",
  "PostToolUse",
  "PostInvocation",
  "Stop"
]);
export const TOOL_STATUSES = new Set(["tool-use", "searching"]);
export const TERMINAL_STATUSES = new Set(["completed", "failed", "error"]);

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
  if (
    tool.includes("search") ||
    tool.includes("grep") ||
    tool.includes("find") ||
    tool.includes("glob") ||
    tool === "rg"
  ) {
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
      status = tool === "ask_question" ? "needs-attention" : toolStatusForName(tool);
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
export function effectiveStatus(last, lastAt, now, windows = {}) {
  const activeWindowMs = windows.activeWindowMs ?? 180_000;
  const toolWindowMs = windows.toolWindowMs ?? 900_000;
  const completedHoldMs = windows.completedHoldMs ?? 120_000;
  if (!last) return { status: "idle", tool: "", reason: "no hook events yet" };
  const ageMs = now - lastAt;
  const ageS = Math.round(ageMs / 1000);
  if (TERMINAL_STATUSES.has(last.status)) {
    if (ageMs > completedHoldMs) return { status: "idle", tool: "", reason: `${last.status} ${ageS}s ago` };
    return { status: last.status, tool: "", reason: "" };
  }
  const window = TOOL_STATUSES.has(last.status) || last.status === "needs-attention" ? toolWindowMs : activeWindowMs;
  if (ageMs > window) return { status: "idle", tool: "", reason: `stale: ${last.type} ${ageS}s ago` };
  return { status: last.status, tool: last.tool || "", reason: "" };
}

// Infers realtime state from ~/.gemini/<product>/brain/<id>/.system_generated/logs/transcript.jsonl
// lines so Antigravity IDE and CLI sessions are tracked even when hooks do not fire.
export function inferAntigravityRealtimeFromTranscriptLines(lines = []) {
  let state = null;
  let pendingToolCalls = 0;
  let completedToolCalls = 0;
  let lastTool = "";

  for (const rawLine of lines) {
    const line = String(rawLine || "").trim();
    if (!line) continue;
    let item;
    try {
      item = JSON.parse(line);
    } catch {
      continue;
    }
    if (!item || typeof item !== "object") continue;
    const type = str(item.type);
    const status = str(item.status).toUpperCase();
    const time = str(item.created_at) || state?.time || "";

    if (type === "USER_INPUT") {
      pendingToolCalls = 0;
      completedToolCalls = 0;
      lastTool = "";
      state = { status: "thinking", tool: "", time };
      continue;
    }

    if (type === "PLANNER_RESPONSE") {
      const toolCalls = Array.isArray(item.tool_calls) ? item.tool_calls : [];
      if (status === "ERROR") {
        pendingToolCalls = 0;
        completedToolCalls = 0;
        state = { status: "failed", tool: "", time };
      } else if (toolCalls.length > 0) {
        pendingToolCalls = toolCalls.length;
        completedToolCalls = 0;
        lastTool = str(toolCalls.at(-1)?.name);
        const toolStatus =
          lastTool === "ask_question" ? "needs-attention" : toolStatusForName(lastTool);
        state = { status: toolStatus, tool: lastTool, time };
      } else if (status === "RUNNING") {
        pendingToolCalls = 0;
        completedToolCalls = 0;
        state = { status: "thinking", tool: "", time };
      } else {
        pendingToolCalls = 0;
        completedToolCalls = 0;
        lastTool = "";
        state = { status: "completed", tool: "", time };
      }
      continue;
    }

    if (type === "GENERIC") {
      if (status === "RUNNING") {
        const toolStatus =
          lastTool === "ask_question" ? "needs-attention" : toolStatusForName(lastTool);
        state = { status: toolStatus, tool: lastTool, time };
      } else if (status === "DONE" || status === "ERROR") {
        completedToolCalls += 1;
        if (pendingToolCalls > 0 && completedToolCalls < pendingToolCalls) {
          const toolStatus =
            lastTool === "ask_question" ? "needs-attention" : toolStatusForName(lastTool);
          state = { status: toolStatus, tool: lastTool, time };
        } else {
          state = { status: "thinking", tool: lastTool, time };
        }
      }
    }
  }

  return state;
}

export function antigravityShortModelName(model) {
  const text = str(model);
  if (!text) return "";
  const withoutTier = text.replace(/\s*\([^)]*\)\s*$/, "").trim();
  if (withoutTier && withoutTier.length <= 18) return withoutTier;
  return (withoutTier || text).slice(0, 18);
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

export function formatResetCountdown(resetInput, nowMs = Date.now()) {
  if (resetInput === null || resetInput === undefined || resetInput === "") return "";
  let targetMs = NaN;
  if (typeof resetInput === "number" && Number.isFinite(resetInput)) {
    targetMs = resetInput < 1e11 ? resetInput * 1000 : resetInput;
  } else {
    const raw = str(resetInput).trim();
    if (!raw) return "";
    if (/^\d+(\.\d+)?$/.test(raw)) {
      const n = Number(raw);
      targetMs = n < 1e11 ? n * 1000 : n;
    } else {
      targetMs = Date.parse(raw);
    }
  }
  if (!Number.isFinite(targetMs)) return "";
  const diffMs = targetMs - nowMs;
  if (diffMs <= 0) return "";
  const totalMinutes = Math.max(1, Math.round(diffMs / 60000));
  const days = Math.floor(totalMinutes / (24 * 60));
  const hours = Math.floor((totalMinutes % (24 * 60)) / 60);
  const mins = totalMinutes % 60;
  if (days > 0) {
    return hours > 0 ? `${days}d${hours}h` : `${days}d`;
  }
  if (hours > 0) {
    return mins > 0 ? `${hours}h${mins}m` : `${hours}h`;
  }
  return `${mins}m`;
}

// Compact, device-friendly line, e.g. "GEM WK 96% | 3P WK 100%" or
// "GEM 5H 80% (3h18m) WK 96% (4d11h) | 3P ..." when reset times are present.
export function quotaSummary(groups, preferredModel = "", nowMs = Date.now()) {
  if (!Array.isArray(groups) || !groups.length) return "";
  const prefer3p = /claude|gpt|sonnet|opus|haiku|o1|o3|o4/i.test(str(preferredModel));
  const ordered = prefer3p
    ? [...groups].sort((a, b) => (/claude|gpt/i.test(b.name) ? 1 : 0) - (/claude|gpt/i.test(a.name) ? 1 : 0))
    : groups;
  return ordered
    .map((group) => {
      const tag = /gemini/i.test(group.name)
        ? "GEM"
        : /claude|gpt/i.test(group.name)
          ? "3P"
          : group.name.slice(0, 3).toUpperCase();
      const parts = group.buckets.map((bucket) => {
        const window =
          /5h/i.test(bucket.id) || /5h/i.test(bucket.window)
            ? "5H"
            : bucket.window === "weekly"
              ? "WK"
              : bucket.window.toUpperCase();
        const countdown = formatResetCountdown(bucket.resetTime, nowMs);
        return countdown
          ? `${window} ${bucket.remainingPercent}% (${countdown})`
          : `${window} ${bucket.remainingPercent}%`;
      });
      return `${tag} ${parts.join(" ")}`;
    })
    .join(" | ");
}
