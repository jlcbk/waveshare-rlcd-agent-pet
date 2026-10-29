// Unit tests for the pure translation logic. Run: node test/translate.test.mjs
import assert from "node:assert/strict";
import { effectiveStatus, parseQuotaDocument, quotaSummary, toolStatusForName, translateHook } from "../lib/translate.mjs";

const windows = { activeWindowMs: 1000, toolWindowMs: 5000, completedHoldMs: 2000 };
const t0 = 1_000_000;

// Hook payload -> bridge event, and nothing sensitive survives the translation.
const pre = translateHook("PreToolUse", {
  toolCall: { name: "run_command", args: { CommandLine: "rm -rf /tmp/x" } },
  conversationId: "c1",
  workspacePaths: ["/w"],
  lastUserInput: "secret prompt",
  stepIdx: "7"
});
assert.equal(pre.source, "antigravity");
assert.equal(pre.type, "PreToolUse");
assert.equal(pre.status, "tool-use");
assert.equal(pre.tool, "run_command");
assert.equal(pre.sessionId, "c1");
assert.equal(pre.workspace, "/w");
assert.equal(pre.stepIdx, 7);
assert.equal(pre.message, "PreToolUse run_command");
const serialized = JSON.stringify(pre);
assert.ok(!serialized.includes("secret prompt") && !serialized.includes("rm -rf"), "prompt/args must not survive");

assert.equal(translateHook("PreToolUse", { toolCall: { name: "search_web" } }).status, "searching");
assert.equal(translateHook("PostToolUse", { toolCall: { name: "view_file" }, error: "exit 1" }).message, "PostToolUse view_file error");
assert.equal(translateHook("SessionStart", { modelName: "gemini-3.8-flash" }).model, "gemini-3.8-flash");
assert.equal(translateHook("SessionStart", {}).status, "started");
assert.equal(translateHook("PreInvocation", {}).status, "thinking");
assert.equal(translateHook("PostInvocation", {}).status, "working");
assert.equal(translateHook("Stop", { terminationReason: "NO_TOOL_CALL" }).status, "completed");
assert.equal(translateHook("Stop", { terminationReason: "USER_CANCELED" }).status, "completed");
assert.equal(translateHook("Stop", { terminationReason: "ERROR" }).status, "failed");
assert.equal(translateHook("Stop", { terminationReason: "ERROR" }).message, "Stop ERROR");
assert.equal(translateHook("PreInvocation", null).status, "thinking");
assert.equal(translateHook("PreInvocation", "garbage").status, "thinking");
assert.equal(translateHook("Whatever", {}).status, "event");

// Ageing: short windows for thinking/working, long for tool steps, hold for completed.
assert.equal(effectiveStatus(null, 0, t0, windows).status, "idle");
const thinking = translateHook("PreInvocation", {});
assert.equal(effectiveStatus(thinking, t0, t0 + 999, windows).status, "thinking");
assert.equal(effectiveStatus(thinking, t0, t0 + 1001, windows).status, "idle");
const tool = translateHook("PreToolUse", { toolCall: { name: "run_command" } });
assert.deepEqual(effectiveStatus(tool, t0, t0 + 4999, windows), { status: "tool-use", tool: "run_command", reason: "" });
assert.equal(effectiveStatus(tool, t0, t0 + 5001, windows).status, "idle");
const done = translateHook("Stop", { terminationReason: "NO_TOOL_CALL" });
assert.equal(effectiveStatus(done, t0, t0 + 1999, windows).status, "completed");
assert.equal(effectiveStatus(done, t0, t0 + 2001, windows).status, "idle");
const failed = translateHook("Stop", { terminationReason: "ERROR" });
assert.equal(effectiveStatus(failed, t0, t0 + 1999, windows).status, "failed");

// Quota document, shape as returned by `agy -p /usage --output-format json` on this machine.
const doc = {
  command: {
    name: "usage",
    data: {
      groups: [
        { name: "Gemini Models", buckets: [{ id: "gemini-weekly", window: "weekly", remaining_fraction: 0.958892822265625, reset_time: "2026-11-04T10:25:19Z" }] },
        { name: "Claude and GPT models", buckets: [{ id: "3p-weekly", window: "weekly", remaining_fraction: 1, reset_time: "2026-11-04T16:03:32Z" }] }
      ]
    }
  }
};
const groups = parseQuotaDocument(doc);
assert.equal(groups.length, 2);
assert.equal(groups[0].buckets[0].remainingPercent, 96);
assert.equal(groups[0].buckets[0].resetTime, "2026-11-04T10:25:19Z");
assert.equal(quotaSummary(groups), "GEM WK 96% | 3P WK 100%");
assert.equal(quotaSummary(parseQuotaDocument({})), "");
const withFiveHour = parseQuotaDocument({
  command: { data: { groups: [{ name: "Gemini Models", buckets: [{ id: "gemini-5h", window: "5h", remaining_fraction: 0.8 }, { id: "gemini-weekly", window: "weekly", remaining_fraction: 0.5 }] }] } }
});
assert.equal(quotaSummary(withFiveHour), "GEM 5H 80% WK 50%");

assert.equal(toolStatusForName(""), "tool-use");
assert.equal(toolStatusForName("grep_search"), "searching");
assert.equal(toolStatusForName("write_file"), "tool-use");

console.log("translate.test: ok");
