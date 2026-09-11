#pragma once

#include <deque>
#include <map>
#include <string>

#include "json.h"
#include "trust_crypto.h"

namespace reconclave {

struct TrustDecision {
  bool allowed = false;
  std::string code;
  std::string message;
  std::string nonce;
};

class TrustPolicy {
 public:
  explicit TrustPolicy(std::string device_id);
  bool loadExecutionKey(const std::string& path, std::string& error);
  void setExecutionKeyForTest(const TrustKey& key) { key_ = key; configured_ = true; }
  void setBootNonceForTest(std::string nonce) { boot_nonce_ = std::move(nonce); }
  bool configured() const { return configured_; }
  const std::string& bootNonce() const { return boot_nonce_; }

  TrustDecision verifyRequest(const std::string& source_node, const std::string& request_id,
                              const std::string& capability, const json::Value& arguments,
                              const json::Value& auth);
  TrustDecision verifyDelegatedScope(const std::string& capability,
                                     const json::Value& arguments,
                                     std::uint64_t now_ms) const;
  json::Value responseAuth(const std::string& destination_node,
                           const std::string& request_id, const std::string& status,
                           const json::Value& signed_body, const std::string& nonce) const;

 private:
  TrustDecision deny(std::string code, std::string message) const;
  std::string device_id_;
  std::string boot_nonce_;
  TrustKey key_{};
  bool configured_ = false;
  std::map<std::string, std::deque<std::string>> recent_nonces_;
};

}  // namespace reconclave
