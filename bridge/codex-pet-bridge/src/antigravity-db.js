// Read-only probe for Google Antigravity's local conversation databases
// and transcripts. Zero npm dependencies: SQLite access goes through
// /usr/bin/sqlite3 (-readonly), and protobuf blobs inside gen_metadata /
// trajectory_metadata_blob are decoded with a minimal wire-type walker.
// This module never writes to ~/.gemini.

import { execFile } from "node:child_process";
import { existsSync, readdirSync, statSync } from "node:fs";
import { open } from "node:fs/promises";
import { homedir } from "node:os";
import path from "node:path";

import {
  TERMINAL_STATUSES,
  TOOL_STATUSES,
  effectiveStatus,
  inferAntigravityRealtimeFromTranscriptLines,
  parseQuotaDocument,
  quotaSummary,
  str
} from "./antigravity-translate.js";

const SQLITE3 = existsSync("/usr/bin/sqlite3") ? "/usr/bin/sqlite3" : "sqlite3";
const SQLITE_FLAGS = ["-readonly", "-json", "-cmd", ".timeout 500"];
const EXEC_OPTS = { timeout: 15000, maxBuffer: 128 * 1024 * 1024 };
const TAIL_BYTES = 96 * 1024;

export const DEFAULT_ANTIGRAVITY_GEMINI_DIR = path.join(homedir(), ".gemini");

// ---------------------------------------------------------------------------
// sqlite3 CLI helper
// ---------------------------------------------------------------------------

// A WAL-mode database cannot be opened -readonly unless its -shm sidecar already
// exists (SQLite would have to create one, which read-only forbids), so a
// checkpointed, closed conversation fails with CANTOPEN. Missing sidecars also
// mean no process has the database open, which makes reading the main file as
// immutable both safe and the only option that leaves ~/.gemini untouched.
// While Antigravity holds the database open the sidecars exist and the plain
// -readonly open sees the WAL frames.
function openTarget(dbPath) {
  if (existsSync(`${dbPath}-shm`) || existsSync(`${dbPath}-wal`)) return dbPath;
  return `file:${dbPath.split("/").map(encodeURIComponent).join("/")}?immutable=1`;
}

/**
 * Run one SQL statement against a DB file via the sqlite3 CLI in readonly mode.
 * Resolves with the parsed JSON array of rows, [] for an empty result set,
 * or null when the DB is missing / locked / corrupt (or output is not JSON).
 */
function sqliteQuery(dbPath, sql) {
  return new Promise((resolve) => {
    execFile(SQLITE3, [...SQLITE_FLAGS, openTarget(dbPath), sql], EXEC_OPTS, (err, stdout) => {
      if (err) return resolve(null);
      const text = String(stdout ?? "").trim();
      if (text === "") return resolve([]);
      try {
        const rows = JSON.parse(text);
        resolve(Array.isArray(rows) ? rows : null);
      } catch {
        resolve(null);
      }
    });
  });
}

// ---------------------------------------------------------------------------
// Roots and filesystem listings
// ---------------------------------------------------------------------------

function rootDir(root) {
  return typeof root === "string" ? root : root.dir;
}

function rootProduct(root) {
  if (typeof root === "string") return path.basename(root);
  return root.product ?? null;
}

/** Known Antigravity install roots under ~/.gemini that actually exist. */
export function defaultRoots(geminiBase = DEFAULT_ANTIGRAVITY_GEMINI_DIR) {
  const candidates = [
    { dir: path.join(geminiBase, "antigravity-cli"), product: "cli" },
    { dir: path.join(geminiBase, "antigravity"), product: "app" },
    { dir: path.join(geminiBase, "antigravity-ide"), product: "ide" }
  ];
  return candidates.filter((r) => {
    try {
      return statSync(r.dir).isDirectory();
    } catch {
      return false;
    }
  });
}

function transcriptPathFor(root, conversationId) {
  return path.join(
    rootDir(root),
    "brain",
    conversationId,
    ".system_generated",
    "logs",
    "transcript.jsonl"
  );
}

/** All conversations across the given roots, newest mtime first. */
export function listConversations(roots = defaultRoots()) {
  const out = [];
  for (const root of roots) {
    const dir = path.join(rootDir(root), "conversations");
    let names;
    try {
      names = readdirSync(dir);
    } catch {
      continue;
    }
    for (const name of names) {
      if (!name.endsWith(".db")) continue;
      const dbPath = path.join(dir, name);
      let st;
      try {
        st = statSync(dbPath);
      } catch {
        continue;
      }
      if (!st.isFile()) continue;
      const conversationId = name.slice(0, -3);
      let mtimeMs = st.mtimeMs;
      try {
        const walSt = statSync(`${dbPath}-wal`);
        if (walSt.isFile() && walSt.mtimeMs > mtimeMs) {
          mtimeMs = walSt.mtimeMs;
        }
      } catch {
        // no WAL sidecar
      }
      try {
        const trSt = statSync(transcriptPathFor(root, conversationId));
        if (trSt.isFile() && trSt.mtimeMs > mtimeMs) {
          mtimeMs = trSt.mtimeMs;
        }
      } catch {
        // no transcript file
      }
      out.push({
        root: rootDir(root),
        product: rootProduct(root),
        dbPath,
        conversationId,
        mtimeMs,
        sizeBytes: st.size
      });
    }
  }
  out.sort((a, b) => b.mtimeMs - a.mtimeMs);
  return out;
}

/** Live presence locks across the given roots. */
export function presenceLocks(roots = defaultRoots()) {
  const out = [];
  for (const root of roots) {
    const dir = path.join(rootDir(root), "presence");
    let names;
    try {
      names = readdirSync(dir);
    } catch {
      continue;
    }
    for (const name of names) {
      if (!name.endsWith(".lock")) continue;
      out.push({ root: rootDir(root), conversationId: name.slice(0, -".lock".length) });
    }
  }
  return out;
}

// ---------------------------------------------------------------------------
// Minimal protobuf wire-format walker (tolerant, never throws)
// ---------------------------------------------------------------------------

const WIRE_VARINT = 0;
const WIRE_I64 = 1;
const WIRE_LEN = 2;
const WIRE_I32 = 5;

const MAX_SAFE_BI = BigInt(Number.MAX_SAFE_INTEGER);
const TWO_POW_64 = 1n << 64n;

/** Read one base-128 varint; returns { value: BigInt, pos } or null if malformed. */
function readVarint(buf, pos) {
  let value = 0n;
  let shift = 0n;
  for (let i = 0; i < 10; i++) {
    if (pos >= buf.length) return null;
    const byte = buf[pos++];
    value |= BigInt(byte & 0x7f) << shift;
    if ((byte & 0x80) === 0) return { value, pos };
    shift += 7n;
  }
  return null;
}

/**
 * Walk every field of a protobuf message buffer.
 * Returns [{ no, wire, value }] where value is BigInt (varint) or Buffer,
 * or null when the buffer is malformed.
 */
function walkFields(buf) {
  const fields = [];
  let pos = 0;
  while (pos < buf.length) {
    const key = readVarint(buf, pos);
    if (!key) return null;
    pos = key.pos;
    const no = Number(key.value >> 3n);
    const wire = Number(key.value & 7n);
    if (no === 0) return null;
    if (wire === WIRE_VARINT) {
      const v = readVarint(buf, pos);
      if (!v) return null;
      pos = v.pos;
      fields.push({ no, wire, value: v.value });
    } else if (wire === WIRE_I64) {
      if (pos + 8 > buf.length) return null;
      fields.push({ no, wire, value: buf.subarray(pos, pos + 8) });
      pos += 8;
    } else if (wire === WIRE_LEN) {
      const len = readVarint(buf, pos);
      if (!len) return null;
      pos = len.pos;
      const n = Number(len.value);
      if (!Number.isSafeInteger(n) || n < 0 || pos + n > buf.length) return null;
      fields.push({ no, wire, value: buf.subarray(pos, pos + n) });
      pos += n;
    } else if (wire === WIRE_I32) {
      if (pos + 4 > buf.length) return null;
      fields.push({ no, wire, value: buf.subarray(pos, pos + 4) });
      pos += 4;
    } else {
      return null;
    }
  }
  return fields;
}

function toSafeNumber(v) {
  if (v <= MAX_SAFE_BI) return Number(v);
  const signed = v - TWO_POW_64;
  return signed >= -MAX_SAFE_BI ? Number(signed) : null;
}

function lastLenField(fields, no) {
  let out = null;
  for (const f of fields) if (f.no === no && f.wire === WIRE_LEN) out = f.value;
  return out;
}

function lastString(fields, no) {
  const buf = lastLenField(fields, no);
  return buf === null ? null : buf.toString("utf8");
}

function lastVarint(fields, no) {
  let out = null;
  for (const f of fields) if (f.no === no && f.wire === WIRE_VARINT) out = f.value;
  return out;
}

function parseUsage(buf) {
  const fields = walkFields(buf);
  if (!fields) return null;
  const usage = { input: 0, cacheRead: 0, thinking: 0, output: 0, totalOutput: 0 };
  const byNumber = { 2: "input", 3: "totalOutput", 5: "cacheRead", 9: "thinking", 10: "output" };
  let modelEnum = null;
  for (const f of fields) {
    if (f.wire !== WIRE_VARINT) continue;
    if (f.no === 1) {
      modelEnum = toSafeNumber(f.value);
      continue;
    }
    const key = byNumber[f.no];
    if (key) usage[key] = toSafeNumber(f.value);
  }
  return { modelEnum, usage };
}

function parseContext(buf) {
  const fields = walkFields(buf);
  if (!fields) return null;
  let current = null;
  let max = null;
  for (const f of fields) {
    if (f.wire !== WIRE_VARINT) continue;
    if (f.no === 1) current = toSafeNumber(f.value);
    else if (f.no === 4) max = toSafeNumber(f.value);
  }
  if (current === null && max === null) return null;
  return { current, max };
}

/**
 * Decode one gen_metadata.data blob into a plain summary object.
 * Tolerant: unknown fields are skipped; malformed input returns null, never throws.
 */
export function decodeGenerationMeta(buffer) {
  if (!Buffer.isBuffer(buffer)) return null;
  const top = walkFields(buffer);
  if (!top) return null;
  const gen = lastLenField(top, 1);
  if (!gen) return null;
  const genFields = walkFields(gen);
  if (!genFields) return null;

  const meta = {
    modelEnum: null,
    modelId: null,
    modelDisplay: null,
    usage: { input: 0, cacheRead: 0, thinking: 0, output: 0, totalOutput: 0 },
    context: null
  };

  const usageMsg = lastLenField(genFields, 4);
  if (usageMsg) {
    const u = parseUsage(usageMsg);
    if (u) {
      meta.modelEnum = u.modelEnum;
      meta.usage = u.usage;
    }
  }

  const holder = lastLenField(genFields, 9);
  if (holder) {
    const holderFields = walkFields(holder);
    if (holderFields) {
      const ctxMsg = lastLenField(holderFields, 10);
      if (ctxMsg) meta.context = parseContext(ctxMsg);
      void toSafeNumber(lastVarint(holderFields, 2));
    }
  }

  meta.modelId = lastString(genFields, 19);
  meta.modelDisplay = lastString(genFields, 21);
  return meta;
}

/** Extract the first file:/// workspace URI stored in trajectory_metadata_blob. */
export function decodeWorkspaceMeta(buffer, depth = 0) {
  if (!Buffer.isBuffer(buffer) || depth > 5) return "";
  const fields = walkFields(buffer);
  if (!fields) return "";
  for (const f of fields) {
    if (f.wire !== WIRE_LEN) continue;
    const text = f.value.toString("utf8");
    if (text.startsWith("file:///") && !text.includes("\u0000")) {
      try {
        return decodeURIComponent(text.slice("file://".length));
      } catch {
        return text.slice("file://".length);
      }
    }
    const nested = decodeWorkspaceMeta(f.value, depth + 1);
    if (nested) return nested;
  }
  return "";
}

export const STEP_TYPE_LABELS = { 14: "user_input", 15: "generation", 132: "tool_call" };

// ---------------------------------------------------------------------------
// Conversation reading
// ---------------------------------------------------------------------------

/**
 * Read one conversation DB (read-only). Resolves null when the DB is missing,
 * locked or corrupt. Rows whose blob fails to decode are skipped and counted
 * in generations.skipped.
 */
export async function readConversation(dbPath) {
  const stepRows = await sqliteQuery(
    dbPath,
    "SELECT idx, step_type, status FROM steps ORDER BY idx"
  );
  if (stepRows === null) return null;
  const metaRows = await sqliteQuery(
    dbPath,
    "SELECT idx, hex(data) AS data_hex FROM gen_metadata ORDER BY idx"
  );
  if (metaRows === null) return null;

  const counts = {};
  let lastStep = null;
  for (const row of stepRows) {
    const idx = Number(row.idx);
    const stepType = Number(row.step_type);
    const status = row.status == null ? null : Number(row.status);
    const key = String(stepType);
    counts[key] = (counts[key] ?? 0) + 1;
    lastStep = { idx, stepType, status };
  }

  const totals = { input: 0, cacheRead: 0, thinking: 0, output: 0, totalOutput: 0 };
  let count = 0;
  let skipped = 0;
  let lastGen = null;
  for (const row of metaRows) {
    if (row.data_hex == null) {
      skipped++;
      continue;
    }
    const buf = Buffer.from(String(row.data_hex), "hex");
    const meta = decodeGenerationMeta(buf);
    if (!meta) {
      skipped++;
      continue;
    }
    count++;
    lastGen = meta;
    for (const key of Object.keys(totals)) {
      const v = meta.usage[key];
      totals[key] += typeof v === "number" && Number.isSafeInteger(v) && v > 0 ? v : 0;
    }
  }

  let workspace = "";
  const trajRows = await sqliteQuery(
    dbPath,
    "SELECT hex(data) AS data_hex FROM trajectory_metadata_blob LIMIT 1"
  );
  if (trajRows?.[0]?.data_hex) {
    workspace = decodeWorkspaceMeta(Buffer.from(String(trajRows[0].data_hex), "hex"));
  }

  return {
    steps: { total: stepRows.length, counts, last: lastStep },
    generations: { count, skipped, last: lastGen, totals },
    workspace
  };
}

export async function readSnapshot({ roots, geminiDir } = {}) {
  const rs = roots ?? defaultRoots(geminiDir);
  const conversations = listConversations(rs);
  const locks = presenceLocks(rs);
  const active = conversations[0] ?? null;
  const activeDetail = active ? await readConversation(active.dbPath) : null;
  return { roots: rs, conversations, locks, active, activeDetail };
}

// ---------------------------------------------------------------------------
// Realtime state + Today tokens + Quota helper for codex-pet-bridge
// ---------------------------------------------------------------------------

async function readTailLines(filePath, maxBytes = TAIL_BYTES) {
  let handle;
  try {
    handle = await open(filePath, "r");
    const info = await handle.stat();
    const start = Math.max(0, info.size - maxBytes);
    const length = info.size - start;
    if (length <= 0) return [];
    const buffer = Buffer.allocUnsafe(length);
    const { bytesRead } = await handle.read(buffer, 0, length, start);
    const text = buffer.subarray(0, bytesRead).toString("utf8");
    const lines = text.split("\n");
    return start > 0 ? lines.slice(1) : lines;
  } catch {
    return [];
  } finally {
    await handle?.close();
  }
}

function ageOutRealtimeStatus(status, ageMs, windows = {}) {
  const activeWindowMs = windows.activeWindowMs ?? 180_000;
  const toolWindowMs = windows.toolWindowMs ?? 900_000;
  const completedHoldMs = windows.completedHoldMs ?? 120_000;
  if (!Number.isFinite(ageMs) || ageMs < 0) return null;
  if (TERMINAL_STATUSES.has(status)) {
    return ageMs <= completedHoldMs ? status : null;
  }
  const limit = TOOL_STATUSES.has(status) || status === "needs-attention" ? toolWindowMs : activeWindowMs;
  return ageMs <= limit ? status : null;
}

function inferStepFallbackStatus(lastStep, mtimeMs) {
  if (!lastStep) return null;
  const time = new Date(mtimeMs).toISOString();
  if (lastStep.stepType === 14) return { status: "thinking", tool: "", time };
  if (lastStep.stepType === 132) {
    return { status: lastStep.status === 1 ? "tool-use" : "thinking", tool: "", time };
  }
  if (lastStep.stepType === 15) {
    if (lastStep.status === 1) return { status: "thinking", tool: "", time };
    if (lastStep.status === 3) return { status: "failed", tool: "", time };
    return { status: "completed", tool: "", time };
  }
  return null;
}

export async function loadAntigravityRealtimeState({
  geminiDir = DEFAULT_ANTIGRAVITY_GEMINI_DIR,
  roots,
  sourcePrefix = "local",
  hookEvent = null,
  hookAt = 0,
  now = Date.now(),
  activeWindowMs = 180_000,
  toolWindowMs = 900_000,
  completedHoldMs = 120_000
} = {}) {
  const windows = { activeWindowMs, toolWindowMs, completedHoldMs };
  const rs = roots ?? defaultRoots(geminiDir);
  const conversations = listConversations(rs);
  const active = conversations[0] ?? null;
  const maxWindow = Math.max(activeWindowMs, toolWindowMs, completedHoldMs);
  const activeFresh = active && now - active.mtimeMs <= maxWindow;
  const activeDetail = active && (activeFresh || hookEvent) ? await readConversation(active.dbPath) : null;
  const generation = activeDetail?.generations?.last ?? null;

  let hookCandidate = null;
  if (hookEvent && hookAt > 0) {
    const eff = effectiveStatus(hookEvent, hookAt, now, windows);
    if (eff.status && eff.status !== "idle") {
      hookCandidate = {
        status: eff.status,
        tool: eff.tool || hookEvent.tool || "",
        time: hookEvent.time || new Date(hookAt).toISOString(),
        timeMs: Date.parse(hookEvent.time || "") || hookAt,
        sessionId: hookEvent.sessionId || active?.conversationId || "",
        workspace: hookEvent.workspace || activeDetail?.workspace || "",
        model: hookEvent.model && hookEvent.model !== "auto" ? hookEvent.model : ""
      };
    }
  }

  let transcriptCandidate = null;
  if (active && activeFresh) {
    const trPath = transcriptPathFor(active.root, active.conversationId);
    const lines = await readTailLines(trPath);
    const parsed = lines.length
      ? inferAntigravityRealtimeFromTranscriptLines(lines)
      : inferStepFallbackStatus(activeDetail?.steps?.last, active.mtimeMs);
    if (parsed?.status) {
      const parsedMs = Date.parse(parsed.time || "") || active.mtimeMs;
      const effectiveTimeMs = Math.max(parsedMs, active.mtimeMs);
      const status = ageOutRealtimeStatus(parsed.status, now - effectiveTimeMs, windows);
      if (status) {
        transcriptCandidate = {
          status,
          tool: parsed.tool || "",
          time: new Date(effectiveTimeMs).toISOString(),
          timeMs: effectiveTimeMs,
          sessionId: active.conversationId,
          workspace: activeDetail?.workspace || "",
          model: ""
        };
      }
    }
  }

  let chosen = null;
  if (hookCandidate && transcriptCandidate) {
    chosen = hookCandidate.timeMs >= transcriptCandidate.timeMs ? hookCandidate : transcriptCandidate;
  } else {
    chosen = hookCandidate || transcriptCandidate;
  }

  if (!chosen) return null;

  const model =
    generation?.modelDisplay ||
    generation?.modelId ||
    chosen.model ||
    (hookEvent?.model && hookEvent.model !== "auto" ? hookEvent.model : "") ||
    "";
  const ctx = generation?.context;
  const context =
    ctx && Number.isFinite(ctx.current) && Number.isFinite(ctx.max) && ctx.max > 0
      ? { used: ctx.current, window: ctx.max, current: ctx.current, max: ctx.max }
      : null;
  const workspace = chosen.workspace || activeDetail?.workspace || hookEvent?.workspace || "";
  const sessionId = chosen.sessionId || active?.conversationId || hookEvent?.sessionId || "";

  return {
    source: `${sourcePrefix}-antigravity`,
    task: `${sourcePrefix}-antigravity-session`,
    status: chosen.status,
    time: chosen.time,
    sessionId,
    workspace,
    tool: chosen.tool || "",
    model,
    modelId: generation?.modelId || "",
    context,
    sessionTokens: activeDetail?.generations?.totals || null
  };
}

export async function antigravityTokensToday({
  geminiDir = DEFAULT_ANTIGRAVITY_GEMINI_DIR,
  roots,
  now = Date.now()
} = {}) {
  const rs = roots ?? defaultRoots(geminiDir);
  const conversations = listConversations(rs);
  if (!conversations.length) return 0;

  const dayStart = new Date(now);
  dayStart.setHours(0, 0, 0, 0);
  const startMs = dayStart.getTime();

  const todayConvs = conversations.filter((item) => item.mtimeMs >= startMs).slice(0, 12);
  const targets = todayConvs.length ? todayConvs : [conversations[0]];

  let total = 0;
  for (const conv of targets) {
    const detail = await readConversation(conv.dbPath);
    const t = detail?.generations?.totals;
    if (t) {
      total += (t.input || 0) + (t.thinking || 0) + (t.output || 0);
    }
  }
  return total;
}

export function resolveAgyBinary(customBin = "") {
  const configured = str(customBin || process.env.PET_ANTIGRAVITY_QUOTA_CMD || process.env.PROBE_QUOTA_CMD);
  if (configured && configured !== "agy") return configured;
  const home = homedir();
  const candidates = [
    path.join(home, ".local", "bin", "agy"),
    path.join(home, ".antigravity", "antigravity", "bin", "agy"),
    "/opt/homebrew/bin/agy",
    "/usr/local/bin/agy",
    "/Applications/Antigravity.app/Contents/Resources/app/bin/agy"
  ];
  for (const candidate of candidates) {
    if (existsSync(candidate)) return candidate;
  }
  return configured || "agy";
}

export function fetchAntigravityQuota({ agyBin = "", timeoutMs = 45_000, preferredModel = "" } = {}) {
  const bin = resolveAgyBinary(agyBin);
  const home = homedir();
  const extraPaths = [
    path.dirname(bin),
    path.join(home, ".local", "bin"),
    "/opt/homebrew/bin",
    "/usr/local/bin"
  ].filter((p) => p && p !== ".");
  const env = {
    ...process.env,
    PET_ANTIGRAVITY_INTERNAL: "1",
    PATH: [...extraPaths, process.env.PATH || "/usr/bin:/bin"].join(":")
  };

  return new Promise((resolve) => {
    execFile(
      bin,
      ["-p", "/usage", "--output-format", "json"],
      { timeout: timeoutMs, maxBuffer: 2 * 1024 * 1024, env },
      (error, stdout) => {
        const at = new Date().toISOString();
        try {
          if (error) throw new Error(error.message.split("\n")[0]);
          const doc = JSON.parse(String(stdout || "{}"));
          const groups = parseQuotaDocument(doc);
          const summary = quotaSummary(groups, preferredModel);
          resolve({
            ok: true,
            at,
            groups,
            summary,
            conversationId: str(doc?.conversation_id),
            error: null
          });
        } catch (failure) {
          resolve({
            ok: false,
            at,
            groups: null,
            summary: "",
            conversationId: "",
            error: failure.message || String(failure)
          });
        }
      }
    );
  });
}
