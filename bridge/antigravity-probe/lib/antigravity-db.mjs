// Read-only probe for Google Antigravity's local conversation databases.
// Zero npm dependencies: the SQLite access goes through the sqlite3 CLI
// (/usr/bin/sqlite3, opened with -readonly), and the protobuf blobs inside
// gen_metadata are decoded with a minimal hand-rolled varint/wire-type walker.
// This module never writes to ~/.gemini.

import { execFile } from 'node:child_process';
import { existsSync, readdirSync, statSync } from 'node:fs';
import { homedir } from 'node:os';
import path from 'node:path';

const SQLITE3 = '/usr/bin/sqlite3';
const SQLITE_FLAGS = ['-readonly', '-json', '-cmd', '.timeout 500'];
const EXEC_OPTS = { timeout: 15000, maxBuffer: 128 * 1024 * 1024 };

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
  return `file:${dbPath.split('/').map(encodeURIComponent).join('/')}?immutable=1`;
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
      const text = String(stdout ?? '').trim();
      if (text === '') return resolve([]);
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
  return typeof root === 'string' ? root : root.dir;
}

function rootProduct(root) {
  if (typeof root === 'string') return path.basename(root);
  return root.product ?? null;
}

/** Known Antigravity install roots under ~/.gemini that actually exist. */
export function defaultRoots() {
  const base = path.join(homedir(), '.gemini');
  const candidates = [
    { dir: path.join(base, 'antigravity-cli'), product: 'cli' },
    { dir: path.join(base, 'antigravity'), product: 'app' },
    { dir: path.join(base, 'antigravity-ide'), product: 'ide' },
  ];
  return candidates.filter((r) => {
    try {
      return statSync(r.dir).isDirectory();
    } catch {
      return false;
    }
  });
}

/** All conversations across the given roots, newest mtime first. */
export function listConversations(roots = defaultRoots()) {
  const out = [];
  for (const root of roots) {
    const dir = path.join(rootDir(root), 'conversations');
    let names;
    try {
      names = readdirSync(dir);
    } catch {
      continue;
    }
    for (const name of names) {
      if (!name.endsWith('.db')) continue;
      const dbPath = path.join(dir, name);
      let st;
      try {
        st = statSync(dbPath);
      } catch {
        continue;
      }
      if (!st.isFile()) continue;
      out.push({
        root: rootDir(root),
        product: rootProduct(root),
        dbPath,
        conversationId: name.slice(0, -3),
        mtimeMs: st.mtimeMs,
        sizeBytes: st.size,
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
    const dir = path.join(rootDir(root), 'presence');
    let names;
    try {
      names = readdirSync(dir);
    } catch {
      continue;
    }
    for (const name of names) {
      if (!name.endsWith('.lock')) continue;
      out.push({ root: rootDir(root), conversationId: name.slice(0, -'.lock'.length) });
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
  return null; // varints longer than 10 bytes are malformed (beyond uint64)
}

/**
 * Walk every field of a protobuf message buffer.
 * Returns [{ no, wire, value }] where value is BigInt (varint) or Buffer,
 * or null when the buffer is malformed. Unknown field numbers are kept
 * (callers pick what they need, everything else is skipped implicitly);
 * unsupported wire types (groups 3/4, unknown) mark the buffer malformed.
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
    if (no === 0) return null; // field number 0 is invalid
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

/**
 * Unsigned 64-bit varint -> JS number, safely. Values that fit in
 * Number.MAX_SAFE_INTEGER pass through; anything larger is re-interpreted as
 * a signed int64 (so the uint64 encoding of -1, 18446744073709551615, becomes
 * -1); anything still out of safe range becomes null.
 */
function toSafeNumber(v) {
  if (v <= MAX_SAFE_BI) return Number(v);
  const signed = v - TWO_POW_64;
  return signed >= -MAX_SAFE_BI ? Number(signed) : null;
}

function lastLenField(fields, no) {
  let out = null;
  for (const f of fields) if (f.no === no && f.wire === WIRE_LEN) out = f.value;
  return out; // protobuf semantics: last occurrence wins
}

function lastString(fields, no) {
  const buf = lastLenField(fields, no);
  return buf === null ? null : buf.toString('utf8');
}

/** Parse the usage submessage (gen.f4): f1 modelEnum, f2 input, f3 totalOutput,
 *  f5 cacheRead (may be absent -> 0), f9 thinking, f10 visible output. */
function parseUsage(buf) {
  const fields = walkFields(buf);
  if (!fields) return null;
  const usage = { input: 0, cacheRead: 0, thinking: 0, output: 0, totalOutput: 0 };
  const byNumber = { 2: 'input', 3: 'totalOutput', 5: 'cacheRead', 9: 'thinking', 10: 'output' };
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

/** Parse the context submessage (gen.f9.f10): f1 current, f4 max. */
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
 * Tolerant: unknown fields are skipped; malformed input returns null, never
 * throws.
 *
 * Layout (internal/undocumented field numbers, verified against a real DB):
 *   top.f1  = generation message
 *     gen.f4  = usage message  (see parseUsage)
 *     gen.f9  = holder message { f2: varint (may be uint64 -1 sentinel),
 *                               f10: context message { f1 current, f4 max } }
 *     gen.f17 = duplicate usage data under its sub-field 2 -> ignored
 *     gen.f19 = model id string (e.g. "gemini-3.8-flash-n")
 *     gen.f21 = model display string (e.g. "Gemini 3.8 Flash (High)")
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
    context: null,
  };

  const usageMsg = lastLenField(genFields, 4); // prefer f4 over the f17 duplicate
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
      // holder.f2 may be the uint64 encoding of -1 (18446744073709551615);
      // exercised here so malformed 64-bit values can never escape, but the
      // value itself is not surfaced.
      void toSafeNumber(lastVarint(holderFields, 2));
    }
  }

  meta.modelId = lastString(genFields, 19);
  meta.modelDisplay = lastString(genFields, 21);
  return meta;
}

function lastVarint(fields, no) {
  let out = null;
  for (const f of fields) if (f.no === no && f.wire === WIRE_VARINT) out = f.value;
  return out;
}

// ---------------------------------------------------------------------------
// Step type labels (HEURISTIC: numbers are internal to Antigravity and were
// observed in one real DB; they may change between app versions)
// ---------------------------------------------------------------------------

export const STEP_TYPE_LABELS = { 14: 'user_input', 15: 'generation', 132: 'tool_call' };

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
    'SELECT idx, step_type, status FROM steps ORDER BY idx'
  );
  if (stepRows === null) return null;
  const metaRows = await sqliteQuery(
    dbPath,
    'SELECT idx, hex(data) AS data_hex FROM gen_metadata ORDER BY idx'
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
    const buf = Buffer.from(String(row.data_hex), 'hex');
    const meta = decodeGenerationMeta(buf);
    if (!meta) {
      skipped++;
      continue;
    }
    count++;
    lastGen = meta;
    for (const key of Object.keys(totals)) {
      const v = meta.usage[key];
      totals[key] += typeof v === 'number' && Number.isSafeInteger(v) && v > 0 ? v : 0;
    }
  }

  return {
    steps: { total: stepRows.length, counts, last: lastStep },
    generations: { count, skipped, last: lastGen, totals },
  };
}

/**
 * Snapshot of the whole Antigravity install: conversations, presence locks,
 * the active conversation (newest by mtime) and its detail (readConversation
 * result, or null). Presence locks are reported but do not drive the choice:
 * the CLI leaves a conversation's .lock behind after it exits, so a lock only
 * proves a conversation once existed, not that it is open now.
 */
export async function readSnapshot({ roots } = {}) {
  const rs = roots ?? defaultRoots();
  const conversations = listConversations(rs);
  const locks = presenceLocks(rs);
  const active = conversations[0] ?? null;
  const activeDetail = active ? await readConversation(active.dbPath) : null;
  return { roots: rs, conversations, locks, active, activeDetail };
}
