#pragma once
#include <Arduino.h>
#include <vector>
#include "rf_history.h"

namespace cap {
struct Tag {
  String uid;
  String type;
  String protocol;
  String detail;
};
struct Band {
  float mhz;
  float rssi{-150};
  float peak{-150};
  uint32_t samples{0};
  RfHistory history{};
  int threshold{-80};
};
// Call before SD initialization; all three devices share the same SPI bus.
void deselect();
bool scanNfc(); // Starts continuous multi-protocol polling.
bool scanningNfc();
bool startRadio(unsigned band);
void stop();
bool update();
const String& status();
const std::vector<Tag>& tags();
const Band& band(unsigned index);
unsigned selectedBand();
bool receiving();
}

namespace cap {
unsigned nfcFilter();
const char* nfcFilterName();
void cycleNfcFilter(bool forward);
void clearTags();
void clearRadioHistory();
void adjustRadioThreshold(bool increase);
}

namespace cap {
bool prepareNdefWrite(const String& text, bool url);
bool readNdefContent(String& text, bool& url, String& uid);
const String& writeTarget();
bool writeNdef(const String& text, bool url);
bool startEmulation(const String& text, bool url, bool felica);
bool emulating();
const String& emulatedIdentifier();
}
