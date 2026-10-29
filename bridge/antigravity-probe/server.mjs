#!/usr/bin/env node
// antigravity-probe: standalone monitor for Google Antigravity's working state.
//
// Push path - Antigravity lifecycle hooks (see install-hooks.sh) POST their stdin
// payload to POST /hook/<Event>, <Event> being one of SessionStart, PreInvocation,
// PreToolUse, PostToolUse, PostInvocation, Stop. The payload itself carries no event
// name, which is why it travels in the URL. Every hook is translated (lib/translate.mjs)
// into the event shape codex-pet-bridge's normalizeEvent() already accepts, kept as
// local state and, when PROBE_FORWARD_URL is set, forwarded verbatim to the bridge's
// POST /events.
//
// Pull path - the local SQLite conversation stores (model, context window, session
// token totals; lib/antigravity-db.mjs) and `agy -p /usage --output-format json`
// (quota) at a low cadence.
//
// Loopback only. Prompt text and tool arguments never reach memory, disk or the log.

import http from "node:http";
import { execFile } from "node:child_process";
import { HOOK_EVENTS, effectiveStatus, parseQuotaDocument, quotaSummary, str, translateHook } from "./lib/translate.mjs";

const HOST = "127.0.0.1";
const PORT = intEnv("PROBE_PORT", 8791);
const TOKEN = process.env.PROBE_TOKEN || "";
const FORWARD_URL = process.env.PROBE_FORWARD_URL || "";
const FORWARD_TOKEN = process.env.PROBE_FORWARD_TOKEN || "";
const WINDOWS = {
  activeWindowMs: intEnv("PROBE_ACTIVE_WINDOW_MS", 180_000),
  toolWindowMs: intEnv("PROBE_TOOL_WINDOW_MS", 900_000),
  completedHoldMs: intEnv("PROBE_COMPLETED_HOLD_MS", 120_000)
};
const QUOTA_INTERVAL_MS = intEnv("PROBE_QUOTA_INTERVAL_MS", 10 * 60_000);
const QUOTA_CMD = process.env.PROBE_QUOTA_CMD || "agy";
const MAX_BODY_BYTES = 1 << 20;
const HISTORY_LIMIT = 50;

let db = null;
try {
  db = await import("./lib/antigravity-db.mjs");
} catch (error) {
  log(`pull path disabled, lib/antigravity-db.mjs not loadable: ${error.message}`);
}

const state = { last: null, lastAt: 0, model: "", conversationId: "", counts: {}, history: [] };
const quota = { at: null, groups: null, summary: "", error: null, errorAt: null };
const startedAt = Date.now();

function intEnv(name, fallback) {
  const parsed = Number.parseInt(process.env[name] ?? "", 10);
  return Number.isFinite(parsed) ? parsed : fallback;
}

function log(message) {
  console.log(`[probe ${new Date().toLocaleTimeString("en-GB", { hour12: false })}] ${message}`);
}

function applyEvent(event) {
  state.last = event;
  state.lastAt = Date.now();
  if (event.sessionId) state.conversationId = event.sessionId;
  if (event.model && event.model !== "auto") state.model = event.model;
  state.counts[event.type] = (state.counts[event.type] || 0) + 1;
  state.history.push(event);
  if (state.history.length > HISTORY_LIMIT) state.history.splice(0, state.history.length - HISTORY_LIMIT);
}

function readJson(req) {
  return new Promise((resolve) => {
    const chunks = [];
    let size = 0;
    req.on("data", (chunk) => {
      size += chunk.length;
      if (size <= MAX_BODY_BYTES) chunks.push(chunk);
    });
    req.on("end", () => {
      if (size > MAX_BODY_BYTES) return resolve({});
      try {
        resolve(JSON.parse(Buffer.concat(chunks).toString("utf8") || "{}"));
      } catch {
        resolve({});
      }
    });
    req.on("error", () => resolve({}));
  });
}

function sendJson(res, code, body) {
  const text = JSON.stringify(body);
  res.writeHead(code, { "content-type": "application/json; charset=utf-8", "content-length": Buffer.byteLength(text) });
  res.end(text);
}

function authorized(url, req) {
  if (!TOKEN) return true;
  return url.searchParams.get("t") === TOKEN || req.headers["x-probe-token"] === TOKEN;
}

async function forward(event) {
  if (!FORWARD_URL) return;
  try {
    const headers = { "content-type": "application/json" };
    if (FORWARD_TOKEN) headers.authorization = `Bearer ${FORWARD_TOKEN}`;
    const res = await fetch(FORWARD_URL, { method: "POST", headers, body: JSON.stringify(event), signal: AbortSignal.timeout(1500) });
    if (!res.ok) log(`forward -> HTTP ${res.status}`);
  } catch (error) {
    log(`forward failed: ${error.message}`);
  }
}

function refreshQuota() {
  return new Promise((resolve) => {
    execFile(QUOTA_CMD, ["-p", "/usage", "--output-format", "json"], { timeout: 45_000, maxBuffer: 1 << 20 }, (error, stdout) => {
      const now = new Date().toISOString();
      try {
        if (error) throw new Error(error.message.split("\n")[0]);
        const groups = parseQuotaDocument(JSON.parse(stdout));
        Object.assign(quota, { groups, summary: quotaSummary(groups), at: now, error: null });
        log(`quota: ${quota.summary || "(no groups)"}`);
      } catch (failure) {
        Object.assign(quota, { error: failure.message, errorAt: now });
        log(`quota refresh failed: ${quota.error}`);
      }
      resolve();
    });
  });
}

async function buildStatus(refresh) {
  if (refresh) await refreshQuota();
  const now = Date.now();
  const effective = effectiveStatus(state.last, state.lastAt, now, WINDOWS);
  let snapshot = null;
  let pull = db ? { ok: true } : { disabled: true };
  if (db) {
    try {
      snapshot = await db.readSnapshot();
    } catch (error) {
      pull = { error: error.message };
    }
  }
  const detail = snapshot?.activeDetail ?? null;
  const generation = detail?.generations?.last ?? null;
  const active = snapshot?.active ?? null;
  return {
    source: "antigravity",
    status: effective.status,
    statusReason: effective.reason,
    tool: effective.tool,
    lastEvent: state.last
      ? { type: state.last.type, status: state.last.status, tool: state.last.tool, sessionId: state.last.sessionId, time: state.last.time, ageMs: now - state.lastAt }
      : null,
    eventCounts: state.counts,
    model: generation?.modelDisplay || state.model || "",
    modelId: generation?.modelId || "",
    context: generation?.context ?? null,
    sessionTokens: detail?.generations?.totals ?? null,
    activeConversation: active
      ? {
          id: active.conversationId,
          product: active.product,
          mtime: new Date(active.mtimeMs).toISOString(),
          locked: Boolean(snapshot?.locks?.some((lock) => lock.conversationId === active.conversationId)),
          steps: detail?.steps ?? null
        }
      : null,
    conversations: snapshot?.conversations?.length ?? null,
    pull,
    quota,
    probe: { port: PORT, forward: FORWARD_URL ? "on" : "off", ...WINDOWS, quotaIntervalMs: QUOTA_INTERVAL_MS, uptimeS: Math.round((now - startedAt) / 1000) }
  };
}

const server = http.createServer(async (req, res) => {
  try {
    const url = new URL(req.url || "/", `http://${HOST}:${PORT}`);
    if (!authorized(url, req)) return sendJson(res, 401, { error: "unauthorized" });

    const hookMatch = url.pathname.match(/^\/hook\/([A-Za-z]+)$/);
    if (req.method === "POST" && (hookMatch || url.pathname === "/events")) {
      const eventName = hookMatch ? hookMatch[1] : str(url.searchParams.get("event"));
      if (!HOOK_EVENTS.has(eventName)) return sendJson(res, 400, { error: `unknown hook event '${eventName}'` });
      const event = translateHook(eventName, await readJson(req));
      applyEvent(event);
      log(`${event.type}${event.tool ? ` ${event.tool}` : ""} -> ${event.status}${event.sessionId ? ` (conv ${event.sessionId.slice(0, 8)})` : ""}`);
      void forward(event);
      return sendJson(res, 202, { ok: true, status: event.status });
    }
    if (req.method === "GET" && url.pathname === "/status") return sendJson(res, 200, await buildStatus(url.searchParams.has("refreshQuota")));
    if (req.method === "GET" && url.pathname === "/events") return sendJson(res, 200, { events: state.history });
    if (req.method === "GET" && url.pathname === "/health") return sendJson(res, 200, { ok: true, uptimeS: Math.round((Date.now() - startedAt) / 1000) });
    return sendJson(res, 404, { error: "not found" });
  } catch (error) {
    return sendJson(res, 500, { error: error.message });
  }
});

server.listen(PORT, HOST, () => {
  log(`listening on http://${HOST}:${PORT} (forward ${FORWARD_URL ? `on -> ${FORWARD_URL}` : "off"}, pull ${db ? "on" : "off"}, quota every ${QUOTA_INTERVAL_MS > 0 ? `${QUOTA_INTERVAL_MS / 1000}s` : "never"})`);
  if (QUOTA_INTERVAL_MS > 0) {
    void refreshQuota();
    setInterval(() => void refreshQuota(), QUOTA_INTERVAL_MS);
  }
});

for (const signal of ["SIGINT", "SIGTERM"]) {
  process.on(signal, () => {
    log(`${signal}: shutting down`);
    server.close(() => process.exit(0));
    setTimeout(() => process.exit(0), 500).unref();
  });
}
