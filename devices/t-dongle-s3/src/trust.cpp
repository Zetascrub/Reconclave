#include "trust.h"

#include <algorithm>
#include <cstring>

#include "esp_random.h"
#include "mbedtls/md.h"

namespace reconclave {
namespace {

constexpr size_t kMaxSeenNonces = 64;
// Lease bounds are a sanity gate only; the exact lease_ms is folded into the
// HMAC canonical string, so authenticity does not depend on these values.
constexpr long kLeaseMinMs = 1000;
constexpr long kLeaseMaxMs = 60000;

void toHex(const uint8_t* bytes, size_t len, char* out) {
  static const char* d = "0123456789abcdef";
  for (size_t i = 0; i < len; ++i) {
    out[i * 2] = d[bytes[i] >> 4];
    out[i * 2 + 1] = d[bytes[i] & 0x0f];
  }
  out[len * 2] = '\0';
}

bool hexDecode(const char* hex, uint8_t* out, size_t out_len) {
  if (strlen(hex) != out_len * 2) return false;
  for (size_t i = 0; i < out_len; ++i) {
    unsigned v = 0;
    if (sscanf(hex + i * 2, "%2x", &v) != 1) return false;
    out[i] = static_cast<uint8_t>(v);
  }
  return true;
}

void hmacSha256(const uint8_t* key, size_t key_len, const uint8_t* msg, size_t msg_len,
                uint8_t out[32]) {
  const mbedtls_md_info_t* info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
  mbedtls_md_hmac(info, key, key_len, msg, msg_len, out);
}

// Constant-time compare over the fixed tag length.
bool ctEqual(const uint8_t* a, const uint8_t* b, size_t len) {
  uint8_t diff = 0;
  for (size_t i = 0; i < len; ++i) diff |= a[i] ^ b[i];
  return diff == 0;
}

}  // namespace

void Trust::begin() {
  uint8_t nonce[16];
  esp_fill_random(nonce, sizeof(nonce));
  toHex(nonce, sizeof(nonce), boot_nonce_hex_);
}

String Trust::sha256Hex(const String& text) {
  uint8_t digest[32];
  const mbedtls_md_info_t* info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
  mbedtls_md(info, reinterpret_cast<const uint8_t*>(text.c_str()), text.length(), digest);
  char hex[65];
  toHex(digest, sizeof(digest), hex);
  return String(hex);
}

// Compact JSON with object keys sorted lexicographically, matching Python's
// json.dumps(x, sort_keys=True, separators=(",", ":")). Object keys are
// restricted to [A-Za-z0-9_] (fails closed otherwise), mirroring the P4.
bool Trust::canonicalJson(JsonVariantConst value, String& out, const char* exclude_key) {
  if (value.is<JsonObjectConst>()) {
    JsonObjectConst obj = value.as<JsonObjectConst>();
    std::vector<const char*> keys;
    for (JsonPairConst kv : obj) {
      const char* k = kv.key().c_str();
      if (exclude_key != nullptr && strcmp(k, exclude_key) == 0) continue;
      for (const char* c = k; *c != '\0'; ++c) {
        const bool safe = (*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z') ||
                          (*c >= '0' && *c <= '9') || *c == '_';
        if (!safe) return false;
      }
      keys.push_back(k);
    }
    std::sort(keys.begin(), keys.end(),
              [](const char* a, const char* b) { return strcmp(a, b) < 0; });
    out += "{";
    for (size_t i = 0; i < keys.size(); ++i) {
      if (i > 0) out += ",";
      out += "\"";
      out += keys[i];
      out += "\":";
      if (!canonicalJson(obj[keys[i]], out, nullptr)) return false;
    }
    out += "}";
    return true;
  }
  if (value.is<JsonArrayConst>()) {
    out += "[";
    bool first = true;
    for (JsonVariantConst item : value.as<JsonArrayConst>()) {
      if (!first) out += ",";
      first = false;
      if (!canonicalJson(item, out, nullptr)) return false;
    }
    out += "]";
    return true;
  }
  // Leaf (null / bool / integer / string): ArduinoJson emits the same compact
  // JSON form Python does for these types.
  serializeJson(value, out);
  return true;
}

bool Trust::nonceSeenOrRecord(uint64_t nonce) {
  if (std::find(seen_nonces_.begin(), seen_nonces_.end(), nonce) != seen_nonces_.end()) {
    return true;
  }
  if (seen_nonces_.size() >= kMaxSeenNonces) seen_nonces_.erase(seen_nonces_.begin());
  seen_nonces_.push_back(nonce);
  return false;
}

const rc_provisioned_peer_t* Trust::verifyRequest(const char* source_id, const char* dest_id,
                                                  const char* request_id, const char* capability,
                                                  JsonVariantConst arguments, JsonObjectConst auth,
                                                  String& reason) {
  // Locate the provisioned peer.
  const rc_provisioned_peer_t* peer = nullptr;
  for (const auto& candidate : RC_PROVISIONED_PEERS) {
    if (strcmp(candidate.peer_id, source_id) == 0) {
      peer = &candidate;
      break;
    }
  }
  if (peer == nullptr) {
    reason = "unknown peer";
    return nullptr;
  }

  const char* tag_hex = auth["tag"] | "";
  const char* nonce_hex = auth["nonce"] | "";
  // Field is named "payload_digest" on the wire; its content is the digest of
  // the request arguments (reconclave_node.py / poe-p4 main.c).
  const char* digest_hex = auth["payload_digest"] | "";
  if (!auth["coordinator_priority"].is<int>() || !auth["lease_ms"].is<long>() ||
      tag_hex[0] == '\0' || nonce_hex[0] == '\0' || digest_hex[0] == '\0') {
    reason = "malformed auth block";
    return nullptr;
  }
  const int priority = auth["coordinator_priority"].as<int>();
  const long lease_ms = auth["lease_ms"].as<long>();
  if (priority != peer->priority || lease_ms < kLeaseMinMs || lease_ms > kLeaseMaxMs) {
    reason = "priority/lease out of bounds";
    return nullptr;
  }

  // Recompute and compare the argument digest.
  String canon;
  if (!canonicalJson(arguments, canon, nullptr)) {
    reason = "arguments not canonicalisable";
    return nullptr;
  }
  if (sha256Hex(canon) != String(digest_hex)) {
    reason = "argument digest mismatch";
    return nullptr;
  }

  // Rebuild the canonical string and compare the truncated HMAC.
  String message = String(source_id) + "|" + dest_id + "|" + request_id + "|" + capability + "|" +
                   boot_nonce_hex_ + "|" + digest_hex + "|" + nonce_hex + "|" + String(priority) +
                   "|" + String(lease_ms);
  uint8_t full[32];
  hmacSha256(peer->key, 32, reinterpret_cast<const uint8_t*>(message.c_str()), message.length(),
             full);
  uint8_t supplied[kTagBytes];
  if (!hexDecode(tag_hex, supplied, kTagBytes) || !ctEqual(full, supplied, kTagBytes)) {
    reason = "bad tag";
    return nullptr;
  }

  // Replay resistance: the request nonce must be fresh for this boot session.
  char* end = nullptr;
  const uint64_t nonce = strtoull(nonce_hex, &end, 16);
  if (end == nonce_hex || *end != '\0' || nonceSeenOrRecord(nonce)) {
    reason = "replayed or malformed nonce";
    return nullptr;
  }
  return peer;
}

}  // namespace reconclave
