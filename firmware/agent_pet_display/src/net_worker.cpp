#include "net_worker.h"

#ifdef ARDUINO

#include <Arduino.h>
#include <HTTPClient.h>
#include <WiFi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <time.h>

#include "anim_codec.h"
#include "app_config.h"
#include "poll_policy.h"

namespace {

constexpr uint16_t kPollTimeoutMs = 2500;
constexpr uint16_t kChunkTimeoutMs = 4000;
constexpr uint32_t kMaxFramesPerChunk = 240;
constexpr int kMaxChunkFailures = 4;

struct ChunkSlot {
  uint8_t* data;  // kAnimChunkBytes; allocated for the first stream and kept (the net task may still write to it)
  size_t length;
  uint32_t firstFrame;
  uint32_t frameCount;
  bool full;
};

SemaphoreHandle_t gLock = nullptr;
std::string gSsid;
std::string gPassword;
std::string gPollUrl;
NetSnapshot gSnapshot;
FocusRequestState gFocus;
DeviceTelemetry gTelemetry;
bool gFastPoll = false;
bool gRunning = false;
uint32_t gLastAgentChangeMs = 0;  // millis() of the last agent change (0 = no baseline yet)
uint32_t gFailStreak = 0;         // consecutive failed polls (fast retry, then backoff)

bool gStreamActive = false;
bool gStreamFailed = false;
std::string gStreamId;
uint32_t gStreamTotal = 0;
uint32_t gStreamNextFrame = 0;
uint32_t gStreamGeneration = 0;
int gChunkFailures = 0;
ChunkSlot gSlotTable[kAnimChunkSlots] = {};
ChunkSlot* gSlots = nullptr;  // points at gSlotTable once every buffer exists

class Guard {
 public:
  Guard() { xSemaphoreTake(gLock, portMAX_DELAY); }
  ~Guard() { xSemaphoreGive(gLock); }
  Guard(const Guard&) = delete;
  Guard& operator=(const Guard&) = delete;
};

void resetSlotsLocked() {
  if (!gSlots) {
    return;
  }
  for (int index = 0; index < kAnimChunkSlots; ++index) {
    gSlots[index].full = false;
    gSlots[index].length = 0;
    gSlots[index].frameCount = 0;
  }
}

void pollOnce() {
  FocusRequestState focus;
  DeviceTelemetry telemetry;
  std::string base;
  {
    Guard guard;
    focus = gFocus;
    telemetry = gTelemetry;
    base = gPollUrl;
  }
  const std::string url = buildFocusedPollUrl(base.c_str(), focus) + buildTelemetryQuery(telemetry);
  AgentState agent{};
  BridgeCommands commands{};
  const uint32_t sentAt = millis();
  const bool ok = fetchPollState(url.c_str(), agent, commands, kPollTimeoutMs);
  const uint32_t doneAt = millis();
  {
    Guard guard;
    gSnapshot.pollSeq += 1;
    gSnapshot.lastPollOk = ok;
    gFailStreak = ok ? 0 : gFailStreak + 1;
    if (ok) {
      const bool firstSuccess = gSnapshot.lastPollOkAtMs == 0;
      const AgentState& prev = gSnapshot.agent;
      if (firstSuccess || prev.status != agent.status || prev.statusDetail != agent.statusDetail ||
          prev.task != agent.task || prev.usageToday != agent.usageToday ||
          prev.usageContext != agent.usageContext || prev.usageQuota != agent.usageQuota) {
        gLastAgentChangeMs = doneAt;
      }
      gSnapshot.agent = agent;
      gSnapshot.commands = commands;
      gSnapshot.lastPollOkAtMs = doneAt;
      if (commands.serverTimeMs > 0) {
        const uint32_t midpoint = sentAt + (doneAt - sentAt) / 2;
        gSnapshot.serverOffsetMs = commands.serverTimeMs - static_cast<int64_t>(midpoint);
        gSnapshot.serverOffsetKnown = true;
      }
    } else {
      gSnapshot.agent.connected = false;
    }
  }
  if (ok) {
    Serial.printf("[BRIDGE] ok source=%d status=%d focus=%s count=%d rtt=%lums\n",
                  static_cast<int>(agent.source),
                  static_cast<int>(agent.status),
                  focus.mode == LocalFocusMode::Pinned ? "pinned" : "auto",
                  agent.focusCount,
                  static_cast<unsigned long>(doneAt - sentAt));
  } else {
    Serial.printf("[BRIDGE] poll failed after %lums\n", static_cast<unsigned long>(doneAt - sentAt));
  }
}

void markChunkFailure(uint32_t generation, const char* reason) {
  Guard guard;
  if (generation != gStreamGeneration) {
    return;
  }
  gChunkFailures += 1;
  Serial.printf("[EGG] chunk failed (%s) %d/%d\n", reason, gChunkFailures, kMaxChunkFailures);
  if (gChunkFailures >= kMaxChunkFailures) {
    gStreamFailed = true;
  }
}

void fillStream() {
  int slot = -1;
  std::string id;
  std::string base;
  uint32_t next = 0;
  uint32_t total = 0;
  uint32_t generation = 0;
  {
    Guard guard;
    if (!gStreamActive || gStreamFailed || !gSlots || gStreamNextFrame >= gStreamTotal) {
      return;
    }
    for (int index = 0; index < kAnimChunkSlots; ++index) {
      if (!gSlots[index].full) {
        slot = index;
        break;
      }
    }
    id = gStreamId;
    base = gPollUrl;
    next = gStreamNextFrame;
    total = gStreamTotal;
    generation = gStreamGeneration;
  }
  if (slot < 0) {
    return;
  }

  const uint32_t remaining = total - next;
  const uint32_t count = remaining < kMaxFramesPerChunk ? remaining : kMaxFramesPerChunk;
  const std::string url = buildAnimChunkUrl(base.c_str(), id, next, count, kAnimChunkBytes);
  if (url.empty()) {
    markChunkFailure(generation, "url");
    return;
  }

  HTTPClient http;
  http.setConnectTimeout(kChunkTimeoutMs);
  http.setTimeout(kChunkTimeoutMs);
  if (!http.begin(url.c_str())) {
    markChunkFailure(generation, "begin");
    return;
  }
  const int code = http.GET();
  if (code != HTTP_CODE_OK) {
    http.end();
    markChunkFailure(generation, "status");
    return;
  }
  const int size = http.getSize();
  if (size <= 0 || size > static_cast<int>(kAnimChunkBytes)) {
    http.end();
    markChunkFailure(generation, "size");
    return;
  }
  uint8_t* target = gSlots[slot].data;
  WiFiClient* stream = http.getStreamPtr();
  size_t received = 0;
  const uint32_t deadline = millis() + kChunkTimeoutMs;
  while (received < static_cast<size_t>(size) && static_cast<int32_t>(millis() - deadline) < 0) {
    const int available = stream ? stream->available() : 0;
    if (available > 0) {
      const size_t want = static_cast<size_t>(size) - received;
      const int got = stream->read(target + received, static_cast<size_t>(available) < want ? available : want);
      if (got > 0) {
        received += static_cast<size_t>(got);
      }
    } else if (!stream || !stream->connected()) {
      break;
    } else {
      delay(1);
    }
  }
  http.end();
  if (received != static_cast<size_t>(size)) {
    markChunkFailure(generation, "short read");
    return;
  }

  uint32_t frames = 0;
  size_t offset = 0;
  while (offset < received) {
    uint8_t type = 0;
    const uint8_t* payload = nullptr;
    size_t payloadLength = 0;
    const size_t used = readAnimRecord(target + offset, received - offset, type, payload, payloadLength);
    if (used == 0) {
      break;
    }
    offset += used;
    frames += 1;
  }
  if (offset != received || frames == 0) {
    markChunkFailure(generation, "records");
    return;
  }

  Guard guard;
  if (generation != gStreamGeneration || !gStreamActive) {
    return;  // stream was stopped or restarted while we were downloading
  }
  gSlots[slot].length = received;
  gSlots[slot].firstFrame = next;
  gSlots[slot].frameCount = frames;
  gSlots[slot].full = true;
  gStreamNextFrame = next + frames;
  gChunkFailures = 0;
}

void netTask(void*) {
  uint32_t lastReconnect = millis();
  uint32_t lastReport = 0;
  uint32_t lastPoll = 0;
  uint32_t reconnectAttempts = 0;
  bool sntpStarted = false;
  bool sleepDisabled = false;
  wl_status_t lastStatus = WL_IDLE_STATUS;

  for (;;) {
    const uint32_t now = millis();
    const wl_status_t status = WiFi.status();
    const bool connected = status == WL_CONNECTED;

    if (status != lastStatus) {
      if (connected) {
        Serial.printf("[WIFI] connected ip=%s rssi=%d dBm\n", WiFi.localIP().toString().c_str(), WiFi.RSSI());
        {
          Guard guard;
          gLastAgentChangeMs = now;  // resync quickly after (re)connect
        }
        reconnectAttempts = 0;
      } else {
        Serial.printf("[WIFI] status=%d\n", static_cast<int>(status));
      }
      lastStatus = status;
    }
    if (!connected && now - lastReconnect >= kWifiReconnectMs) {
      reconnectAttempts += 1;
      Serial.printf("[WIFI] reconnect attempt=%lu status=%d\n", static_cast<unsigned long>(reconnectAttempts),
                    static_cast<int>(status));
      WiFi.disconnect(false);
      WiFi.begin(gSsid.c_str(), gPassword.c_str());
      lastReconnect = now;
    }
    if (connected && !sntpStarted) {
      // UTC only; local time comes from the POSIX TZ the bridge sends.
      configTime(0, 0, "pool.ntp.org", "time.cloudflare.com", "ntp.aliyun.com");
      sntpStarted = true;
    }
    if (now - lastReport >= kWifiReportMs) {
      if (connected) {
        Serial.printf("[WIFI] ok ip=%s rssi=%d dBm\n", WiFi.localIP().toString().c_str(), WiFi.RSSI());
      } else {
        Serial.printf("[WIFI] waiting status=%d attempts=%lu\n", static_cast<int>(status),
                      static_cast<unsigned long>(reconnectAttempts));
      }
      lastReport = now;
    }

    NetworkState network{};
    network.wifiKnown = true;
    network.wifiConnected = connected;
    network.wifiConnecting =
        !connected && (status == WL_IDLE_STATUS || status == WL_DISCONNECTED || status == WL_SCAN_COMPLETED);
    network.wifiStatusCode = static_cast<int>(status);
    network.reconnectAttempts = reconnectAttempts;
    if (connected) {
      network.rssi = WiFi.RSSI();
      network.ip = WiFi.localIP().toString().c_str();
    }

    bool streaming = false;
    bool fastPoll = false;
    bool busy = false;
    uint32_t lastAgentChange = 0;
    uint32_t failStreak = 0;
    {
      Guard guard;
      gSnapshot.network = network;
      if (!connected) {
        gSnapshot.agent.connected = false;
      }
      streaming = gStreamActive && !gStreamFailed;
      fastPoll = gFastPoll;
      busy = gSnapshot.agent.status == AgentStatus::Running ||
             gSnapshot.agent.status == AgentStatus::NeedsAttention;
      lastAgentChange = gLastAgentChangeMs;
      failStreak = gFailStreak;
    }

    // Modem sleep adds ~100 ms latency per request; turn it off while streaming.
    if (streaming != sleepDisabled) {
      WiFi.setSleep(!streaming);
      sleepDisabled = streaming;
    }

    const uint32_t interval = pollIntervalMs(fastPoll, busy, lastAgentChange != 0,
                                             now - lastAgentChange, failStreak);
    if (connected && millis() - lastPoll >= interval) {
      pollOnce();
      lastPoll = millis();
    }
    if (connected && streaming) {
      fillStream();
    }
    // Idle pacing 100 ms: poll scheduling and status checks tolerate it;
    // streaming keeps its 2 ms cadence.
    vTaskDelay(pdMS_TO_TICKS(streaming ? 2 : 100));
  }
}

}  // namespace

void netWorkerBegin(const char* ssid, const char* password, const char* pollUrl) {
  if (gRunning) {
    return;
  }
  gLock = xSemaphoreCreateMutex();
  gSsid = ssid ? ssid : "";
  gPassword = password ? password : "";
  gPollUrl = pollUrl ? pollUrl : "";
  WiFi.mode(WIFI_STA);
  WiFi.persistent(false);
  WiFi.setAutoReconnect(true);
  WiFi.begin(gSsid.c_str(), gPassword.c_str());
  gRunning = true;
  xTaskCreatePinnedToCore(netTask, "net", 12288, nullptr, 1, nullptr, 0);
}

bool netWorkerSnapshot(NetSnapshot& out) {
  if (!gRunning) {
    return false;
  }
  Guard guard;
  out = gSnapshot;
  return true;
}

void netWorkerSetFocus(const FocusRequestState& focus) {
  if (!gRunning) {
    return;
  }
  Guard guard;
  // The app calls this after every poll to mirror its focus state; only a real
  // selection change (not the periodic no-op sync) should restart active mode.
  const bool changed = gFocus.mode != focus.mode || gFocus.focusId != focus.focusId ||
                       gFocus.visibleIndex != focus.visibleIndex;
  gFocus = focus;
  if (changed) {
    gLastAgentChangeMs = millis();  // poll with the new focus promptly
  }
}

void netWorkerSetTelemetry(const DeviceTelemetry& telemetry) {
  if (!gRunning) {
    return;
  }
  Guard guard;
  gTelemetry = telemetry;
}

void netWorkerSetFastPoll(bool fast) {
  if (!gRunning) {
    return;
  }
  Guard guard;
  gFastPoll = fast;
}

void netWorkerStartStream(const std::string& id, uint32_t totalFrames) {
  if (!gRunning) {
    return;
  }
  Guard guard;
  if (!gSlots) {
    // Separate 16 KB blocks: without PSRAM one 48 KB block is often not available.
    bool allocated = true;
    for (int index = 0; index < kAnimChunkSlots; ++index) {
      if (!gSlotTable[index].data) {
        gSlotTable[index].data = static_cast<uint8_t*>(malloc(kAnimChunkBytes));
      }
      allocated = allocated && gSlotTable[index].data != nullptr;
    }
    if (allocated) {
      gSlots = gSlotTable;
    }
  }
  gStreamGeneration += 1;
  gChunkFailures = 0;
  if (!gSlots) {
    gStreamActive = false;
    gStreamFailed = true;
    return;
  }
  resetSlotsLocked();
  gStreamId = id;
  gStreamTotal = totalFrames;
  gStreamNextFrame = 0;
  gStreamFailed = false;
  gStreamActive = true;
}

void netWorkerStopStream() {
  if (!gRunning) {
    return;
  }
  Guard guard;
  gStreamGeneration += 1;
  gStreamActive = false;
  gStreamFailed = false;
  resetSlotsLocked();
}

bool netWorkerPeekChunk(uint32_t firstFrame, AnimChunkView& out) {
  if (!gRunning) {
    return false;
  }
  Guard guard;
  if (!gSlots) {
    return false;
  }
  for (int index = 0; index < kAnimChunkSlots; ++index) {
    if (gSlots[index].full && gSlots[index].firstFrame == firstFrame) {
      out.data = gSlots[index].data;
      out.length = gSlots[index].length;
      out.firstFrame = gSlots[index].firstFrame;
      out.frameCount = gSlots[index].frameCount;
      out.slot = index;
      return true;
    }
  }
  return false;
}

void netWorkerReleaseChunk(int slot) {
  if (!gRunning || slot < 0 || slot >= kAnimChunkSlots) {
    return;
  }
  Guard guard;
  if (gSlots) {
    gSlots[slot].full = false;
  }
}

bool netWorkerStreamFailed() {
  if (!gRunning) {
    return true;
  }
  Guard guard;
  return gStreamFailed;
}

#else  // host builds: no network

void netWorkerBegin(const char*, const char*, const char*) {}
bool netWorkerSnapshot(NetSnapshot& out) {
  out = NetSnapshot{};
  return false;
}
void netWorkerSetFocus(const FocusRequestState&) {}
void netWorkerSetTelemetry(const DeviceTelemetry&) {}
void netWorkerSetFastPoll(bool) {}
void netWorkerStartStream(const std::string&, uint32_t) {}
void netWorkerStopStream() {}
bool netWorkerPeekChunk(uint32_t, AnimChunkView&) {
  return false;
}
void netWorkerReleaseChunk(int) {}
bool netWorkerStreamFailed() {
  return true;
}

#endif
