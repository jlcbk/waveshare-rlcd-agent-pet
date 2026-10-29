import assert from "node:assert/strict";
import { execFile, spawn } from "node:child_process";
import { mkdir, mkdtemp, rm, utimes, writeFile } from "node:fs/promises";
import { tmpdir } from "node:os";
import { join } from "node:path";
import test from "node:test";
import { promisify } from "node:util";

import {
  antigravityTokensToday,
  decodeGenerationMeta,
  decodeWorkspaceMeta,
  loadAntigravityRealtimeState,
  readConversation
} from "../src/antigravity-db.js";
import {
  antigravityShortModelName,
  effectiveStatus,
  inferAntigravityRealtimeFromTranscriptLines,
  parseQuotaDocument,
  quotaSummary,
  translateHook
} from "../src/antigravity-translate.js";
import { pickAutoCurrent } from "../src/codex-session-status.js";

const execFileAsync = promisify(execFile);

function encodeVarint(value) {
  let v = BigInt(value);
  if (v < 0n) v = (1n << 64n) + v;
  const bytes = [];
  while (v >= 0x80n) {
    bytes.push(Number((v & 0x7fn) | 0x80n));
    v >>= 7n;
  }
  bytes.push(Number(v));
  return Buffer.from(bytes);
}

function pbVarint(fieldNo, value) {
  return Buffer.concat([encodeVarint((fieldNo << 3) | 0), encodeVarint(value)]);
}

function pbBytes(fieldNo, buf) {
  const b = Buffer.isBuffer(buf) ? buf : Buffer.from(String(buf), "utf8");
  return Buffer.concat([encodeVarint((fieldNo << 3) | 2), encodeVarint(b.length), b]);
}

function buildGenMetaBlob({
  modelEnum = 1037,
  input = 1000,
  cacheRead = 5000,
  thinking = 100,
  output = 200,
  totalOutput = 300,
  ctxCurrent = 12345,
  ctxMax = 256000,
  modelId = "gemini-4-argon",
  modelDisplay = "Gemini 4 Argon (Medium)"
} = {}) {
  const usage = Buffer.concat([
    pbVarint(1, modelEnum),
    pbVarint(2, input),
    pbVarint(3, totalOutput),
    pbVarint(5, cacheRead),
    pbVarint(9, thinking),
    pbVarint(10, output)
  ]);
  const ctx = Buffer.concat([pbVarint(1, ctxCurrent), pbVarint(4, ctxMax)]);
  const holder = Buffer.concat([pbVarint(2, -1n), pbBytes(10, ctx)]);
  const gen = Buffer.concat([
    pbBytes(4, usage),
    pbBytes(9, holder),
    pbBytes(19, modelId),
    pbBytes(21, modelDisplay)
  ]);
  return pbBytes(1, gen);
}

test("Antigravity hook translation and privacy redaction", () => {
  const now = new Date("2026-10-29T02:00:00.000Z");
  const preTool = translateHook(
    "PreToolUse",
    {
      conversationId: "conv-123",
      workspacePaths: ["/Users/me/project"],
      modelName: "gemini-4-argon",
      lastUserInput: "TOP_SECRET_PROMPT_TEXT",
      toolCall: { name: "grep_search", args: { query: "SECRET_QUERY" } },
      stepIdx: 4
    },
    now
  );
  assert.equal(preTool.source, "antigravity");
  assert.equal(preTool.status, "searching");
  assert.equal(preTool.tool, "grep_search");
  assert.equal(preTool.sessionId, "conv-123");
  assert.equal(preTool.workspace, "/Users/me/project");
  assert.ok(!JSON.stringify(preTool).includes("TOP_SECRET_PROMPT_TEXT"));
  assert.ok(!JSON.stringify(preTool).includes("SECRET_QUERY"));

  const askTool = translateHook("PreToolUse", { toolCall: { name: "ask_question" } }, now);
  assert.equal(askTool.status, "needs-attention");

  const stopOk = translateHook("Stop", { terminationReason: "COMPLETED" }, now);
  assert.equal(stopOk.status, "completed");

  const stopErr = translateHook("Stop", { terminationReason: "ERROR" }, now);
  assert.equal(stopErr.status, "failed");
});

test("Antigravity effectiveStatus ageing windows and quota formatting", () => {
  const t0 = 1_000_000;
  const windows = { activeWindowMs: 180_000, toolWindowMs: 900_000, completedHoldMs: 120_000 };
  const toolEv = { type: "PreToolUse", status: "tool-use", tool: "run_command" };
  assert.equal(effectiveStatus(toolEv, t0, t0 + 300_000, windows).status, "tool-use");
  assert.equal(effectiveStatus(toolEv, t0, t0 + 950_000, windows).status, "idle");

  const doneEv = { type: "Stop", status: "completed", tool: "" };
  assert.equal(effectiveStatus(doneEv, t0, t0 + 60_000, windows).status, "completed");
  assert.equal(effectiveStatus(doneEv, t0, t0 + 130_000, windows).status, "idle");

  const groups = parseQuotaDocument({
    command: {
      data: {
        groups: [
          {
            name: "Gemini Models",
            buckets: [
              { id: "gemini-weekly", window: "weekly", remaining_fraction: 0.964 },
              { id: "gemini-5h", window: "5h", remaining_fraction: 0.892 }
            ]
          },
          {
            name: "Claude and GPT models",
            buckets: [{ id: "3p-weekly", window: "weekly", remaining_fraction: 1 }]
          }
        ]
      }
    }
  });
  assert.equal(quotaSummary(groups), "GEM WK 96% 5H 89% | 3P WK 100%");
  assert.equal(quotaSummary(groups, "Claude Sonnet 4.5"), "3P WK 100% | GEM WK 96% 5H 89%");
  const groupsWithReset = parseQuotaDocument({
    command: {
      data: {
        groups: [
          {
            name: "Gemini Models",
            buckets: [
              { id: "gemini-weekly", window: "weekly", remaining_fraction: 0.94, reset_time: "2026-11-02T14:00:00Z" },
              { id: "gemini-5h", window: "5h", remaining_fraction: 0.85, reset_time: "2026-10-29T06:18:00Z" }
            ]
          }
        ]
      }
    }
  });
  const nowMs = Date.parse("2026-10-29T03:00:00Z");
  assert.equal(quotaSummary(groupsWithReset, "", nowMs), "GEM WK 94% (4d11h) 5H 85% (3h18m)");
  assert.equal(antigravityShortModelName("Gemini 4 Argon (Medium)"), "Gemini 4 Argon");
});

test("Antigravity transcript JSONL inference tracks thinking, tools and completion", () => {
  const steps = [
    { type: "USER_INPUT", status: "DONE", created_at: "2026-10-29T02:00:00Z" },
    {
      type: "PLANNER_RESPONSE",
      status: "DONE",
      created_at: "2026-10-29T02:00:02Z",
      tool_calls: [{ name: "grep_search" }]
    },
    { type: "GENERIC", status: "DONE", created_at: "2026-10-29T02:00:03Z" },
    {
      type: "PLANNER_RESPONSE",
      status: "DONE",
      created_at: "2026-10-29T02:00:05Z",
      tool_calls: [{ name: "run_command" }, { name: "read_file" }]
    },
    { type: "GENERIC", status: "DONE", created_at: "2026-10-29T02:00:06Z" },
    { type: "GENERIC", status: "DONE", created_at: "2026-10-29T02:00:07Z" },
    {
      type: "PLANNER_RESPONSE",
      status: "DONE",
      created_at: "2026-10-29T02:00:10Z",
      tool_calls: null
    }
  ];
  const statuses = steps.map((_, idx) =>
    inferAntigravityRealtimeFromTranscriptLines(steps.slice(0, idx + 1).map((s) => JSON.stringify(s)))?.status
  );
  assert.deepEqual(statuses, [
    "thinking",
    "searching",
    "thinking",
    "tool-use",
    "tool-use",
    "thinking",
    "completed"
  ]);
});

test("Antigravity DB decoding, realtime state and bridge-server integration", async () => {
  const blob = buildGenMetaBlob({
    input: 2500,
    cacheRead: 60000,
    thinking: 300,
    output: 400,
    totalOutput: 700,
    ctxCurrent: 64200,
    ctxMax: 256000,
    modelId: "gemini-4-argon",
    modelDisplay: "Gemini 4 Argon (Medium)"
  });
  const decoded = decodeGenerationMeta(blob);
  assert.equal(decoded.modelDisplay, "Gemini 4 Argon (Medium)");
  assert.deepEqual(decoded.context, { current: 64200, max: 256000 });
  assert.equal(decoded.usage.input, 2500);

  const wsBlob = pbBytes(1, pbBytes(7, "file:///Users/cui/Documents/Projects/RLCD_firm1"));
  assert.equal(decodeWorkspaceMeta(wsBlob), "/Users/cui/Documents/Projects/RLCD_firm1");

  const tempRoot = await mkdtemp(join(tmpdir(), "pet-antigravity-"));
  let server;
  try {
    const geminiDir = join(tempRoot, "gemini");
    const convDir = join(geminiDir, "antigravity", "conversations");
    const logDir = join(geminiDir, "antigravity", "brain", "conv-ag-1", ".system_generated", "logs");
    await mkdir(convDir, { recursive: true });
    await mkdir(logDir, { recursive: true });

    const dbPath = join(convDir, "conv-ag-1.db");
    await execFileAsync("sqlite3", [
      dbPath,
      `
        CREATE TABLE steps (idx integer primary key, step_type integer, status integer);
        CREATE TABLE gen_metadata (idx integer primary key, data blob, size integer);
        CREATE TABLE trajectory_metadata_blob (id text primary key, data blob);
        INSERT INTO steps VALUES (0, 14, 2), (1, 15, 2), (2, 132, 1);
        INSERT INTO gen_metadata VALUES (1, X'${blob.toString("hex")}', ${blob.length});
        INSERT INTO trajectory_metadata_blob VALUES ('main', X'${wsBlob.toString("hex")}');
      `
    ]);

    const nowMs = Date.now();
    const trPath = join(logDir, "transcript.jsonl");
    await writeFile(
      trPath,
      [
        JSON.stringify({ type: "USER_INPUT", status: "DONE", created_at: new Date(nowMs - 5000).toISOString() }),
        JSON.stringify({
          type: "PLANNER_RESPONSE",
          status: "DONE",
          created_at: new Date(nowMs - 2000).toISOString(),
          tool_calls: [{ name: "run_command" }]
        })
      ].join("\n") + "\n"
    );
    await utimes(dbPath, new Date(nowMs - 2000), new Date(nowMs - 2000));
    await utimes(trPath, new Date(nowMs - 2000), new Date(nowMs - 2000));

    const conv = await readConversation(dbPath);
    assert.equal(conv.workspace, "/Users/cui/Documents/Projects/RLCD_firm1");
    assert.equal(conv.generations.totals.input, 2500);

    const rt = await loadAntigravityRealtimeState({ geminiDir, sourcePrefix: "mac", now: nowMs });
    assert.equal(rt.source, "mac-antigravity");
    assert.equal(rt.status, "tool-use");
    assert.equal(rt.tool, "run_command");
    assert.equal(rt.workspace, "/Users/cui/Documents/Projects/RLCD_firm1");
    assert.deepEqual(rt.context, { used: 64200, window: 256000, current: 64200, max: 256000 });

    const todayTok = await antigravityTokensToday({ geminiDir, now: nowMs });
    assert.equal(todayTok, 2500 + 300 + 400);

    const autoPicked = pickAutoCurrent(
      { source: "mac-codex", status: "completed", time: new Date(nowMs - 30_000).toISOString() },
      [rt]
    );
    assert.equal(autoPicked.source, "mac-antigravity");
    assert.equal(autoPicked.status, "tool-use");

    const port = 17820 + (process.pid % 150);
    server = spawn(process.execPath, ["./src/bridge-server.js"], {
      cwd: process.cwd(),
      env: {
        ...process.env,
        PET_BRIDGE_PORT: String(port),
        PET_BRIDGE_LOG: join(tempRoot, "events.jsonl"),
        PET_BRIDGE_STATE: join(tempRoot, "bridge-state.json"),
        PET_BRIDGE_CLAUDE_PROJECTS: join(tempRoot, "claude-projects"),
        PET_BRIDGE_ANTIGRAVITY_HOME: geminiDir,
        PET_BRIDGE_ANTIGRAVITY_QUOTA_INTERVAL_MS: "0",
        PET_BRIDGE_CODEX_STATE: join(tempRoot, "missing.sqlite"),
        PET_BRIDGE_USAGE_CACHE_MS: "0"
      },
      stdio: ["ignore", "pipe", "pipe"]
    });

    for (let i = 0; i < 50; i++) {
      try {
        const res = await fetch(`http://127.0.0.1:${port}/health`);
        if (res.ok) break;
      } catch {
        await new Promise((r) => setTimeout(r, 100));
      }
    }

    const poll1 = await (await fetch(`http://127.0.0.1:${port}/esp32/poll`)).json();
    assert.equal(poll1.ok, true);
    assert.equal(poll1.current_status, "tool-use");
    assert.equal(poll1.usage.today, "3.2k tok");
    assert.equal(poll1.usage.context, "64.2k / 256k");
    assert.equal(poll1.usage.quota, "Gemini 4 Argon");

    const hookRes = await fetch(`http://127.0.0.1:${port}/hook/PreToolUse`, {
      method: "POST",
      headers: { "content-type": "application/json" },
      body: JSON.stringify({
        conversationId: "conv-ag-1",
        workspacePaths: ["/Users/cui/Documents/Projects/RLCD_firm1"],
        modelName: "Gemini 4 Argon (Medium)",
        toolCall: { name: "grep_search" }
      })
    });
    assert.equal(hookRes.status, 202);

    const statusBody = await (await fetch(`http://127.0.0.1:${port}/status`)).json();
    assert.equal(statusBody.realtime.antigravity.status, "searching");
    assert.equal(statusBody.realtime.antigravity.tool, "grep_search");
    assert.equal(statusBody.realtime.antigravity.project, "RLCD_firm1");
  } finally {
    server?.kill();
    await rm(tempRoot, { recursive: true, force: true });
  }
});
