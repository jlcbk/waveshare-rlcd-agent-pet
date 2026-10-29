// Plain Node test script for lib/antigravity-db.mjs — no framework.
// Run: node test/db.test.mjs   (exit code non-zero on failure)
//
// Part 1 is pure unit testing against a SYNTHETIC protobuf buffer encoded by a
// tiny helper below. Part 2 integrates read-only against the real Antigravity
// DB when it exists on this machine and SKIPs otherwise.

import { existsSync } from 'node:fs';
import { homedir } from 'node:os';
import path from 'node:path';
import {
  STEP_TYPE_LABELS,
  decodeGenerationMeta,
  defaultRoots,
  listConversations,
  presenceLocks,
  readConversation,
  readSnapshot,
} from '../lib/antigravity-db.mjs';

// --- tiny test harness ------------------------------------------------------

let failures = 0;
function check(name, cond, extra = '') {
  if (cond) {
    console.log(`ok - ${name}`);
  } else {
    failures++;
    console.error(`FAIL - ${name}${extra ? ` :: ${extra}` : ''}`);
  }
}

function deepEqual(a, b) {
  if (a === b) return true;
  if (a === null || b === null || typeof a !== 'object' || typeof b !== 'object') return false;
  if (Array.isArray(a) !== Array.isArray(b)) return false;
  const ka = Object.keys(a);
  const kb = Object.keys(b);
  if (ka.length !== kb.length) return false;
  return ka.every((k) => deepEqual(a[k], b[k]));
}

// --- tiny protobuf encoder (test-only helper) --------------------------------

function pbVarint(value) {
  let v = BigInt(value);
  const out = [];
  do {
    let byte = Number(v & 0x7fn);
    v >>= 7n;
    if (v > 0n) byte |= 0x80;
    out.push(byte);
  } while (v > 0n);
  return Buffer.from(out);
}

const pbTag = (fieldNo, wire) => pbVarint((BigInt(fieldNo) << 3n) | BigInt(wire));
const pbLen = (fieldNo, payload) => Buffer.concat([pbTag(fieldNo, 2), pbVarint(payload.length), payload]);
const pbInt = (fieldNo, value) => Buffer.concat([pbTag(fieldNo, 0), pbVarint(value)]);

// --- part 1: decodeGenerationMeta unit tests on a synthetic buffer -----------

const usageMsg = Buffer.concat([
  pbInt(1, 1318), // modelEnum
  pbInt(2, 100), // input
  pbInt(3, 10), // totalOutput
  pbInt(5, 20), // cacheRead
  pbInt(9, 7), // thinking
  pbInt(10, 3), // visible output
]);
const contextMsg = Buffer.concat([pbInt(1, 1234), pbInt(4, 256000)]);
const holderMsg = Buffer.concat([
  pbInt(2, 18446744073709551615n), // uint64 encoding of -1
  pbLen(10, contextMsg),
]);
const genMsg = Buffer.concat([
  pbLen(4, usageMsg),
  pbLen(9, holderMsg),
  pbLen(19, Buffer.from('gemini-x', 'utf8')),
  pbLen(21, Buffer.from('Gemini X', 'utf8')),
]);
const synthetic = pbLen(1, genMsg);

const meta = decodeGenerationMeta(synthetic);
check('synthetic: decodes to an object', meta !== null, JSON.stringify(meta));
check('synthetic: modelEnum === 1318', meta?.modelEnum === 1318, String(meta?.modelEnum));
check('synthetic: modelId === "gemini-x"', meta?.modelId === 'gemini-x', String(meta?.modelId));
check('synthetic: modelDisplay === "Gemini X"', meta?.modelDisplay === 'Gemini X', String(meta?.modelDisplay));
check(
  'synthetic: usage { input 100, cacheRead 20, thinking 7, output 3, totalOutput 10 }',
  deepEqual(meta?.usage, { input: 100, cacheRead: 20, thinking: 7, output: 3, totalOutput: 10 }),
  JSON.stringify(meta?.usage)
);
check(
  'synthetic: context { current 1234, max 256000 }',
  deepEqual(meta?.context, { current: 1234, max: 256000 }),
  JSON.stringify(meta?.context)
);
check(
  'malformed [0xff,0xff,0xff] returns null',
  decodeGenerationMeta(Buffer.from([0xff, 0xff, 0xff])) === null
);
let emptyThrew = false;
let emptyResult;
try {
  emptyResult = decodeGenerationMeta(Buffer.alloc(0));
} catch {
  emptyThrew = true;
}
check('empty buffer does not throw', !emptyThrew, `threw: ${emptyThrew}`);
check('empty buffer returns null (no gen message)', emptyResult === null, String(emptyResult));
check('non-buffer input returns null', decodeGenerationMeta('nope') === null);
check('STEP_TYPE_LABELS heuristic map', deepEqual(STEP_TYPE_LABELS, { 14: 'user_input', 15: 'generation', 132: 'tool_call' }));

// --- part 2: integration against the real DB (read-only) ---------------------

const REAL_DB = path.join(
  homedir(),
  '.gemini',
  'antigravity-cli',
  'conversations',
  '65633db5-4090-4aaa-9f52-8646e4a327b5.db'
);

if (existsSync(REAL_DB)) {
  const conv = await readConversation(REAL_DB);
  if (conv === null) {
    failures++;
    console.error('FAIL - real: readConversation resolved null (DB unreadable)');
  } else {
    check('real: generations.count === 11', conv.generations.count === 11, `got ${conv.generations.count}`);
    check('real: generations.skipped === 0', conv.generations.skipped === 0, `got ${conv.generations.skipped}`);
    check('real: steps.total === 22', conv.steps.total === 22, `got ${conv.steps.total}`);
    check(
      'real: step counts {14:3, 15:11, 132:8}',
      conv.steps.counts[14] === 3 && conv.steps.counts[15] === 11 && conv.steps.counts[132] === 8,
      JSON.stringify(conv.steps.counts)
    );
    check(
      'real: last generation modelDisplay === "Gemini 3.8 Flash (High)"',
      conv.generations.last?.modelDisplay === 'Gemini 3.8 Flash (High)',
      JSON.stringify(conv.generations.last?.modelDisplay)
    );
    check(
      'real: context.max === 256000',
      conv.generations.last?.context?.max === 256000,
      JSON.stringify(conv.generations.last?.context)
    );
    const numbers = [
      ...Object.values(conv.generations.last?.usage ?? {}),
      ...Object.values(conv.generations.totals),
    ];
    check(
      'real: all usage numbers are non-negative safe integers',
      numbers.every((n) => Number.isSafeInteger(n) && n >= 0),
      JSON.stringify(numbers)
    );
    console.log(
      `real totals (input=${conv.generations.totals.input}, cacheRead=${conv.generations.totals.cacheRead}, ` +
        `thinking=${conv.generations.totals.thinking}, output=${conv.generations.totals.output}, ` +
        `totalOutput=${conv.generations.totals.totalOutput})`
    );
  }
} else {
  console.log(`SKIP: real conversation DB not found at ${REAL_DB}`);
}

// --- part 3: roots / listings / snapshot --------------------------------------

const roots = defaultRoots();
console.log(`roots: ${roots.map((r) => r.product).join(', ') || '(none)'}`);
if (roots.length === 0) {
  console.log('SKIP: no Antigravity roots exist, skipping listing/snapshot checks');
} else {
  const convs = listConversations();
  check('listConversations returns >= 1 entry', convs.length >= 1, `got ${convs.length}`);
  const sortedDesc = convs.every((c, i) => i === 0 || convs[i - 1].mtimeMs >= c.mtimeMs);
  check('listConversations sorted newest mtime first', sortedDesc);
  check(
    'conversation entries have the expected shape',
    convs.length === 0 ||
      (typeof convs[0].root === 'string' &&
        typeof convs[0].product === 'string' &&
        typeof convs[0].dbPath === 'string' &&
        typeof convs[0].conversationId === 'string' &&
        typeof convs[0].mtimeMs === 'number' &&
        typeof convs[0].sizeBytes === 'number')
  );
  const locks = presenceLocks();
  check('presenceLocks returns an array', Array.isArray(locks));
  console.log(`listings: ${convs.length} conversation(s), ${locks.length} presence lock(s)`);

  let snapshot = null;
  let snapThrew = false;
  try {
    snapshot = await readSnapshot();
  } catch (e) {
    snapThrew = true;
    console.error(`readSnapshot threw: ${e?.stack ?? e}`);
  }
  check('readSnapshot resolves without throwing', !snapThrew && snapshot !== null);
  check(
    'readSnapshot shape',
    snapshot !== null &&
      Array.isArray(snapshot.roots) &&
      Array.isArray(snapshot.conversations) &&
      Array.isArray(snapshot.locks) &&
      (snapshot.active === null ||
        (snapshot.active && typeof snapshot.active.dbPath === 'string')) &&
      (snapshot.activeDetail === null || typeof snapshot.activeDetail === 'object')
  );
  if (snapshot?.active) {
    console.log(
      `active: ${snapshot.active.conversationId} (${snapshot.active.product}), ` +
        `generations=${snapshot.activeDetail?.generations?.count ?? 'n/a'}`
    );
  }
}

// --- every conversation must be readable, with or without WAL sidecars ----------
// A closed, checkpointed conversation has no -wal/-shm files and cannot be opened
// -readonly the normal way (SQLITE_CANTOPEN); the module falls back to an
// immutable URI for those. Both kinds should decode.

for (const conv of listConversations()) {
  const sidecars = existsSync(`${conv.dbPath}-shm`) || existsSync(`${conv.dbPath}-wal`);
  const detail = await readConversation(conv.dbPath);
  check(
    `readable ${sidecars ? 'with' : 'without'} sidecars: ${conv.conversationId.slice(0, 8)}`,
    detail !== null && Number.isSafeInteger(detail.steps.total) && Number.isSafeInteger(detail.generations.count),
    detail === null ? 'readConversation returned null' : ''
  );
}

// --- verdict -------------------------------------------------------------------

if (failures > 0) {
  console.error(`\n${failures} check(s) FAILED`);
  process.exit(1);
}
console.log('\nall checks passed');
process.exit(0);
