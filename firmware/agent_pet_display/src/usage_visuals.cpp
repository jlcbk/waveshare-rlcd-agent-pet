#include "usage_visuals.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

namespace {

int parsePercentAndResetAfterToken(const std::string& text,
                                   const char* token,
                                   std::string& outReset) {
  outReset.clear();
  const size_t tokenPos = text.find(token);
  if (tokenPos == std::string::npos) {
    return -1;
  }

  size_t pos = tokenPos + strlen(token);
  while (pos < text.size() && text[pos] == ' ') {
    ++pos;
  }

  int value = 0;
  bool foundDigit = false;
  while (pos < text.size() && isdigit(static_cast<unsigned char>(text[pos]))) {
    foundDigit = true;
    value = (value * 10) + (text[pos] - '0');
    ++pos;
  }
  if (!foundDigit) {
    return -1;
  }

  if (pos < text.size() && text[pos] == '%') {
    ++pos;
  }
  while (pos < text.size() && text[pos] == ' ') {
    ++pos;
  }
  if (pos < text.size() && text[pos] == '(') {
    const size_t closePos = text.find(')', pos + 1);
    if (closePos != std::string::npos && closePos > pos + 1 && (closePos - pos - 1) <= 12) {
      outReset = text.substr(pos + 1, closePos - pos - 1);
    }
  }

  return value;
}

bool parseScaledNumber(const char* str, size_t len, double& outValue) {
  while (len > 0 && isspace(static_cast<unsigned char>(*str))) {
    ++str;
    --len;
  }
  while (len > 0 && isspace(static_cast<unsigned char>(str[len - 1]))) {
    --len;
  }
  if (len == 0) {
    return false;
  }

  char buf[32];
  if (len >= sizeof(buf)) {
    return false;
  }
  memcpy(buf, str, len);
  buf[len] = '\0';

  char* endPtr = nullptr;
  const double base = strtod(buf, &endPtr);
  if (endPtr == buf || base < 0.0) {
    return false;
  }
  while (*endPtr != '\0' && isspace(static_cast<unsigned char>(*endPtr))) {
    ++endPtr;
  }
  double scale = 1.0;
  if (*endPtr == 'k' || *endPtr == 'K') {
    scale = 1e3;
    ++endPtr;
  } else if (*endPtr == 'm' || *endPtr == 'M') {
    scale = 1e6;
    ++endPtr;
  } else if (*endPtr == 'b' || *endPtr == 'B') {
    scale = 1e9;
    ++endPtr;
  }
  while (*endPtr != '\0' && isspace(static_cast<unsigned char>(*endPtr))) {
    ++endPtr;
  }
  if (*endPtr != '\0') {
    return false;
  }
  outValue = base * scale;
  return true;
}

}  // namespace

QuotaUsage parseQuotaUsage(const std::string& text) {
  QuotaUsage usage;
  usage.fiveHourPercent = parsePercentAndResetAfterToken(text, "5H", usage.fiveHourReset);
  usage.weekPercent = parsePercentAndResetAfterToken(text, "WK", usage.weekReset);
  if (usage.weekPercent < 0) {
    usage.weekPercent = parsePercentAndResetAfterToken(text, "Wk", usage.weekReset);
  }
  if (usage.weekPercent < 0) {
    usage.weekPercent = parsePercentAndResetAfterToken(text, "Week", usage.weekReset);
  }
  return usage;
}

namespace {

std::string stripParenthesizedSegments(const std::string& s) {
  std::string out;
  out.reserve(s.size());
  bool inParen = false;
  for (char c : s) {
    if (c == '(') {
      inParen = true;
      while (!out.empty() && out.back() == ' ') {
        out.pop_back();
      }
    } else if (c == ')') {
      inParen = false;
    } else if (!inParen) {
      if (c == ' ' && (!out.empty() && out.back() == ' ')) {
        continue;
      }
      out.push_back(c);
    }
  }
  while (!out.empty() && out.back() == ' ') {
    out.pop_back();
  }
  return out;
}

}  // namespace

bool quotaUsageSameIgnoringCountdown(const std::string& a, const std::string& b) {
  if (a == b) {
    return true;
  }
  return stripParenthesizedSegments(a) == stripParenthesizedSegments(b);
}

int parseContextUsagePercent(const std::string& text) {
  const size_t slashPos = text.find('/');
  if (slashPos == std::string::npos) {
    return -1;
  }
  double used = 0.0;
  double window = 0.0;
  if (!parseScaledNumber(text.c_str(), slashPos, used) ||
      !parseScaledNumber(text.c_str() + slashPos + 1, text.size() - slashPos - 1, window) ||
      window <= 0.0) {
    return -1;
  }
  int pct = static_cast<int>((used * 100.0 / window) + 0.5);
  if (pct < 0) {
    pct = 0;
  }
  if (pct > 100) {
    pct = 100;
  }
  return pct;
}

std::string expandTodayTokenUnit(const std::string& value) {
  static constexpr const char* kSuffix = " tok";
  static constexpr size_t kSuffixLen = 4;
  if (value.size() > kSuffixLen &&
      value.compare(value.size() - kSuffixLen, kSuffixLen, kSuffix) == 0) {
    return value.substr(0, value.size() - kSuffixLen) + " Tokens";
  }
  return value;
}

std::string stripTodayTokenUnit(const std::string& value) {
  static constexpr const char* const kSuffixes[] = {" Tokens", " tokens", " Token", " token", " tok", " TOK"};
  for (const char* suffix : kSuffixes) {
    const size_t len = strlen(suffix);
    if (value.size() > len && value.compare(value.size() - len, len, suffix) == 0) {
      return value.substr(0, value.size() - len);
    }
  }
  return value;
}
