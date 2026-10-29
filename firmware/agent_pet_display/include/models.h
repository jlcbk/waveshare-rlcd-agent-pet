#pragma once

#include <array>
#include <stdint.h>
#include <string>

#include "clock_model.h"

enum class SourceKind {
  Codex,
  Hermes,
  OpenClaw,
  ClaudeCode,
  Antigravity,
  Unknown,
};

enum class AgentStatus {
  Idle,
  Running,
  NeedsAttention,
  Completed,
  Error,
};

enum class PetMode {
  Sleep,
  Idle,
  Starting,
  Thinking,
  Searching,
  ToolUse,
  Working,
  AlmostDone,
  Busy,
  Attention,
  Celebrate,
  Error,
  Tired,
  Charging,
};

enum class ScreenPage {
  Overview,
  Usage,
  Clock,
};

static constexpr int kScreenPageCount = 3;

// User-facing display preferences; persisted on the board and optionally
// pushed from the bridge (menu bar app / dashboard).
struct DisplaySettings {
  ClockStyle clockStyle{ClockStyle::Sans};
  bool hour12{false};
  bool showSeconds{true};
};

enum class FocusMode {
  Auto,
  Pinned,
};

static constexpr int kMaxAgentSlots = 6;

struct AgentSlotSummary {
  std::string id{};
  SourceKind source{SourceKind::Unknown};
  AgentStatus status{AgentStatus::Idle};
  std::string task{};
  std::string updatedAt{};
  bool present{false};
  std::string sourceRaw{};
};

struct AgentState {
  SourceKind source{SourceKind::Unknown};
  std::string sourceRaw{};
  AgentStatus status{AgentStatus::Idle};
  std::string statusDetail{"idle"};
  std::string task{};
  std::string updatedAt{};
  FocusMode focusMode{FocusMode::Auto};
  std::string focusId{};
  int focusIndex{-1};
  int focusCount{0};
  std::array<AgentSlotSummary, kMaxAgentSlots> agentSlots{};
  std::string usageToday{"--"};
  std::string usageTodayLabel{"TODAY"};
  std::string usageTodayHint{"Today total in this workspace"};
  std::string usageContext{"--"};
  std::string usageContextLabel{"CONTEXT"};
  std::string usageContextHint{"Current turn tokens / model window"};
  std::string usageQuota{"--"};
  std::string usageQuotaLabel{"QUOTA"};
  std::string usageQuotaHint{"Remaining 5-hour and weekly limits"};
  std::string usageQuotaStyle{"quota"};
  bool connected{false};
};

struct PowerState {
  int voltageMv{0};
  int percent{0};
  bool charging{false};
  bool lowBattery{false};
  bool sampleOk{false};
};

struct EnvironmentState {
  bool clockValid{false};
  int year{0};
  int month{0};
  int day{0};
  int hour{0};
  int minute{0};
  int second{0};
  int weekday{0};
  float temperatureC{0.0f};
  float humidityPct{0.0f};
  bool climateValid{false};
};

struct NetworkState {
  bool wifiKnown{false};
  bool wifiConnected{false};
  bool wifiConnecting{false};
  int wifiStatusCode{0};
  int rssi{0};
  uint32_t reconnectAttempts{0};
  std::string ip{};
};

struct ClockAgentLine {
  std::string source{};
  std::string status{};
  bool attention{false};
  bool active{false};
};

static constexpr int kClockAgentLines = 3;

// Structured Wi-Fi status for the Overview/Usage top-bar icon. The bar icon
// (not the legacy "WIFI n/4" strings, which the clock page and footer keep
// using) is driven from these fields.
enum class WifiIconState {
  Unknown,          // network not surveyed yet (legacy "WIFI --")
  Connecting,       // association in progress (legacy "WIFI TRY")
  Off,              // known but disconnected (legacy "WIFI OFF")
  ConnectedNoRssi,  // associated, RSSI not in yet (legacy "WIFI OK")
  Connected,        // associated with RSSI; see wifiIconLevel 1..4
};

// Everything a clock face needs, already formatted and bounded.
struct ClockView {
  ClockStyle style{ClockStyle::Sans};
  bool hour12{false};
  bool showSeconds{true};
  bool timeValid{false};
  int year{0};
  int month{0};
  int day{0};
  int weekday{0};
  int hour{0};
  int minute{0};
  int second{0};
  bool climateValid{false};
  float temperatureC{0.0f};
  float humidityPct{0.0f};
  bool batteryValid{false};
  int batteryPercent{0};
  bool batteryLow{false};
  bool charging{false};
  bool agentConnected{false};
  std::string agentSource{"--"};
  std::string agentDetail{"IDLE"};
  std::string agentTask{};
  bool agentAttention{false};
  bool agentActive{false};
  std::array<ClockAgentLine, kClockAgentLines> agents{};
  int agentCount{0};
  std::string linkLabel{"WIFI --"};
  std::string bridgeLabel{"BRIDGE --"};
  PetMode petMode{PetMode::Sleep};
  std::string petBubble{"..."};
};

struct DisplayState {
  ScreenPage page{ScreenPage::Overview};
  PetMode petMode{PetMode::Sleep};
  std::string sourceLabel{"UNKNOWN"};
  std::string taskLine{};
  std::string statusLabel{"idle"};
  std::string statusDetail{"IDLE"};
  bool offline{true};
  bool showBatteryDetail{false};
  std::string sidebarTime{"--:--:--"};
  std::string sidebarDate{"----/--/--"};
  std::string sidebarClimate{"--"};
  std::string sidebarAgent{"--"};
  std::string sidebarTokens{"--"};
  std::string sidebarTokensLabel{"TODAY"};
  std::string sidebarTokensHint{"Today total in this workspace"};
  std::string sidebarContext{"--"};
  std::string sidebarContextLabel{"CONTEXT"};
  std::string sidebarContextHint{"Current turn tokens / model window"};
  std::string sidebarQuota{"--"};
  std::string sidebarQuotaLabel{"QUOTA"};
  std::string sidebarQuotaHint{"Remaining 5-hour and weekly limits"};
  std::string sidebarQuotaStyle{"quota"};
  std::string focusLabel{"AUTO"};
  std::string focusHint{"BOOT page  HOLD/KEY agent"};
  std::string buddyBubble{"..."};
  std::string footerMessage{"--"};
  std::string linkLabel{"WIFI --"};
  std::string networkLine{"WIFI --"};
  std::string bridgeLine{"BRIDGE --"};
  // Icon fields for the Overview/Usage top bar; filled by deriveDisplayState
  // from NetworkState via the shared rssiBars() thresholds. level 0 = no
  // signal bars, 1..4 = rssiBars() bucket for a connected RSSI.
  WifiIconState wifiIconState{WifiIconState::Unknown};
  int wifiIconLevel{0};
  ClockView clock{};
};

inline bool shouldShowBatteryDetail(const PowerState& powerState) {
  return powerState.charging || powerState.lowBattery;
}

inline ScreenPage nextScreenPage(ScreenPage page) {
  switch (page) {
    case ScreenPage::Overview:
      return ScreenPage::Usage;
    case ScreenPage::Usage:
      return ScreenPage::Clock;
    default:
      return ScreenPage::Overview;
  }
}

inline int screenPageNumber(ScreenPage page) {
  return static_cast<int>(page) + 1;
}
