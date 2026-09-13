// Provisioned-HMAC trust for the node: the receiving half of the same scheme
// the PoE-P4 implements and the desktop coordinator mints. Verifies that a
// request came from a provisioned peer, is bound to this boot session, matches
// its argument digest, and hasn't been replayed.
//
// Interop-critical and mirrored from devices/poe-p4/main/main.c + the desktop's
// reconclave_node.py:
//   digest    = sha256( canonical_json(arguments) )                     (hex)
//   canonical = source|dest|request_id|capability|boot_nonce|digest|nonce|priority|lease_ms
//   tag       = HMAC-SHA256(peer_key, canonical)[:16]                   (hex)
// canonical_json is Python's json.dumps(x, sort_keys=True, separators=(",",":")).
#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>

#include <vector>

#include "generated_trust.h"

namespace reconclave {

constexpr size_t kTagBytes = 16;  // 128-bit truncated HMAC ("hmac-sha256-128")

class Trust {
 public:
  void begin();  // generate a fresh 16-byte boot nonce for this session
  const char* bootNonceHex() const { return boot_nonce_hex_; }

  // Verifies a request's `auth` object against the provisioned peer named by
  // `source_id`. Returns the matched peer on success (and records the request
  // nonce so it can't be replayed), or nullptr with `reason` set.
  const rc_provisioned_peer_t* verifyRequest(const char* source_id, const char* dest_id,
                                             const char* request_id, const char* capability,
                                             JsonVariantConst arguments, JsonObjectConst auth,
                                             String& reason);

  // Exposed for the digest step / tests.
  static bool canonicalJson(JsonVariantConst value, String& out, const char* exclude_key);
  static String sha256Hex(const String& text);

 private:
  bool nonceSeenOrRecord(uint64_t nonce);

  char boot_nonce_hex_[33] = {0};
  std::vector<uint64_t> seen_nonces_;
};

}  // namespace reconclave
