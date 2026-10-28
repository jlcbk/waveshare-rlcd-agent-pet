#pragma once
// Minimal Arduino interface stubs for compiling the REAL
// battery_protection.cpp early-wake path in the native test environment
// (same technique as the original scratch harness).
#include <stdint.h>
#define RTC_DATA_ATTR
struct FakeSerial {
  template <typename... Args> void printf(const char*, Args...) {}
  void flush() {}
};
extern FakeSerial Serial;
void delay(unsigned long ms);
