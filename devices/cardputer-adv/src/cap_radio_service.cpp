#include "cap_radio_service.h"
#include "ndef_content.h"
#include <esp_system.h>
#include <SPI.h>
#include <RadioLib.h>
#include <M5UnitUnified.h>
#include <M5UnitUnifiedNFC.h>

namespace cap {
namespace {
constexpr int kRadioCs = 5, kNfcCs = 6, kRadioGdo = 15, kRfSwitch = 13;
// Explicitly reuse the SD SPI instance; never create/reinitialize another host.
Module radioModule(kRadioCs, kRadioGdo, RADIOLIB_NC, RADIOLIB_NC, SPI);
CC1101 radio(&radioModule);
m5::unit::UnitUnified units;
m5::unit::CapCC1101NFC nfc;
m5::nfc::NFCLayerA layerA(nfc);
m5::nfc::NFCLayerB layerB(nfc);
m5::nfc::NFCLayerF layerF(nfc);
m5::nfc::EmulationLayerA emulatorA(nfc);
m5::nfc::EmulationLayerF emulatorF(nfc);
bool emulateActive = false, emulateFelica = false;
String targetUid, emulatedUid;
std::array<uint8_t, 180> memoryA{};
std::array<uint8_t, 448> memoryF{};
uint32_t lastEmulationUiMs = 0;
// Inventory is enough to identify ISO15693 tags. The driver's full detect()
// also requires optional Get System Information, excluding some valid tags.
class InventoryLayerV : public m5::nfc::NFCLayerV {
 public:
  using NFCLayerV::NFCLayerV;
  bool inventory(m5::nfc::v::PICC& picc) { return detect_single(picc); }
};
InventoryLayerV layerV(nfc);
bool nfcActive = false;
unsigned nfcPhase = 0;
unsigned filter = 0;
constexpr const char* filterNames[] = {"Auto A/B/F/V", "NFC-A", "NFC-B", "FeliCa", "ISO15693"};
bool phaseAllowed(unsigned phase) {
  return filter == 0 || (filter == 1 && phase == 0) ||
      (filter == 2 && phase == 1) || (filter == 3 && (phase == 2 || phase == 3)) ||
      (filter == 4 && phase == 4);
}
uint32_t lastNfcPoll = 0;
constexpr m5::nfc::NFC modes[] = {m5::nfc::NFC::A, m5::nfc::NFC::B,
    m5::nfc::NFC::F, m5::nfc::NFC::F, m5::nfc::NFC::V};
constexpr const char* modeNames[] = {"NFC-A", "NFC-B", "FeliCa 212", "FeliCa 424", "ISO15693"};
bool nfcAdded = false, nfcReady = false, radioReady = false, active = false;
unsigned selected = 1;
uint32_t lastSample = 0;
String message = "Cap ready to probe";
std::vector<Tag> observations;
Band bands[] = {{315.0f}, {433.92f}, {868.0f}, {915.0f}};
}
void deselect() {
  for (int pin : {kRadioCs, kNfcCs}) {
    pinMode(pin, OUTPUT);
    digitalWrite(pin, HIGH);
  }
}
void stop() {
  if (emulateActive) {
    if (emulateFelica) emulatorF.end(); else emulatorA.end();
    emulateActive = false;
    message = "Emulation stopped";
  }
  if (radioReady) radio.standby();
  if (active) message = "Receiver stopped";
  active = false;
  if (nfcActive) message = "NFC scanning stopped";
  nfcActive = false;
  if (nfcReady) nfc.writeOperationControl(0); // RF field and oscillator off.
}
bool scanNfc() {
  stop();
  deselect();
  if (!nfcAdded) {
    // Poll IRQ status through SPI. This avoids relying on a shared GPIO ISR
    // service or an interrupt edge that happened before the handler ran.
    auto cfg = nfc.config();
    cfg.using_irq = false;
    cfg.mode = m5::nfc::NFC::A;
    nfc.config(cfg);
    nfcAdded = units.add(nfc, SPI, SPISettings(5000000, MSBFIRST, SPI_MODE1));
  }
  if (!nfcAdded) {
    message = "NFC SPI setup failed";
    return false;
  }
  // Restore reader mode after emulation; configuration is applied at begin.
  auto readerConfig = nfc.config();
  readerConfig.emulation = false;
  readerConfig.mode = m5::nfc::NFC::A;
  nfc.config(readerConfig);
  uint8_t type = 0, revision = 0;
  bool chipFound = false;
  for (unsigned retry = 0; retry < 3 && !chipFound; ++retry) {
    chipFound = nfc.readICIdentity(type, revision) && type == 0x05 && revision != 0;
    if (!chipFound) delay(20);
  }
  if (!chipFound) {
    message = "NFC chip not found; check cap";
    Serial.printf("[cap] NFC identity type=%02X rev=%02X\n", type, revision);
    nfcReady = false;
    return false;
  }
  nfcReady = units.begin();
  if (!nfcReady) {
    nfc.writeOperationControl(0);
    message = "NFC init failed (chip found)";
    Serial.println("[cap] ST25R3916 detected, RF initialization failed");
    return false;
  }
  nfcActive = true;
  nfcPhase = 0;
  while (!phaseAllowed(nfcPhase)) ++nfcPhase;
  lastNfcPoll = millis();
  message = String("Scanning ") + filterNames[filter];
  return true;
}

namespace {
void remember(const String& uid, const String& type, const String& protocol, const String& detail) {
  for (auto& tag : observations) {
    if (tag.uid == uid && tag.protocol == protocol) {
      tag.detail = detail;
      return;
    }
  }
  // Keep the most recent 32 unique observations rather than silently refusing
  // every new tag once the history fills up.
  if (observations.size() >= 32) observations.erase(observations.begin());
  observations.push_back({uid, type, protocol, detail});
}
bool pollNfc() {
  if (!nfcActive || millis() - lastNfcPoll < 100) return false;
  // Keep the oscillator running while switching protocols, but drop the field
  // long enough for a tag put in HALT/QUIET by the previous pass to reset.
  bool ok = nfc.writeOperationControl(0x80);
  delay(6);
  ok = ok && nfc.configureNFCMode(modes[nfcPhase]);
  if (ok && nfcPhase == 3) {
    using m5::nfc::Bitrate;
    ok = nfc.writeBitrate(Bitrate::Bps424K, Bitrate::Bps424K);
  }
  uint8_t operation = 0;
  ok = ok && nfc.readOperationControl(operation) && (operation & 0xC8) == 0xC8;
  if (!ok) {
    const String failed = String("NFC RF setup failed: ") + modeNames[nfcPhase];
    stop();
    message = failed;
    return true;
  }
  delay(5); // Allow newly powered tags to become ready before polling.
  units.update();
  bool detected = false;
  if (nfcPhase == 0) {
    m5::nfc::a::PICC picc;
    // Preserve successful identification even if a subsequent HALT is missed.
    bool activated = false;
    if (layerA.wakeup(picc.atqa)) activated = layerA.select(picc);
    // The driver fills size only after a complete anticollision/SELECT. Keep
    // that UID even if an ISO-DEP tag's optional ATS negotiation then fails.
    detected = picc.size == 4 || picc.size == 7 || picc.size == 10;
    if (detected) {
      char detail[32];
      snprintf(detail, sizeof(detail), "ATQA %04X SAK %02X", picc.atqa, picc.sak);
      remember(picc.uidAsString().c_str(), picc.typeAsString().c_str(), "NFC-A", detail);
      if (activated) layerA.deactivate();
    }
  } else if (nfcPhase == 1) {
    m5::nfc::b::PICC picc;
    detected = layerB.detect(picc, 0x00, 60);
    if (detected) remember(picc.pupiAsString().c_str(), "ISO14443-B", "NFC-B", "PUPI (may be random)");
  } else if (nfcPhase == 2 || nfcPhase == 3) {
    m5::nfc::f::PICC picc;
    detected = layerF.polling(picc, m5::nfc::f::system_code_wildcard,
        m5::nfc::f::RequestCode::None, m5::nfc::f::TimeSlot::Slot1);
    if (detected) remember(picc.idmAsString().c_str(), "FeliCa", "NFC-F",
        String(nfcPhase == 2 ? "212k PMm " : "424k PMm ") + picc.pmmAsString().c_str());
  } else {
    m5::nfc::v::PICC picc;
    detected = layerV.inventory(picc);
    if (detected) {
      char detail[24];
      snprintf(detail, sizeof(detail), "ISO15693 DSFID %02X", picc.dsfID);
      remember(picc.uidAsString().c_str(), "ISO15693", "NFC-V", detail);
    }
  }
  if (detected) message = String("Detected ") + modeNames[nfcPhase];
  else if (observations.empty()) message = String("Scanning ") + filterNames[filter];
  do { nfcPhase = (nfcPhase + 1) % 5; } while (!phaseAllowed(nfcPhase));
  lastNfcPoll = millis();
  return true;
}
}

bool startRadio(unsigned index) {
  if (index >= 4) return false;
  stop();
  deselect();
  selected = index;
  int16_t result = radio.begin(bands[index].mhz);
  radioReady = result == RADIOLIB_ERR_NONE;
  if (radioReady) {
    pinMode(kRfSwitch, OUTPUT);
    digitalWrite(kRfSwitch, index >= 2 ? HIGH : LOW);
    // RF_SW1 is driven by CC1101 GDO2, not by an ESP32 GPIO.
    result = radio.setDIOMapping(2, RADIOLIB_CC1101_GDOX_HW_TO_0 |
        (index == 0 ? RADIOLIB_CC1101_GDO2_NORM : RADIOLIB_CC1101_GDO2_INV));
    // Async direct RX leaves GDO2 dedicated to the band switch. Synchronous
    // direct RX would overwrite GDO2 with serial data and switch RF paths.
    if (result == RADIOLIB_ERR_NONE) result = radio.receiveDirectAsync();
  }
  active = result == RADIOLIB_ERR_NONE;
  if (!active) {
    if (radioReady) radio.standby();
    message = "CC1101 unavailable: " + String(result);
    return false;
  }
  message = "Receiving channel energy";
  lastSample = millis();
  return true;
}
bool update() {
  if (emulateActive) {
    units.update();
    if (emulateFelica) emulatorF.update(); else emulatorA.update();
    if (millis() - lastEmulationUiMs < 150) return false;
    lastEmulationUiMs = millis();
    static const char* statesA[] = {"Stopped", "Waiting for reader", "Idle", "Ready", "Reader connected", "Halted"};
    static const char* statesF[] = {"Stopped", "Waiting for reader", "Reader connected", "Selected"};
    const unsigned state = emulateFelica ? static_cast<unsigned>(emulatorF.state()) : static_cast<unsigned>(emulatorA.state());
    const char* nextMessage = emulateFelica ? statesF[state < 4 ? state : 0] : statesA[state < 6 ? state : 0];
    if (message == nextMessage) return false;
    message = nextMessage;
    return true;
  }
  if (nfcActive) return pollNfc();
  if (!active || millis() - lastSample < 100) return false;
  lastSample = millis();
  auto& value = bands[selected];
  const float rssi = radio.getRSSI();
  if (!value.history.push(rssi, lastSample)) return false;
  value.rssi = rssi;
  if (value.samples == 0 || value.rssi > value.peak) value.peak = value.rssi;
  ++value.samples;
  return true;
}
const String& status() { return message; }
const std::vector<Tag>& tags() { return observations; }
const Band& band(unsigned index) { return bands[index < 4 ? index : 0]; }
unsigned selectedBand() { return selected; }
bool receiving() { return active; }
bool scanningNfc() { return nfcActive; }
unsigned nfcFilter() { return filter; }
const char* nfcFilterName() { return filterNames[filter]; }
void cycleNfcFilter(bool forward) {
  const bool restart = nfcActive;
  stop();
  filter = (filter + (forward ? 1 : 4)) % 5;
  if (restart) scanNfc();
  else message = String("NFC filter: ") + filterNames[filter];
}
void clearTags() { observations.clear(); message = "NFC history cleared"; }
void adjustRadioThreshold(bool increase) {
  auto& threshold = bands[selected].threshold;
  threshold += increase ? 5 : -5;
  if (threshold < -120) threshold = -120;
  if (threshold > -30) threshold = -30;
}
void clearRadioHistory() {
  auto& value = bands[selected];
  value.history.clear();
  value.samples = 0;
  value.rssi = value.peak = -150;
  message = "RF history cleared";
}
namespace {
bool selectWritableTag(m5::nfc::a::PICC& picc, size_t bytes) {
  if (!scanNfc()) return false;
  nfcActive = false;
  // Fresh initialization uses NFC-A regardless of the discovery filter.
  if (!layerA.wakeup(picc.atqa) || !layerA.select(picc)) {
    message = "No Type 2 tag; hold tag still";
    stop();
    return false;
  }
  if (!picc.isMifareUltralight() && !picc.isNTAG2()) {
    message = "Writing needs a Type 2 tag";
    stop();
    return false;
  }
  if (!layerA.identify(picc) || !layerA.reactivate(picc)) {
    message = "Cannot identify writable tag";
    stop();
    return false;
  }
  // Restrict to the known Type 2 families; no key/configuration/format writes.
  using Type = m5::nfc::a::Type;
  if (picc.type != Type::NTAG_213 && picc.type != Type::NTAG_215 &&
      picc.type != Type::NTAG_216 && picc.type != Type::MIFARE_Ultralight) {
    message = "Use NTAG213/215/216/Ultralight";
    stop();
    return false;
  }
  uint8_t header[16]{};
  bool valid = false;
  if (!layerA.read16(header, 0) ||
      header[12] != 0xE1 || (header[15] & 0x0F) != 0 ||
      !layerA.ndefIsValidFormat(valid) || !valid) {
    message = "Needs writable NDEF format";
    stop();
    return false;
  }
  if (bytes > static_cast<size_t>(header[14]) * 8 || bytes > picc.userAreaSize()) {
    message = "Content exceeds tag capacity";
    stop();
    return false;
  }
  return true;
}
}
const String& writeTarget() { return targetUid; }
bool readNdefContent(String& text, bool& url, String& uid) {
  text = ""; uid = ""; url = false;
  if (!scanNfc()) return false;
  nfcActive = false;
  m5::nfc::a::PICC picc;
  bool ok = layerA.wakeup(picc.atqa) && layerA.select(picc);
  ok = ok && (picc.isMifareUltralight() || picc.isNTAG2());
  ok = ok && layerA.identify(picc) && layerA.reactivate(picc);
  if (!ok) { stop(); message = "Hold a Type 2 NDEF tag on cap"; return false; }
  uid = picc.uidAsString().c_str();
  m5::nfc::ndef::TLV value;
  ok = layerA.ndefRead(value);
  layerA.deactivate();
  stop();
  if (!ok || value.required() > 2054) { message = "NDEF unavailable or too large"; return false; }
  std::vector<uint8_t> bytes(value.required());
  std::string decoded;
  ok = value.encode(bytes.data(), bytes.size()) && decodeNdefContent(bytes, decoded, url);
  if (!ok) { message = "First record isn't UTF-8 text/URI"; return false; }
  text = decoded.c_str();
  message = url ? "URI / first NDEF record" : "Text / first NDEF record";
  return true;
}
bool prepareNdefWrite(const String& text, bool url) {
  targetUid = "";
  const auto record = ndefRecord(text.c_str(), url);
  if (record.empty()) { message = "Enter text or http(s) URL"; return false; }
  m5::nfc::a::PICC picc;
  if (!selectWritableTag(picc, record.size() + 3)) return false;
  targetUid = picc.uidAsString().c_str();
  layerA.deactivate();
  stop();
  message = "Ready: overwrite NDEF content";
  return true;
}
bool writeNdef(const String& text, bool url) {
  const String expected = targetUid;
  targetUid = ""; // Confirmation is single use, including failed attempts.
  const auto bytes = ndefTlv(ndefRecord(text.c_str(), url));
  if (expected.isEmpty() || bytes.empty()) { message = "Prepare a target before writing"; return false; }
  m5::nfc::a::PICC picc;
  if (!selectWritableTag(picc, bytes.size())) return false;
  if (expected != picc.uidAsString().c_str()) {
    stop();
    message = "Different tag; write cancelled";
    return false;
  }
  m5::nfc::ndef::TLV wanted;
  bool ok = wanted.decode(bytes.data(), bytes.size()) != 0 && layerA.ndefWrite(wanted);
  m5::nfc::ndef::TLV actual;
  bool verified = ok && layerA.ndefRead(actual);
  std::vector<uint8_t> wantedBytes(wanted.required()), actualBytes(actual.required());
  if (verified) {
    verified = wanted.encode(wantedBytes.data(), wantedBytes.size()) != 0 &&
        actual.encode(actualBytes.data(), actualBytes.size()) != 0 && wantedBytes == actualBytes;
  }
  layerA.deactivate();
  stop();
  message = verified ? "NDEF written and verified" : ok ? "Written; verification failed" : "Write failed; tag may be partial";
  return verified;
}
bool emulating() { return emulateActive; }
const String& emulatedIdentifier() { return emulatedUid; }
bool startEmulation(const String& text, bool url, bool felica) {
  const auto record = ndefRecord(text.c_str(), url);
  if (record.empty()) { message = "Enter text or http(s) URL"; return false; }
  if (!scanNfc()) return false; // Assign/probe the shared SPI device.
  stop();
  auto config = nfc.config();
  config.emulation = true;
  config.mode = felica ? m5::nfc::NFC::F : m5::nfc::NFC::A;
  nfc.config(config);
  // The driver caches its protocol. Force a transition after reset, including
  // repeated reader/emulator sessions that use the same A or F protocol.
  nfcReady = units.begin() &&
      nfc.configureEmulationMode(felica ? m5::nfc::NFC::A : m5::nfc::NFC::F) &&
      nfc.configureEmulationMode(config.mode);
  if (!nfcReady) { nfc.writeOperationControl(0); message = "Emulation setup failed"; return false; }
  bool started = false;
  if (felica) {
    std::array<uint8_t, 8> idm{};
    esp_fill_random(idm.data(), idm.size());
    idm[0] = 0x02; idm[1] = 0xFE;
    const std::array<uint8_t, 8> pmm{0x00,0xF1,0x00,0x00,0x00,0x01,0x43,0x00};
    memoryF = type3Image(record, idm, pmm);
    m5::nfc::f::PICC picc;
    started = picc.emulate(m5::nfc::f::Type::FeliCaLiteS, idm.data(), pmm.data()) &&
        emulatorF.begin(picc, memoryF.data(), memoryF.size());
    emulatedUid = picc.idmAsString().c_str();
  } else {
    std::array<uint8_t, 7> uid{};
    esp_fill_random(uid.data(), uid.size());
    uid[0] = 0x04;
    memoryA = type2Image(record, uid);
    m5::nfc::a::PICC picc;
    started = picc.emulate(m5::nfc::a::Type::NTAG_213, uid.data(), uid.size()) &&
        emulatorA.begin(picc, memoryA.data(), memoryA.size());
    emulatedUid = picc.uidAsString().c_str();
  }
  if (!started) {
    if (felica) emulatorF.end(); else emulatorA.end();
    stop();
    message = "Emulation failed to start";
    return false;
  }
  emulateFelica = felica;
  emulateActive = true;
  lastEmulationUiMs = millis();
  message = "Waiting for reader";
  return true;
}

}
