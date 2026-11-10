#!/usr/bin/env node
import { execFileSync, spawnSync } from "node:child_process";
import { existsSync } from "node:fs";
import { readFile, writeFile, mkdir } from "node:fs/promises";
import { homedir } from "node:os";
import { dirname, join, resolve } from "node:path";
import { fileURLToPath } from "node:url";

import { DEFAULT_ANTIGRAVITY_GEMINI_DIR, loadAntigravityRealtimeState } from "./antigravity-db.js";
import { DEFAULT_CLAUDE_PROJECTS_DIR, loadClaudeRealtimeState } from "./claude-session-status.js";
import { buildHermesLogSyncNotifications } from "./hermes-log-channel.js";
import { readHermesLogActivity } from "./hermes-log-sync.js";

const HERE = dirname(fileURLToPath(import.meta.url));
const NOTIFY_CLIENT = resolve(HERE, "notify-client.js");
const DEFAULT_OUTBOX_PATH = resolve(
  process.env.PET_NOTIFY_OUTBOX || join(homedir(), ".codex-pet-bridge", "notify-outbox.jsonl")
);
const STATE_PATH = resolve(
  process.env.PET_AGENT_SYNC_STATE || join(homedir(), ".codex-pet-bridge", "agent-sync-state.json")
);
const CODEX_SESSIONS_DIR = resolve(process.env.CODEX_SESSIONS_DIR || join(homedir(), ".codex", "sessions"));
const CLAUDE_PROJECTS_DIR = resolve(process.env.PET_BRIDGE_CLAUDE_PROJECTS || DEFAULT_CLAUDE_PROJECTS_DIR);
const ANTIGRAVITY_GEMINI_DIR = resolve(
  process.env.PET_BRIDGE_ANTIGRAVITY_HOME || DEFAULT_ANTIGRAVITY_GEMINI_DIR
);
const HERMES_LOG_PATH = resolve(process.env.PET_AGENT_SYNC_HERMES_LOG || join(homedir(), ".hermes", "logs", "agent.log"));
const SOURCE_PREFIX = process.env.PET_AGENT_SYNC_PREFIX || "local";
const ACTIVE_WINDOW_MS = numberFromEnv("PET_AGENT_SYNC_ACTIVE_WINDOW_MS", 45000);
const HERMES_COMPLETED_WINDOW_MS = numberFromEnv("PET_AGENT_SYNC_HERMES_COMPLETED_WINDOW_MS", 30 * 60 * 1000);
const REFRESH_MS = numberFromEnv("PET_AGENT_SYNC_REFRESH_MS", 90000);
const WATCH_INTERVAL_MS = numberFromEnv("PET_AGENT_SYNC_INTERVAL_MS", 15000);

function isActiveRealtime(rt) {
  return Boolean(
    rt &&
      rt.status &&
      rt.status !== "idle" &&
      rt.status !== "completed" &&
      rt.status !== "failed" &&
      rt.status !== "error"
  );
}

function notifyArgsToEvent(extraArgs) {
  const out = {};
  for (let i = 0; i < extraArgs.length; i += 1) {
    const arg = extraArgs[i];
    if (arg === "--source") out.source = extraArgs[++i] || "";
    else if (arg === "--task") out.task = extraArgs[++i] || "";
    else if (arg === "--status") out.status = extraArgs[++i] || "";
    else if (arg === "--message") out.message = extraArgs[++i] || "";
    else if (arg === "--session-id") out.sessionId = extraArgs[++i] || "";
    else if (arg === "--workspace") out.workspace = extraArgs[++i] || "";
    else if (arg === "--notify") out.notify = true;
    else if (arg === "--no-notify") out.notify = false;
  }
  return out;
}

export async function runAgentSyncTick({
  statePath = STATE_PATH,
  sourcePrefix = SOURCE_PREFIX,
  codexSessionsDir = CODEX_SESSIONS_DIR,
  claudeProjectsDir = CLAUDE_PROJECTS_DIR,
  antigravityGeminiDir = ANTIGRAVITY_GEMINI_DIR,
  hermesLogPath = HERMES_LOG_PATH,
  codexRealtime = undefined,
  claudeRealtime = undefined,
  antigravityRealtime = undefined,
  emitEvent = null,
  flushOutbox = null
} = {}) {
  const state = await loadStateFrom(statePath);
  const sendEvent = async (payload) => {
    if (emitEvent) {
      await emitEvent(payload);
      return true;
    }
    const cliArgs = [
      "--source", payload.source || "",
      "--task", payload.task || "",
      "--status", payload.status || "running",
      "--message", payload.message || "",
      payload.notify ? "--notify" : "--no-notify"
    ];
    if (payload.sessionId) cliArgs.push("--session-id", payload.sessionId);
    if (payload.workspace) cliArgs.push("--workspace", payload.workspace);
    return notify(cliArgs);
  };

  const resolvedCodexRt = codexRealtime !== undefined ? codexRealtime : null;
  const codexIsActive =
    resolvedCodexRt !== null ? isActiveRealtime(resolvedCodexRt) : await codexActive(codexSessionsDir);
  await updateChannel(state, sendEvent, {
    key: "codex",
    active: codexIsActive,
    source: `${sourcePrefix}-codex`,
    task: resolvedCodexRt?.task || `${sourcePrefix}-codex-runtime`,
    workspace: resolvedCodexRt?.workspace || "",
    sessionId: resolvedCodexRt?.sessionId || "",
    runningMessage: `${sourcePrefix} Codex task is running`,
    doneMessage: `${sourcePrefix} Codex task finished`
  });

  const resolvedClaudeRt =
    claudeRealtime !== undefined
      ? claudeRealtime
      : await resolveClaudeState(claudeProjectsDir, sourcePrefix);
  const claudeIsActive =
    resolvedClaudeRt !== null
      ? isActiveRealtime(resolvedClaudeRt)
      : process.env.PET_AGENT_SYNC_CLAUDE !== "0" &&
        (!existsSync(claudeProjectsDir) || process.env.PET_AGENT_SYNC_CLAUDE_PROCESS_SCAN === "1") &&
        claudeProcessActive();
  await updateChannel(state, sendEvent, {
    key: "claude",
    active: claudeIsActive,
    source: `${sourcePrefix}-claude`,
    task: resolvedClaudeRt?.task || `${sourcePrefix}-claude-session`,
    workspace: resolvedClaudeRt?.workspace || "",
    sessionId: resolvedClaudeRt?.sessionId || "",
    runningMessage: `${sourcePrefix} Claude Code is working`,
    doneMessage: `${sourcePrefix} Claude Code response stopped`
  });

  const resolvedAgRt =
    antigravityRealtime !== undefined
      ? antigravityRealtime
      : await resolveAntigravityState(antigravityGeminiDir, sourcePrefix);
  await updateChannel(state, sendEvent, {
    key: "antigravity",
    active: isActiveRealtime(resolvedAgRt),
    source: `${sourcePrefix}-antigravity`,
    task: resolvedAgRt?.task || `${sourcePrefix}-antigravity-session`,
    workspace: resolvedAgRt?.workspace || "",
    sessionId: "",
    runningMessage: `${sourcePrefix} Antigravity is working`,
    doneMessage: `${sourcePrefix} Antigravity response stopped`
  });

  await updateHermesLogChannel(state, sendEvent, hermesLogPath);
  if (flushOutbox) {
    await flushOutbox();
  } else if (!emitEvent && existsSync(DEFAULT_OUTBOX_PATH)) {
    notify(["--flush"]);
  }
  state.updatedAt = new Date().toISOString();
  await saveStateTo(statePath, state);
}

async function updateChannel(state, sendEvent, options) {
  const now = Date.now();
  const wasActive = Boolean(state[`${options.key}Active`]);
  const lastNotify = Number(state[`${options.key}LastRunningNotify`] || 0);
  if (options.active) {
    if (options.workspace) state[`${options.key}LastWorkspace`] = options.workspace;
    if (options.sessionId) state[`${options.key}LastSessionId`] = options.sessionId;
    if (options.task) state[`${options.key}LastTask`] = options.task;
    if (!wasActive || now - lastNotify >= REFRESH_MS) {
      await sendEvent({
        source: options.source,
        task: options.task,
        workspace: options.workspace || "",
        sessionId: options.sessionId || "",
        status: "running",
        message: options.runningMessage,
        notify: false
      });
      state[`${options.key}LastRunningNotify`] = now;
    }
    state[`${options.key}Active`] = true;
    state[`${options.key}LastSeen`] = now;
  } else if (wasActive) {
    await sendEvent({
      source: options.source,
      task: options.task || state[`${options.key}LastTask`] || "",
      workspace: options.workspace || state[`${options.key}LastWorkspace`] || "",
      sessionId: options.sessionId || state[`${options.key}LastSessionId`] || "",
      status: "completed",
      message: options.doneMessage,
      notify: true
    });
    state[`${options.key}Active`] = false;
    state[`${options.key}LastRunningNotify`] = 0;
  }
}

async function updateHermesLogChannel(state, sendEvent, hermesLogPath = HERMES_LOG_PATH) {
  const activity = readHermesLogActivity(hermesLogPath, {
    activeWindowMs: ACTIVE_WINDOW_MS,
    completedWindowMs: HERMES_COMPLETED_WINDOW_MS
  });
  const { notifications } = buildHermesLogSyncNotifications(state, activity, {
    refreshMs: REFRESH_MS
  });
  for (const notifyArgs of notifications) {
    await sendEvent(notifyArgsToEvent(notifyArgs));
  }
}

function notify(extraArgs) {
  const result = spawnSync(process.execPath, [NOTIFY_CLIENT, ...extraArgs], {
    stdio: "ignore",
    timeout: numberFromEnv("PET_AGENT_SYNC_NOTIFY_TIMEOUT_MS", 3000)
  });
  return result.status === 0;
}

async function codexActive(codexSessionsDir = CODEX_SESSIONS_DIR) {
  return (await codexSessionActive(codexSessionsDir)) || codexCliActive();
}

async function codexSessionActive(codexSessionsDir = CODEX_SESSIONS_DIR) {
  if (!existsSync(codexSessionsDir)) return false;
  const now = Date.now();
  const files = listRecentJsonl(codexSessionsDir, 12);
  for (const path of files) {
    const age = now - path.mtimeMs;
    if (age > ACTIVE_WINDOW_MS) continue;
    const payload = await lastJsonPayload(path.path);
    if (!payload) continue;
    if (payload.completed_at || ["completed", "turn_complete"].includes(payload.reason)) continue;
    return true;
  }
  return false;
}

function codexCliActive() {
  for (const proc of listProcesses()) {
    if (proc.pid === process.pid) continue;
    const command = proc.command.toLowerCase();
    if (!/\bcodex\b/.test(command)) continue;
    if (!command.includes(" exec ")) continue;
    if (proc.cpu >= 1 || proc.stat.includes("R")) return true;
  }
  return false;
}

async function resolveClaudeState(claudeProjectsDir, sourcePrefix) {
  if (process.env.PET_AGENT_SYNC_CLAUDE === "0") return null;
  if (!existsSync(claudeProjectsDir)) return null;
  return loadClaudeRealtimeState({
    projectsDir: claudeProjectsDir,
    activeWindowMs: ACTIVE_WINDOW_MS,
    sourcePrefix
  }).catch(() => null);
}

async function resolveAntigravityState(antigravityGeminiDir, sourcePrefix) {
  if (process.env.PET_AGENT_SYNC_ANTIGRAVITY === "0") return null;
  if (!existsSync(antigravityGeminiDir)) return null;
  return loadAntigravityRealtimeState({
    geminiDir: antigravityGeminiDir,
    activeWindowMs: Math.max(ACTIVE_WINDOW_MS, 180_000),
    sourcePrefix
  }).catch(() => null);
}

function claudeProcessActive() {
  if (process.env.PET_AGENT_SYNC_CLAUDE_PROCESS_SCAN === "0") return false;
  for (const proc of listProcesses()) {
    if (proc.pid === process.pid) continue;
    const command = proc.command.toLowerCase();
    if (!/\bclaude\b/.test(command)) continue;
    if (command.includes("claude-hook") || command.includes("agent-sync")) continue;
    // The Claude desktop app (and its helpers) is not Claude Code.
    if (command.includes(".app/")) continue;
    if (proc.cpu >= 1 || proc.stat.includes("R")) return true;
  }
  return false;
}

function listProcesses() {
  try {
    const output = execFileSync("/bin/ps", ["-axo", "pid=,stat=,pcpu=,command="], {
      encoding: "utf8",
      timeout: 1000
    });
    return output
      .split("\n")
      .map((line) => line.trim())
      .filter(Boolean)
      .map((line) => {
        const parts = line.split(/\s+/);
        return {
          pid: Number(parts[0]),
          stat: parts[1] || "",
          cpu: Number(parts[2] || 0),
          command: parts.slice(3).join(" ")
        };
      })
      .filter((proc) => Number.isFinite(proc.pid));
  } catch {
    return [];
  }
}

function listRecentJsonl(root, limit) {
  try {
    const output = execFileSync("/usr/bin/find", [root, "-type", "f", "-name", "*.jsonl", "-print"], {
      encoding: "utf8",
      timeout: 1000
    });
    return output
      .split("\n")
      .filter(Boolean)
      .map((path) => {
        try {
          const statOutput = execFileSync("/usr/bin/stat", ["-f", "%m", path], { encoding: "utf8", timeout: 500 });
          return { path, mtimeMs: Number(statOutput.trim()) * 1000 };
        } catch {
          return null;
        }
      })
      .filter(Boolean)
      .sort((a, b) => b.mtimeMs - a.mtimeMs)
      .slice(0, limit);
  } catch {
    return [];
  }
}

async function lastJsonPayload(path) {
  try {
    const text = await readFile(path, "utf8");
    const lines = text.split("\n").filter(Boolean).slice(-80);
    let payload = null;
    for (const line of lines) {
      try {
        const record = JSON.parse(line);
        payload = record.payload || record;
      } catch {
        // Ignore malformed partial lines.
      }
    }
    return payload;
  } catch {
    return null;
  }
}

async function loadStateFrom(statePath = STATE_PATH) {
  try {
    return JSON.parse(await readFile(statePath, "utf8"));
  } catch {
    return {};
  }
}

async function saveStateTo(statePath = STATE_PATH, state = {}) {
  await mkdir(dirname(statePath), { recursive: true });
  await writeFile(statePath, `${JSON.stringify(state, null, 2)}\n`, "utf8");
}

function numberFromEnv(name, fallback) {
  const value = Number(process.env[name]);
  return Number.isFinite(value) && value > 0 ? value : fallback;
}

function printHelp() {
  console.log(`Usage:
  pet-agent-sync [--tick] [--watch]

Environment:
  PET_AGENT_SYNC_PREFIX=laptop
  PET_BRIDGE_URL=http://127.0.0.1:17366/events
  CODEX_SESSIONS_DIR=~/.codex/sessions
  PET_BRIDGE_CLAUDE_PROJECTS=~/.claude/projects
  PET_AGENT_SYNC_CLAUDE=0                  (skip Claude Code entirely)
  PET_AGENT_SYNC_CLAUDE_PROCESS_SCAN=1     (also scan processes when transcripts exist)
  PET_AGENT_SYNC_HERMES_LOG=~/.hermes/logs/agent.log
  PET_AGENT_SYNC_INTERVAL_MS=15000
`);
}

const isMainModule =
  Boolean(process.argv[1]) && resolve(process.argv[1]) === fileURLToPath(import.meta.url);

if (isMainModule) {
  const args = new Set(process.argv.slice(2));
  if (args.has("--help")) {
    printHelp();
    process.exit(0);
  }

  if (args.has("--watch")) {
    await runAgentSyncTick();
    setInterval(() => void runAgentSyncTick(), WATCH_INTERVAL_MS);
  } else {
    await runAgentSyncTick();
  }
}
