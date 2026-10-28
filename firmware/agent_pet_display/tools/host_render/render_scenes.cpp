// Renders representative screens with the real firmware drawing code and the
// real U8g2 library, then writes 400x300 PBM files for review.
//
// Usage: render_scenes <output-dir>

#include <Arduino.h>

#include <stdio.h>
#include <sys/stat.h>

#include <string>
#include <vector>

#include "battery_monitor.h"
#include "egg_builtin.h"
#include "host_frame.h"
#include "models.h"
#include "pet_state_machine.h"
#include "screen_renderer.h"

namespace {

struct Scene {
  std::string name;
  AgentState agent;
  PowerState power;
  EnvironmentState environment;
  NetworkState network;
  ScreenPage page;
  uint32_t millis;
  DisplaySettings settings{};
};

EnvironmentState sampleEnvironment() {
  EnvironmentState env{};
  env.clockValid = true;
  env.year = 2026;
  env.month = 9;
  env.day = 28;
  env.hour = 10;
  env.minute = 42;
  env.second = 17;
  env.weekday = 1;
  env.temperatureC = 22.6f;
  env.humidityPct = 48.0f;
  env.climateValid = true;
  return env;
}

NetworkState onlineNetwork() {
  NetworkState net{};
  net.wifiKnown = true;
  net.wifiConnected = true;
  net.rssi = -52;
  net.ip = "192.168.1.50";
  return net;
}

AgentState codexAgent(const char* detail, AgentStatus status) {
  AgentState agent{};
  agent.connected = true;
  agent.source = SourceKind::Codex;
  agent.status = status;
  agent.statusDetail = detail;
  agent.task = "refactor bridge relay";
  agent.updatedAt = "2026-09-28T09:42:00Z";
  agent.usageToday = "812k tok";
  agent.usageContext = "189k / 258k";
  agent.usageQuota = "5H 88% WK 92%";
  agent.focusCount = 3;
  return agent;
}

PowerState battery(int percent, int mv) {
  PowerState power{};
  power.percent = percent;
  power.voltageMv = mv;
  power.sampleOk = true;
  return power;
}

std::vector<Scene> buildScenes() {
  std::vector<Scene> scenes;
  // Full-width metric cards + top-bar voltage: Today reads "49M tok", which
  // the Overview page expands to "49M Token" at draw time; 4095 mV renders
  // as "4.09V".
  AgentState fullWidth = codexAgent("thinking", AgentStatus::Running);
  fullWidth.usageToday = "49M tok";
  fullWidth.usageContext = "189k / 258k";
  scenes.push_back({"overview-full-width-voltage", fullWidth, battery(87, 4095),
                    sampleEnvironment(), onlineNetwork(), ScreenPage::Overview, 1000});
  // Failed/un-calibrated ADC read: the top bar must show "--%", never
  // "0% LOW" (a dead sensor is not an empty battery).
  PowerState invalidBattery{};
  invalidBattery.percent = 0;
  invalidBattery.voltageMv = 0;
  invalidBattery.sampleOk = false;
  scenes.push_back({"overview-battery-invalid", fullWidth, invalidBattery,
                    sampleEnvironment(), onlineNetwork(), ScreenPage::Overview, 1000});
  scenes.push_back({"overview-codex-thinking", codexAgent("thinking", AgentStatus::Running), battery(84, 3980),
                    sampleEnvironment(), onlineNetwork(), ScreenPage::Overview, 1000});
  AgentState done = codexAgent("done", AgentStatus::Completed);
  done.task = "mac-codex-runtime";
  done.usageToday = "1.6M tok";
  done.usageContext = "121k / 475k";
  done.usageQuota = "5H 90% WK 78%";
  EnvironmentState photoEnv = sampleEnvironment();
  photoEnv.year = 2026;
  photoEnv.month = 10;
  photoEnv.day = 27;
  photoEnv.hour = 12;
  photoEnv.minute = 3;
  photoEnv.second = 1;
  photoEnv.temperatureC = 19.3f;
  photoEnv.humidityPct = 34.0f;
  NetworkState photoNetwork = onlineNetwork();
  photoNetwork.rssi = -46;
  scenes.push_back({"overview-codex-done", done, battery(88, 3980), photoEnv, photoNetwork,
                    ScreenPage::Overview, 1000});
  AgentState longAttention = codexAgent("needs-attention", AgentStatus::NeedsAttention);
  longAttention.source = SourceKind::OpenClaw;
  longAttention.task = "Review the unusually long project request before continuing";
  longAttention.usageToday = "1234567890123456789012345M tokens";
  longAttention.usageContext = "1234567890123k / 1234567890123k";
  longAttention.usageQuota = "5H 100% WK 99%";
  longAttention.focusMode = FocusMode::Pinned;
  longAttention.focusIndex = 1;
  NetworkState reconnecting = onlineNetwork();
  reconnecting.wifiConnected = false;
  reconnecting.wifiConnecting = true;
  PowerState lowBattery = battery(4, 3410);
  lowBattery.lowBattery = true;
  scenes.push_back({"overview-attention-long", longAttention, lowBattery, photoEnv, onlineNetwork(),
                    ScreenPage::Overview, 1000});
  AgentState otherMetrics = codexAgent("working", AgentStatus::Running);
  otherMetrics.usageQuotaStyle = "text";
  otherMetrics.usageQuotaLabel = "CREDITS";
  otherMetrics.usageQuota = "12 / 250";
  EnvironmentState noTimeEnv = sampleEnvironment();
  noTimeEnv.clockValid = false;
  scenes.push_back({"overview-text-metric-no-time", otherMetrics, battery(84, 3980), noTimeEnv, onlineNetwork(),
                    ScreenPage::Overview, 1000});
  scenes.push_back({"usage-codex", codexAgent("tool-use", AgentStatus::Running), battery(84, 3980),
                    sampleEnvironment(), onlineNetwork(), ScreenPage::Usage, 1000});
  AgentState offline{};
  NetworkState wifiOk = onlineNetwork();
  scenes.push_back({"overview-bridge-offline", offline, battery(84, 3980), sampleEnvironment(), wifiOk,
                    ScreenPage::Overview, 1000});
  scenes.push_back({"overview-wifi-reconnecting", offline, lowBattery, noTimeEnv, reconnecting,
                    ScreenPage::Overview, 1000});

  // Wi-Fi icon states on the top bars: four rssiBars() levels (one scene per
  // bucket) plus unknown/TRY/OFF/OK, then one bucket on the Usage page to
  // check its narrower Doto-suffix budget (climate starts at x=92 there).
  AgentState wifiAgent = codexAgent("working", AgentStatus::Running);
  const PowerState wifiPower = derivePowerState(3980, false);
  // One scene per rssiBars() bucket; the filename states the actual level.
  const int wifiRssi[] = {-50, -60, -70, -85};  // -> levels 4, 3, 2, 1
  for (int index = 0; index < 4; ++index) {
    NetworkState net{};
    net.wifiKnown = true;
    net.wifiConnected = true;
    net.rssi = wifiRssi[index];
    net.ip = "192.168.1.50";
    char name[48];
    snprintf(name, sizeof(name), "overview-wifi-signal-%dof4", 4 - index);
    scenes.push_back({name, wifiAgent, wifiPower, sampleEnvironment(), net,
                      ScreenPage::Overview, 1000});
  }
  NetworkState wifiOff{};
  wifiOff.wifiKnown = true;
  wifiOff.reconnectAttempts = 7;
  scenes.push_back({"overview-wifi-off", wifiAgent, wifiPower, sampleEnvironment(), wifiOff,
                    ScreenPage::Overview, 1000});
  NetworkState wifiTrying{};
  wifiTrying.wifiKnown = true;
  wifiTrying.wifiConnecting = true;
  wifiTrying.reconnectAttempts = 2;
  scenes.push_back({"overview-wifi-connecting", wifiAgent, wifiPower, sampleEnvironment(), wifiTrying,
                    ScreenPage::Overview, 1000});
  scenes.push_back({"overview-wifi-unknown", wifiAgent, wifiPower, sampleEnvironment(), NetworkState{},
                    ScreenPage::Overview, 1000});
  NetworkState wifiOkNoRssi{};
  wifiOkNoRssi.wifiKnown = true;
  wifiOkNoRssi.wifiConnected = true;
  scenes.push_back({"overview-wifi-ok-no-rssi", wifiAgent, wifiPower, sampleEnvironment(), wifiOkNoRssi,
                    ScreenPage::Overview, 1000});
  NetworkState usageWifi = onlineNetwork();
  usageWifi.rssi = -70;
  scenes.push_back({"usage-wifi-signal-2of4", wifiAgent, wifiPower, sampleEnvironment(), usageWifi,
                    ScreenPage::Usage, 1000});

  // Clock page: every style with a busy Codex plus two other agents.
  AgentState multi = codexAgent("thinking", AgentStatus::Running);
  multi.focusCount = 3;
  multi.agentSlots[0] = AgentSlotSummary{"slot_a", SourceKind::Codex, AgentStatus::Running, "refactor", "", true};
  multi.agentSlots[1] = AgentSlotSummary{"slot_b", SourceKind::Hermes, AgentStatus::Completed, "notes", "", true};
  multi.agentSlots[2] = AgentSlotSummary{"slot_c", SourceKind::ClaudeCode, AgentStatus::Idle, "", "", true};
  for (int index = 0; index < kClockStyleCount; ++index) {
    Scene scene{std::string("clock-") + clockStyleName(static_cast<ClockStyle>(index)), multi, battery(84, 3980),
                sampleEnvironment(), onlineNetwork(), ScreenPage::Clock, 1000};
    scene.settings.clockStyle = static_cast<ClockStyle>(index);
    scenes.push_back(scene);
  }

  // Variants: someone is waiting for you, 12-hour afternoon, bridge down, night pet.
  AgentState waiting = codexAgent("needs-attention", AgentStatus::NeedsAttention);
  waiting.source = SourceKind::Hermes;
  waiting.task = "approve shell command";
  waiting.focusCount = 2;
  waiting.agentSlots[0] = AgentSlotSummary{"slot_h", SourceKind::Hermes, AgentStatus::NeedsAttention, "", "", true};
  waiting.agentSlots[1] = AgentSlotSummary{"slot_a", SourceKind::Codex, AgentStatus::Running, "", "", true};
  Scene attention{"clock-segment-attention-12h", waiting, battery(23, 3610), sampleEnvironment(), onlineNetwork(),
                  ScreenPage::Clock, 1000};
  attention.environment.hour = 16;
  attention.environment.minute = 7;
  attention.environment.second = 44;
  attention.settings.clockStyle = ClockStyle::Segment;
  attention.settings.hour12 = true;
  scenes.push_back(attention);
  Scene termAttention = attention;
  termAttention.name = "clock-terminal-attention";
  termAttention.settings.clockStyle = ClockStyle::Terminal;
  termAttention.settings.hour12 = false;
  scenes.push_back(termAttention);

  Scene analogOffline{"clock-analog-bridge-off", offline, battery(84, 3980), sampleEnvironment(), wifiOk,
                      ScreenPage::Clock, 1000};
  analogOffline.environment.hour = 3;
  analogOffline.environment.minute = 51;
  analogOffline.environment.second = 5;
  analogOffline.settings.clockStyle = ClockStyle::Analog;
  scenes.push_back(analogOffline);

  AgentState idle = codexAgent("idle", AgentStatus::Idle);
  idle.task = "";
  Scene night{"clock-pet-night", idle, battery(84, 3980), sampleEnvironment(), onlineNetwork(), ScreenPage::Clock, 1000};
  night.environment.hour = 1;
  night.environment.minute = 12;
  night.settings.clockStyle = ClockStyle::Pet;
  scenes.push_back(night);

  Scene words{"clock-words-2359", idle, battery(84, 3980), sampleEnvironment(), onlineNetwork(), ScreenPage::Clock, 1000};
  words.environment.hour = 23;
  words.environment.minute = 58;
  words.settings.clockStyle = ClockStyle::Words;
  scenes.push_back(words);

  EnvironmentState unsynced = sampleEnvironment();
  unsynced.clockValid = false;
  Scene noTime{"clock-sans-unsynced", offline, battery(84, 3980), unsynced, wifiOk, ScreenPage::Clock, 1000};
  scenes.push_back(noTime);
  return scenes;
}

}  // namespace

int main(int argc, char** argv) {
  const std::string outDir = argc > 1 ? argv[1] : "host-render-out";
  mkdir(outDir.c_str(), 0755);

  ScreenRenderer renderer;
  renderer.begin();

  int failures = 0;
  for (const Scene& scene : buildScenes()) {
    hostSetMillis(scene.millis);
    const DisplayState view =
        deriveDisplayState(scene.agent, scene.power, scene.environment, scene.page, scene.network, scene.settings);
    renderer.render(view, scene.power);
    const std::string path = outDir + "/" + scene.name + ".pbm";
    if (!hostWritePanelPbm(path)) {
      fprintf(stderr, "failed to write %s\n", path.c_str());
      ++failures;
      continue;
    }
    printf("wrote %s\n", path.c_str());
  }
  // Easter egg screens are drawn straight into the panel buffer.
  U8G2* panel = renderer.u8g2();
  // Low-battery notice: drawn directly (no DisplayState), then the panel would
  // deep sleep. 3540 mV / sampleOk shows as "3.54V".
  PowerState noticePower{};
  noticePower.voltageMv = 3540;
  noticePower.percent = 4;
  noticePower.lowBattery = true;
  noticePower.sampleOk = true;
  renderer.renderLowBatteryNotice(noticePower);
  failures += hostWritePanelPbm(outDir + "/low-battery-notice.pbm") ? 0 : 1;
  printf("wrote %s\n", (outDir + "/low-battery-notice.pbm").c_str());
  const uint32_t eggTimes[] = {400, 2600, 6100, 10500};
  for (uint32_t at : eggTimes) {
    drawBuiltinEggFrame(*panel, at);
    const std::string path = outDir + "/egg-builtin-" + std::to_string(at) + "ms.pbm";
    failures += hostWritePanelPbm(path) ? 0 : 1;
    printf("wrote %s\n", path.c_str());
  }
  drawEggCountdown(*panel, "badapple", 3);
  failures += hostWritePanelPbm(outDir + "/egg-countdown.pbm") ? 0 : 1;
  drawEggMessage(*panel, "STREAM STALLED", "CHECK THE BRIDGE / WIFI");
  failures += hostWritePanelPbm(outDir + "/egg-message.pbm") ? 0 : 1;
  drawEggMessage(*panel, "BAD ANIMATION DATA", "UPLOAD IT AGAIN");
  failures += hostWritePanelPbm(outDir + "/egg-message-bad.pbm") ? 0 : 1;
  return failures == 0 ? 0 : 1;
}
