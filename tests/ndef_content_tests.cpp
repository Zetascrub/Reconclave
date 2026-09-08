#include "../devices/cardputer-adv/src/ndef_content.h"
#include <cassert>
int main() {
  using namespace cap;
  const auto record = ndefRecord("Hi", false);
  const std::vector<uint8_t> expected{0xD1,1,5,'T',2,'e','n','H','i'};
  assert(record == expected);
  const auto tlv = ndefTlv(record);
  assert(tlv.front() == 3 && tlv[1] == 9 && tlv.back() == 0xFE);
  assert(std::equal(record.begin(), record.end(), tlv.begin()+2));
  std::string decoded;
  bool isUrl = true;
  assert(decodeNdefContent(tlv, decoded, isUrl) && decoded == "Hi" && !isUrl);
  assert(decodeNdefContent(ndefTlv(ndefRecord("https://example.com", true)), decoded, isUrl));
  assert(isUrl && decoded == "https://example.com");
  const std::vector<uint8_t> compressed{3,8,0xD1,1,4,'U',4,'a','.','b',0xFE};
  assert(decodeNdefContent(compressed, decoded, isUrl) && decoded == "https://a.b");
  for (size_t i = 0; i < tlv.size() - 1; ++i) {
    assert(!decodeNdefContent(std::vector<uint8_t>(tlv.begin(), tlv.begin() + i), decoded, isUrl));
  }
  auto malformed = tlv;
  malformed[6] = 63; // Language length exceeds payload.
  assert(!decodeNdefContent(malformed, decoded, isUrl));
  malformed = tlv; malformed[2] |= 0x20; // Chunked record.
  assert(!decodeNdefContent(malformed, decoded, isUrl));
  malformed = tlv; malformed[6] |= 0x80; // UTF-16 is not rendered as UTF-8.
  assert(!decodeNdefContent(malformed, decoded, isUrl));
  malformed = tlv; malformed[4] = 255;
  assert(!decodeNdefContent(malformed, decoded, isUrl));
  assert(!decodeNdefContent(ndefTlv(ndefRecord(std::string("a\0b", 3), false)), decoded, isUrl));
  const std::vector<uint8_t> longHeader{3,0xFF,0,12,0xC1,1,0,0,0,5,'T',2,'e','n','H','i',0xFE};
  assert(decodeNdefContent(longHeader, decoded, isUrl) && decoded == "Hi");
  const auto uri = ndefRecord("https://example.com", true);
  assert(uri[3] == 'U' && uri[4] == 0 && uri[2] == 20);
  assert(ndefRecord("", false).empty());
  assert(ndefRecord(std::string(121,'x'), false).empty());
  assert(ndefRecord("example.com", true).empty());
  assert(ndefRecord("https://", true).empty());
  assert(ndefRecord("https://example.com/a b", true).empty());
  const auto maximum = ndefRecord(std::string(120, 'x'), false);
  const std::array<uint8_t,7> uid{4,1,2,3,4,5,6};
  const auto a = type2Image(maximum, uid);
  assert(a[3] == (0x88 ^ 4 ^ 1 ^ 2));
  assert(a[8] == (3 ^ 4 ^ 5 ^ 6));
  assert(a[12] == 0xE1 && a[14] * 8 == 144 && a[15] == 0);
  assert(a[16] == 3 && a[17] == maximum.size());
  assert(a[18 + maximum.size()] == 0xFE && a[167] == 0xFF);
  const auto bad = type2Image(std::vector<uint8_t>(200, 0xFF), uid);
  assert(bad[12] == 0); // Don't advertise a truncated oversized message.
  const std::array<uint8_t,8> idm{2,0xFE,1,2,3,4,5,6};
  const std::array<uint8_t,8> pmm{0,0xF1,0,0,0,1,0x43,0};
  const auto f = type3Image(maximum, idm, pmm);
  assert(f[4] == 13 && f[13] == maximum.size());
  unsigned sum = 0;
  for (unsigned i=0; i<14; ++i) sum += f[i];
  assert((unsigned(f[14]) << 8 | f[15]) == sum);
  assert(std::equal(maximum.begin(), maximum.end(), f.begin()+16));
  assert(std::equal(idm.begin(), idm.end(), f.begin()+17*16));
  assert(std::equal(pmm.begin(), pmm.end(), f.begin()+18*16+8));
  assert(f[23*16+3] == 1);
}
