#include "net_worker.h"

#ifdef ARDUINO

#include <Arduino.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <mdns.h>
#include <time.h>

#include "anim_codec.h"
#include "app_config.h"
#include "pm_controller.h"
#include "poll_policy.h"
#include "usage_visuals.h"

namespace {

constexpr uint16_t kPollTimeoutMs = 2500;
constexpr uint16_t kChunkTimeoutMs = 4000;
constexpr uint16_t kWifiListenInterval = 10;
constexpr uint32_t kMaxFramesPerChunk = 240;
constexpr int kMaxChunkFailures = 4;
constexpr uint32_t kMdnsLookupTimeoutMs = 1500;
constexpr uint32_t kMdnsLookupCooldownMs = 15000;

struct ChunkSlot {
  uint8_t* data;  // kAnimChunkBytes; allocated for the first stream and kept (the net task may still write to it)
  size_t length;
  uint32_t firstFrame;
  uint32_t frameCount;
  bool full;
};

SemaphoreHandle_t gLock = nullptr;
TaskHandle_t gNetTaskHandle = nullptr;
std::string gSsid;
std::string gPassword;
std::string gPollUrl;
std::string gMdnsHost;
std::string gSavedBridgeIp;
NetSnapshot gSnapshot;
FocusRequestState gFocus;
DeviceTelemetry gTelemetry;
bool gFastPoll = false;
bool gRunning = false;
bool gReconnectBackoffReset = false;
uint32_t gLastAgentChangeMs = 0;  // millis() of the last agent change (0 = no baseline yet)
uint32_t gFailStreak = 0;         // consecutive failed polls (fast retry, then backoff)

void wakeNetTask() {
  if (gNetTaskHandle != nullptr) {
    xTaskNotifyGive(gNetTaskHandle);
  }
}

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
          prev.usageContext != agent.usageContext) {
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
    } else if (gFailStreak >= 2 || gSnapshot.lastPollOkAtMs == 0) {
      gSnapshot.agent.connected = false;
    }
  }
  if (ok) {
    if (!pmLightSleepArmed()) {
      Serial.printf("[BRIDGE] ok source=%d status=%d focus=%s count=%d rtt=%lums\n",
                    static_cast<int>(agent.source),
                    static_cast<int>(agent.status),
                    focus.mode == LocalFocusMode::Pinned ? "pinned" : "auto",
                    agent.focusCount,
                    static_cast<unsigned long>(doneAt - sentAt));
    }
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

  static WiFiClient sStreamClient;
  static HTTPClient sStreamHttp;
  sStreamHttp.setReuse(true);
  sStreamHttp.setConnectTimeout(kChunkTimeoutMs);
  sStreamHttp.setTimeout(kChunkTimeoutMs);
  const bool wasConnected = sStreamClient.connected();
  if (!sStreamHttp.begin(sStreamClient, url.c_str())) {
    sStreamClient.stop();
    markChunkFailure(generation, "begin");
    return;
  }
  int code = sStreamHttp.GET();
  if (code <= 0 && wasConnected) {
    sStreamHttp.end();
    sStreamClient.stop();
    if (sStreamHttp.begin(sStreamClient, url.c_str())) {
      code = sStreamHttp.GET();
    }
  }
  if (code != HTTP_CODE_OK) {
    sStreamHttp.end();
    sStreamClient.stop();
    markChunkFailure(generation, "status");
    return;
  }
  const int size = sStreamHttp.getSize();
  if (size <= 0 || size > static_cast<int>(kAnimChunkBytes)) {
    sStreamHttp.end();
    sStreamClient.stop();
    markChunkFailure(generation, "size");
    return;
  }
  uint8_t* target = gSlots[slot].data;
  WiFiClient* stream = sStreamHttp.getStreamPtr();
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
  sStreamHttp.end();
  if (received != static_cast<size_t>(size)) {
    sStreamClient.stop();
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

void setWifiPowerSave(wifi_ps_type_t wanted) {
  WiFi.setSleep(wanted);
  wifi_ps_type_t actual = WIFI_PS_NONE;
  esp_err_t err = esp_wifi_get_ps(&actual);
  if (err == ESP_OK && actual != wanted) {
    err = esp_wifi_set_ps(wanted);
  }
  if (err != ESP_OK) {
    Serial.printf("[WIFI] power-save setup failed: 0x%x\n", err);
  }
}

void beginWifi() {
  // With connect=false, begin() returns the previous status, not the setup result.
  WiFi.begin(gSsid.c_str(), gPassword.c_str(), 0, nullptr, false);
  wifi_config_t config = {};
  esp_err_t err = esp_wifi_get_config(WIFI_IF_STA, &config);
  if (err == ESP_OK) {
    config.sta.listen_interval = kWifiListenInterval;
    err = esp_wifi_set_config(WIFI_IF_STA, &config);
  }
  if (err != ESP_OK) {
    Serial.printf("[WIFI] listen interval setup failed: 0x%x\n", err);
  }
  err = esp_wifi_connect();
  if (err != ESP_OK) {
    Serial.printf("[WIFI] connection start failed: 0x%x\n", err);
  }
}

bool tryResolveBridgeHost(bool streaming) {
  if (gMdnsHost.empty()) {
    return false;
  }
  setWifiPowerSave(WIFI_PS_NONE);
  if (mdns_init() != ESP_OK) {
    setWifiPowerSave(streaming ? WIFI_PS_NONE : WIFI_PS_MAX_MODEM);
    return false;
  }
  esp_ip4_addr_t addr = {};
  const esp_err_t err = mdns_query_a(gMdnsHost.c_str(), kMdnsLookupTimeoutMs, &addr);
  mdns_free();
  setWifiPowerSave(streaming ? WIFI_PS_NONE : WIFI_PS_MAX_MODEM);
  if (err != ESP_OK || addr.addr == 0) {
    Serial.printf("[BRIDGE] mDNS lookup %s.local failed: 0x%x\n", gMdnsHost.c_str(), err);
    return false;
  }
  char ipBuf[24] = {};
  snprintf(ipBuf, sizeof(ipBuf), IPSTR, IP2STR(&addr));
  {
    Guard guard;
    gPollUrl = replaceUrlHost(gPollUrl.c_str(), ipBuf);
  }
  if (gSavedBridgeIp != ipBuf) {
    Preferences prefs;
    if (prefs.begin("net_cache", false)) {
      prefs.putString("bridge_ip", ipBuf);
      prefs.end();
    }
    gSavedBridgeIp = ipBuf;
  }
  Serial.printf("[BRIDGE] mDNS resolved %s.local -> %s\n", gMdnsHost.c_str(), ipBuf);
  return true;
}

void netTask(void*) {
  uint32_t lastReconnect = millis();
  uint32_t lastReport = 0;
  uint32_t lastPoll = 0;
  uint32_t lastMdnsLookup = 0;
  uint32_t lastRssiSample = 0;
  uint32_t reconnectAttempts = 0;
  uint32_t cachedIpRaw = 0;
  std::string cachedIp;
  int cachedRssi = 0;
  bool sntpStarted = false;
  bool sleepDisabled = false;
  wl_status_t lastStatus = WL_IDLE_STATUS;

  for (;;) {
    const uint32_t now = millis();
    const wl_status_t status = WiFi.status();
    const bool connected = status == WL_CONNECTED;

    bool resetBackoff = false;
    {
      Guard guard;
      if (gReconnectBackoffReset) {
        resetBackoff = true;
        gReconnectBackoffReset = false;
      }
    }
    if (resetBackoff && !connected) {
      reconnectAttempts = 0;
      lastReconnect = now - kWifiReconnectMs;
    }

    if (status != lastStatus) {
      if (connected) {
        const IPAddress ip = WiFi.localIP();
        cachedIpRaw = static_cast<uint32_t>(ip);
        cachedIp = ip.toString().c_str();
        cachedRssi = WiFi.RSSI();
        lastRssiSample = now;
        Serial.printf("[WIFI] connected ip=%s rssi=%d dBm\n", cachedIp.c_str(), cachedRssi);
        lastPoll = 0;  // poll immediately on (re)connect; only real agent changes reset sleep pacing
        reconnectAttempts = 0;
      } else {
        cachedIpRaw = 0;
        cachedIp.clear();
        cachedRssi = 0;
        Serial.printf("[WIFI] status=%d\n", static_cast<int>(status));
      }
      lastStatus = status;
    }

    const uint32_t reconnectInterval = wifiReconnectIntervalMs(reconnectAttempts);
    if (!connected && now - lastReconnect >= reconnectInterval) {
      reconnectAttempts += 1;
      Serial.printf("[WIFI] reconnect attempt=%lu interval=%lums status=%d\n",
                    static_cast<unsigned long>(reconnectAttempts),
                    static_cast<unsigned long>(reconnectInterval),
                    static_cast<int>(status));
      WiFi.disconnect(false);
      beginWifi();
      lastReconnect = now;
    }
    if (connected && !sntpStarted) {
      // UTC only; local time comes from the POSIX TZ the bridge sends.
      configTime(0, 0, "pool.ntp.org", "time.cloudflare.com", "ntp.aliyun.com");
      sntpStarted = true;
    }
    if (connected) {
      const IPAddress ip = WiFi.localIP();
      const uint32_t ipRaw = static_cast<uint32_t>(ip);
      if (ipRaw != cachedIpRaw) {
        cachedIpRaw = ipRaw;
        cachedIp = ip.toString().c_str();
      }
      if (lastRssiSample == 0 || now - lastRssiSample >= 5000) {
        cachedRssi = WiFi.RSSI();
        lastRssiSample = now;
      }
    }
    if (now - lastReport >= kWifiReportMs) {
      if (connected) {
        if (!pmLightSleepArmed()) {
          Serial.printf("[WIFI] ok ip=%s rssi=%d dBm\n", cachedIp.c_str(), cachedRssi);
        }
      } else {
        Serial.printf("[WIFI] waiting status=%d attempts=%lu next=%lums\n", static_cast<int>(status),
                      static_cast<unsigned long>(reconnectAttempts),
                      static_cast<unsigned long>(wifiReconnectIntervalMs(reconnectAttempts)));
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
      network.rssi = cachedRssi;
      network.ip = cachedIp;
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
      gSnapshot.pacingMode =
          deriveSleepPacingMode(fastPoll, busy, lastAgentChange != 0, now - lastAgentChange);
    }

    // Streaming needs low latency; restore maximum modem sleep afterwards.
    if (streaming != sleepDisabled) {
      setWifiPowerSave(streaming ? WIFI_PS_NONE : WIFI_PS_MAX_MODEM);
      sleepDisabled = streaming;
    }

    uint32_t interval = pollIntervalMs(fastPoll, busy, lastAgentChange != 0,
                                       now - lastAgentChange, failStreak);
    if (connected && (lastPoll == 0 || millis() - lastPoll >= interval)) {
      pollOnce();
      lastPoll = millis();
      // Re-evaluate interval after pollOnce() so a newly-busy or failed state
      // schedules the next wake accurately.
      {
        Guard guard;
        streaming = gStreamActive && !gStreamFailed;
        fastPoll = gFastPoll;
        busy = gSnapshot.agent.status == AgentStatus::Running ||
               gSnapshot.agent.status == AgentStatus::NeedsAttention;
        lastAgentChange = gLastAgentChangeMs;
        failStreak = gFailStreak;
        gSnapshot.pacingMode =
            deriveSleepPacingMode(fastPoll, busy, lastAgentChange != 0, lastPoll - lastAgentChange);
      }
      interval = pollIntervalMs(fastPoll, busy, lastAgentChange != 0,
                                lastPoll - lastAgentChange, failStreak);
      if (failStreak >= 2 && (lastMdnsLookup == 0 || lastPoll - lastMdnsLookup >= kMdnsLookupCooldownMs)) {
        lastMdnsLookup = lastPoll;
        if (tryResolveBridgeHost(streaming)) {
          lastPoll = 0;
          interval = 0;
        }
      }
    }
    if (connected && streaming) {
      fillStream();
    }
    if (streaming) {
      vTaskDelay(pdMS_TO_TICKS(2));
      continue;
    }

    // Event-driven sleep: block until the next scheduled poll / reconnect
    // deadline or an explicit xTaskNotifyGive() from a button/focus/stream
    // change on core 1. Eliminates the 10 Hz idle wakeups on core 0.
    const uint32_t afterNow = millis();
    uint32_t sleepMs = 1000;
    if (connected) {
      const uint32_t elapsed = afterNow - lastPoll;
      const uint32_t remaining = elapsed >= interval ? 0 : (interval - elapsed);
      sleepMs = remaining < 50 ? 50 : (remaining > kDeepIdlePollMs ? kDeepIdlePollMs : remaining);
    } else {
      const uint32_t targetRecon = wifiReconnectIntervalMs(reconnectAttempts);
      const uint32_t elapsedRecon = afterNow - lastReconnect;
      const uint32_t remainingRecon = elapsedRecon >= targetRecon ? 0 : (targetRecon - elapsedRecon);
      // Poll Wi-Fi association status every 250 ms during the first 4 s after
      // kicking off a connect; then relax to up to 2 s steps until the next
      // reconnect attempt is due.
      const uint32_t capMs = elapsedRecon < 4000 ? 250 : 2000;
      sleepMs = remainingRecon < capMs ? (remainingRecon < 50 ? 50 : remainingRecon) : capMs;
    }
    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(sleepMs));
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
  gMdnsHost = normalizeMdnsHost(kBridgeMdnsHost);
  if (gMdnsHost.empty()) {
    const std::string urlHost = extractUrlHost(gPollUrl.c_str());
    if (urlHost.size() > 6 && urlHost.compare(urlHost.size() - 6, 6, ".local") == 0) {
      gMdnsHost = normalizeMdnsHost(urlHost.c_str());
    }
  }
  {
    Preferences prefs;
    if (prefs.begin("net_cache", true)) {
      const String cachedIp = prefs.getString("bridge_ip", "");
      prefs.end();
      if (cachedIp.length() > 0) {
        gSavedBridgeIp = cachedIp.c_str();
        gPollUrl = replaceUrlHost(gPollUrl.c_str(), gSavedBridgeIp.c_str());
        Serial.printf("[BRIDGE] loaded cached IP from NVS: %s\n", gSavedBridgeIp.c_str());
      }
    }
  }
  WiFi.mode(WIFI_STA);
  WiFi.persistent(false);
  WiFi.setAutoReconnect(true);
  setWifiPowerSave(WIFI_PS_MAX_MODEM);
  beginWifi();
  gRunning = true;
  xTaskCreatePinnedToCore(netTask, "net", 12288, nullptr, 1, &gNetTaskHandle, 0);
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
  bool changed = false;
  {
    Guard guard;
    // The app calls this after every poll to mirror its focus state; only a real
    // selection change (not the periodic no-op sync) should restart active mode.
    changed = gFocus.mode != focus.mode || gFocus.focusId != focus.focusId ||
              gFocus.visibleIndex != focus.visibleIndex;
    gFocus = focus;
    if (changed) {
      gLastAgentChangeMs = millis();  // poll with the new focus promptly
    }
  }
  if (changed) {
    wakeNetTask();
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
  bool wake = false;
  {
    Guard guard;
    wake = fast && !gFastPoll;
    gFastPoll = fast;
  }
  if (wake) {
    wakeNetTask();
  }
}

void netWorkerWake() {
  if (!gRunning) {
    return;
  }
  {
    Guard guard;
    gReconnectBackoffReset = true;
    gLastAgentChangeMs = millis();
    gSnapshot.pacingMode = SleepPacingMode::Fast;
  }
  wakeNetTask();
}

void netWorkerStopForDeepSleep() {
  if (!gRunning) {
    return;
  }
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
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
  wakeNetTask();
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
void netWorkerWake() {}
void netWorkerStopForDeepSleep() {}
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
