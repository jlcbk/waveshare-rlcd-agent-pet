export function inferCodexRealtimeStateFromLines(lines) {
  for (let index = lines.length - 1; index >= 0; index -= 1) {
    const state = inferCodexRealtimeStateFromLine(lines[index]);
    if (state) return state;
  }
  return null;
}

function summarizeCodexFunctionCall(payload) {
  const name = typeof payload?.name === "string" ? payload.name.trim() : "";
  if (!name) return "";
  if (typeof payload.arguments === "string" && payload.arguments.trim()) {
    try {
      const args = JSON.parse(payload.arguments);
      const rawCmd =
        typeof args?.cmd === "string"
          ? args.cmd
          : Array.isArray(args?.command)
            ? String(args.command.at(-1) || "")
            : typeof args?.command === "string"
              ? args.command
              : "";
      const firstLine = rawCmd.split("\n")[0].trim();
      if (firstLine) {
        return `${name}: ${firstLine.slice(0, 36)}`;
      }
    } catch {
      // ignore non-JSON arguments
    }
  }
  return name;
}

export function inferCodexRealtimeStateFromLine(line) {
  if (!line || !line.trim()) return null;

  let item;
  try {
    item = JSON.parse(line);
  } catch {
    return null;
  }

  const payload = item?.payload && typeof item.payload === "object" ? item.payload : {};
  const time = typeof item.timestamp === "string" ? item.timestamp : "";

  if (item.type === "event_msg") {
    switch (payload.type) {
      case "task_complete":
      case "turn_aborted":
        return { status: "completed", time };
      case "task_started":
      case "user_message":
      case "agent_message":
        return { status: "thinking", time };
      default:
        return null;
    }
  }

  if (item.type !== "response_item") return null;

  switch (payload.type) {
    case "reasoning":
    case "function_call_output":
      return { status: "thinking", time };
    case "web_search_call":
      return { status: "searching", time, tool: "web_search", detail: "web_search" };
    case "function_call": {
      const tool = typeof payload.name === "string" ? payload.name : "";
      const detail = summarizeCodexFunctionCall(payload);
      if (payload.name === "request_user_input") {
        return { status: "needs-attention", time, tool, detail };
      }
      return { status: "tool-use", time, tool, detail };
    }
    case "message":
      if (payload.role === "user" || payload.role === "assistant") {
        return { status: "thinking", time };
      }
      return null;
    default:
      return null;
  }
}

/**
 * Pick the best current-state view from two sources.
 *
 * Prefers `codexRealtime` (finer-grained JSONL-derived state) when it is
 * at least as recent as the bridge event. On timestamp tie, realtime wins
 * because it carries more detail than agent-sync "running"/"completed".
 */
export function pickCurrentState(eventCurrent, codexRealtime) {
  const realtimeTime = Date.parse(codexRealtime?.time || 0);
  const eventTime = Date.parse(eventCurrent?.time || 0);
  const eventFamily = sourceFamily(eventCurrent?.source);
  const realtimeFamily = sourceFamily(codexRealtime?.source);

  if (codexRealtime && (!eventFamily || eventFamily === realtimeFamily) && realtimeTime >= eventTime) {
    return codexRealtime;
  }

  if (eventCurrent) {
    const out = {
      source: eventCurrent.source || "",
      task: eventCurrent.task || "",
      status: eventCurrent.status || "idle",
      time: eventCurrent.time || ""
    };
    if (eventCurrent.sessionId) out.sessionId = eventCurrent.sessionId;
    if (eventCurrent.workspace) out.workspace = eventCurrent.workspace;
    if (eventCurrent.project) out.project = eventCurrent.project;
    if (eventCurrent.tool) out.tool = eventCurrent.tool;
    if (eventCurrent.detail) out.detail = eventCurrent.detail;
    return out;
  }

  return null;
}

const ACTIVE_STATUSES = new Set([
  "needs-attention",
  "running",
  "working",
  "thinking",
  "searching",
  "tool-use",
  "started",
  "progress",
  "near-complete"
]);

const SHELL_TOOLS = new Set([
  "run_command",
  "send_command_input",
  "manage_task",
  "command_status",
  "bash",
  "exec_command",
  "write_stdin",
  "shell"
]);

function isShellTool(tool) {
  return SHELL_TOOLS.has(String(tool || "").trim().toLowerCase());
}

function activityTier(item) {
  const status = item?.status;
  if (status === "needs-attention") return 0;
  if (ACTIVE_STATUSES.has(status)) {
    if (status === "tool-use" && isShellTool(item?.tool)) return 2;
    return 1;
  }
  return 3;
}

function timeValue(item) {
  const value = Date.parse(item?.time || 0);
  return Number.isFinite(value) ? value : 0;
}

function newest(items) {
  let best = null;
  for (const item of items) {
    if (!best) {
      best = item;
      continue;
    }
    const tierDiff = activityTier(item) - activityTier(best);
    if (tierDiff < 0 || (tierDiff === 0 && timeValue(item) > timeValue(best))) {
      best = item;
    }
  }
  return best;
}

/**
 * Pick the auto "current" state from the latest bridge event and several
 * realtime views (Codex rollout, Claude Code transcript, ...).
 *
 * The realtime view of the event's own agent family refines it exactly like
 * pickCurrentState. When the result is no longer active (or is a lower-tier
 * terminal shell task while another agent is actively consuming an LLM),
 * the higher-priority active agent takes over.
 */
export function pickAutoCurrent(eventCurrent, realtimes = []) {
  const list = (Array.isArray(realtimes) ? realtimes : [realtimes]).filter(Boolean);
  const eventFamily = sourceFamily(eventCurrent?.source);
  const sameFamily = newest(list.filter((item) => !eventFamily || sourceFamily(item.source) === eventFamily));
  const refined = pickCurrentState(eventCurrent, sameFamily);
  const refinedTier = activityTier(refined);
  if (refined && ACTIVE_STATUSES.has(refined.status)) {
    const higherPriorityTakeover = newest(
      list.filter((item) => item !== refined && ACTIVE_STATUSES.has(item.status) && activityTier(item) < refinedTier)
    );
    return higherPriorityTakeover || refined;
  }
  const refinedTime = timeValue(refined);
  const takeover = newest(
    list.filter((item) => item !== refined && ACTIVE_STATUSES.has(item.status) && timeValue(item) > refinedTime)
  );
  return takeover || refined || newest(list);
}

function sourceFamily(source) {
  const text = String(source || "").toLowerCase();
  if (text.includes("codex")) return "codex";
  if (text.includes("claude")) return "claude";
  if (text.includes("antigravity") || text === "agy" || text.endsWith("-agy")) return "antigravity";
  if (text.includes("hermes")) return "hermes";
  if (text.includes("openclaw")) return "openclaw";
  return "";
}
