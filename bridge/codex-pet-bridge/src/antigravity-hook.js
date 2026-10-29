#!/usr/bin/env node
import { fileURLToPath } from "node:url";

import { translateHook } from "./antigravity-translate.js";
import { readStdinJson, relayBridgeEvent } from "./bridge-relay.js";

if (isMain()) {
  if (process.env.PET_ANTIGRAVITY_INTERNAL === "1") {
    process.exit(0);
  }
  try {
    const input = await readStdinJson();
    const eventName = process.argv[2] || input.hook_event_name || input.type || "PreInvocation";
    const event = normalizeAntigravityHookEvent(eventName, input);
    const result = await relayBridgeEvent(event);
    if (!result.ok) {
      console.error(`pet-antigravity-hook queued event after send failure: ${result.error}`);
    }
    process.exit(0);
  } catch (error) {
    // Hooks are observational; never block Antigravity when the bridge is offline.
    console.error(`pet-antigravity-hook ignored error: ${error.message || String(error)}`);
    process.exit(0);
  }
}

export function normalizeAntigravityHookEvent(eventName, input = {}) {
  const translated = translateHook(eventName, input);
  const slug = slugFromPath(translated.workspace);
  return {
    ...translated,
    task: input.task || slug || "antigravity-session",
    notify: translated.status === "completed" || translated.status === "failed" || translated.status === "needs-attention"
  };
}

function slugFromPath(value) {
  const text = String(value || "").trim();
  if (!text) return "";
  return text.split(/[\\/]/).filter(Boolean).at(-1) || "";
}

function isMain() {
  return process.argv[1] === fileURLToPath(import.meta.url);
}
