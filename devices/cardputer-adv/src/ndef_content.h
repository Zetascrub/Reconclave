#pragma once
#include <array>
#include <cstdint>
#include <string>
#include <vector>
#include <algorithm>

namespace cap {
constexpr size_t kNdefTextLimit = 120;
// Decode the first well-known Text/URI record from an encoded Message TLV.
// Bound every field before touching it; never display a partially read record.
inline bool decodeNdefContent(const std::vector<uint8_t>& tlv, std::string& text, bool& url) {
  text.clear(); url = false;
  size_t pos = 0;
  auto available = [&](size_t count) { return pos <= tlv.size() && count <= tlv.size() - pos; };
  if (!available(2) || tlv[pos++] != 3) return false;
  size_t length = tlv[pos++];
  if (length == 255) {
    if (!available(2)) return false;
    length = (size_t(tlv[pos]) << 8) | tlv[pos + 1]; pos += 2;
  }
  if (!available(length) || length < 3 || length > 2048) return false;
  const size_t end = pos + length;
  const uint8_t flags = tlv[pos++], typeLength = tlv[pos++];
  if (!(flags & 0x80) || (flags & 0x20) || (flags & 7) != 1 || typeLength != 1) return false;
  const size_t lengthBytes = (flags & 0x10) ? 1 : 4;
  if (end - pos < lengthBytes) return false;
  size_t payload = 0;
  for (size_t i = 0; i < lengthBytes; ++i) payload = (payload << 8) | tlv[pos++];
  size_t idLength = 0;
  if (flags & 8) { if (pos == end) return false; idLength = tlv[pos++]; }
  if (end - pos < 1 + idLength) return false;
  const uint8_t type = tlv[pos++];
  pos += idLength;
  if (!payload || payload > end - pos || payload > 1024) return false;
  if ((flags & 0x40) && payload != end - pos) return false;
  const uint8_t status = tlv[pos++]; --payload;
  if (type == 'T') {
    if (status & 0xC0) return false; // UTF-16/reserved flag isn't supported by the display.
    const size_t language = status & 0x3F;
    if (language > payload) return false;
    pos += language; payload -= language;
  } else if (type == 'U') {
    static const char* prefixes[] = {"", "http://www.", "https://www.", "http://", "https://",
        "tel:", "mailto:", "ftp://anonymous:anonymous@", "ftp://ftp.", "ftps://", "sftp://",
        "smb://", "nfs://", "ftp://", "dav://", "news:", "telnet://", "imap:", "rtsp://",
        "urn:", "pop:", "sip:", "sips:", "tftp:", "btspp://", "btl2cap://", "btgoep://",
        "tcpobex://", "irdaobex://", "file://", "urn:epc:id:", "urn:epc:tag:",
        "urn:epc:pat:", "urn:epc:raw:", "urn:epc:", "urn:nfc:"};
    if (status >= sizeof(prefixes) / sizeof(prefixes[0])) return false;
    text = prefixes[status]; url = true;
  } else return false;
  text.append(reinterpret_cast<const char*>(tlv.data() + pos), payload);
  // Reject embedded NUL and control bytes that would conceal or redraw content.
  for (unsigned char c : text) if (c < 32 || c == 127) { text.clear(); return false; }
  return true;
}
inline std::vector<uint8_t> ndefRecord(const std::string& text, bool url) {
  if (text.empty() || text.size() > kNdefTextLimit) return {};
  if (url) {
    const size_t prefix = text.rfind("https://", 0) == 0 ? 8 : text.rfind("http://", 0) == 0 ? 7 : 0;
    if (!prefix || text.size() <= prefix || text.find_first_of(" \t\r\n") != std::string::npos) return {};
  }
  std::vector<uint8_t> record{0xD1, 0x01, static_cast<uint8_t>(text.size() + (url ? 1 : 3)),
      static_cast<uint8_t>(url ? 'U' : 'T')};
  if (url) record.push_back(0); // Full URI, no abbreviation.
  else record.insert(record.end(), {0x02, 'e', 'n'}); // UTF-8, language=en.
  record.insert(record.end(), text.begin(), text.end());
  return record;
}
inline std::vector<uint8_t> ndefTlv(const std::vector<uint8_t>& record) {
  if (record.empty() || record.size() > 254) return {};
  std::vector<uint8_t> tlv{0x03, static_cast<uint8_t>(record.size())};
  tlv.insert(tlv.end(), record.begin(), record.end());
  tlv.push_back(0xFE);
  return tlv;
}
inline std::array<uint8_t, 180> type2Image(const std::vector<uint8_t>& record,
                                        const std::array<uint8_t, 7>& uid) {
  std::array<uint8_t, 180> image{};
  const auto tlv = ndefTlv(record);
  if (tlv.empty() || tlv.size() > 144) return image;
  std::copy_n(uid.begin(), 3, image.begin());
  image[3] = 0x88 ^ uid[0] ^ uid[1] ^ uid[2];
  std::copy_n(uid.begin() + 3, 4, image.begin() + 4);
  image[8] = uid[3] ^ uid[4] ^ uid[5] ^ uid[6];
  image[9] = 0x48;
  image[12] = 0xE1; image[13] = 0x10; image[14] = 0x12;
  if (tlv.size() <= 144) std::copy(tlv.begin(), tlv.end(), image.begin() + 16);
  image[167] = 0xFF; // NTAG213 AUTH0: password protection disabled in this RAM image.
  return image;
}
inline std::array<uint8_t, 448> type3Image(const std::vector<uint8_t>& record,
                                        const std::array<uint8_t, 8>& idm,
                                        const std::array<uint8_t, 8>& pmm) {
  std::array<uint8_t, 448> image{};
  if (record.empty() || record.size() > 208) return image;
  image[0] = 0x10; image[1] = 4; image[2] = 1; image[4] = 13;
  image[10] = 1; // NDEF read/write flag; writes affect RAM only.
  image[11] = (record.size() >> 16) & 0xFF;
  image[12] = (record.size() >> 8) & 0xFF;
  image[13] = record.size() & 0xFF;
  uint16_t checksum = 0;
  for (size_t i = 0; i < 14; ++i) checksum += image[i];
  image[14] = checksum >> 8; image[15] = checksum & 0xFF;
  if (record.size() <= 208) std::copy(record.begin(), record.end(), image.begin() + 16);
  std::fill_n(image.begin() + 14 * 16, 16, 0xFF); // REG
  std::copy(idm.begin(), idm.end(), image.begin() + 17 * 16);
  std::copy(idm.begin(), idm.end(), image.begin() + 18 * 16);
  std::copy(pmm.begin(), pmm.end(), image.begin() + 18 * 16 + 8);
  image[20 * 16] = 0x88; image[20 * 16 + 1] = 0xB4; // Lite-S system code
  image[23 * 16] = image[23 * 16 + 1] = image[23 * 16 + 2] = 0xFF;
  image[23 * 16 + 3] = 1; image[23 * 16 + 4] = 0xFF; // NDEF system enabled
  image[24 * 16 + 1] = image[24 * 16 + 2] = 0xFF;
  return image;
}
}
