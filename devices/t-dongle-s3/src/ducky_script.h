// A DuckyScript-subset parser/executor: the same script language family used
// by the USB Rubber Ducky and (per its web UI docs) the Malduino W, so
// payloads copied from that wider ecosystem's docs/examples port over
// directly. See devices/t-dongle-s3/README.md for the exact supported
// command list and the guardrails below.
#pragma once

#include <Arduino.h>
#include <USBHIDKeyboard.h>

namespace reconclave {

struct DuckyScriptResult {
  bool ok = false;
  int steps_run = 0;
  int line_number = 0;  // 1-based line that failed; 0 if ok or n/a.
  String error;         // Empty if ok.
};

// Bounded independent of anything the script itself claims - see the .cpp for
// why each of these exists. They bound what a *legitimate* button press can
// do; they are not themselves the reason firing requires a button press in
// the first place (that constraint lives in main.cpp/README.md and predates
// this file).
constexpr int kDuckyMaxLines = 500;
constexpr unsigned long kDuckyMaxSingleDelayMs = 60000;
constexpr unsigned long kDuckyMaxTotalRuntimeMs = 120000;
constexpr int kDuckyMaxRepeatCount = 50;

DuckyScriptResult runDuckyScript(const String& body, USBHIDKeyboard& keyboard);

}  // namespace reconclave
