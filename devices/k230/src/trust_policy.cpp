#include "trust_policy.h"

#include <arpa/inet.h>
#include <sys/random.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <sstream>
#include <vector>

namespace reconclave { namespace {
constexpr std::size_t kRecentNonces = 64;

std::string randomNonce() {
  std::array<unsigned char, 16> bytes{};
  if (getrandom(bytes.data(), bytes.size(), 0) != static_cast<ssize_t>(bytes.size())) return {};
  char text[33]{};
  for (std::size_t i = 0; i < bytes.size(); ++i) std::snprintf(text + i * 2, 3, "%02x", bytes[i]);
  return text;
}

bool uintField(const json::Value& object, const char* name, std::uint64_t& result) {
  const auto* value = object.find(name);
  if (value == nullptr || value->type() != json::Value::Type::Number) return false;
  result = value->asUInt64();
  return true;
}

bool isLowerHex(const std::string& value, std::size_t length) {
  return value.size() == length &&
         std::all_of(value.begin(), value.end(), [](unsigned char c) {
           return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
         });
}

struct Network { std::uint32_t address=0, mask=0; };
bool parseNetwork(const std::string& text, Network& out) {
  const auto slash = text.find('/');
  const std::string address = text.substr(0, slash);
  int prefix = 32;
  if (slash != std::string::npos) {
    char* end = nullptr; prefix = static_cast<int>(std::strtol(text.c_str() + slash + 1, &end, 10));
    if (end == text.c_str() + slash + 1 || *end != '\0' || prefix < 0 || prefix > 32) return false;
  }
  in_addr parsed{}; if (inet_pton(AF_INET, address.c_str(), &parsed) != 1) return false;
  out.mask = prefix == 0 ? 0 : 0xffffffffU << (32 - prefix);
  out.address = ntohl(parsed.s_addr) & out.mask; return true;
}
bool contains(const Network& outer, const Network& inner) {
  return (inner.mask & outer.mask) == outer.mask && (inner.address & outer.mask) == outer.address;
}
bool overlaps(const Network& a, const Network& b) {
  const auto mask = a.mask & b.mask; return (a.address & mask) == (b.address & mask);
}
std::vector<Network> networks(const json::Value* array, bool& valid) {
  std::vector<Network> result; valid = array != nullptr && array->isArray();
  if (!valid) return result;
  for (const auto& item : array->items()) { Network parsed; if (!item.isString() || !parseNetwork(item.asString(), parsed)) { valid=false; return {}; } result.push_back(parsed); }
  return result;
}
}

TrustPolicy::TrustPolicy(std::string device_id) : device_id_(std::move(device_id)), boot_nonce_(randomNonce()) {}
bool TrustPolicy::loadExecutionKey(const std::string& path, std::string& error) { configured_=loadHexTrustKey(path,key_,error); return configured_; }
TrustDecision TrustPolicy::deny(std::string code,std::string message) const { return {false,std::move(code),std::move(message),""}; }

TrustDecision TrustPolicy::verifyRequest(const std::string& source,const std::string& request_id,
 const std::string& capability,const json::Value& arguments,const json::Value& auth) {
  if(!configured_||boot_nonce_.empty()) return deny("UNAUTHENTICATED","execution trust is not configured");
  if(!auth.isObject()) return deny("UNAUTHENTICATED","request is not signed");
  const auto*n=auth.find("nonce"),*d=auth.find("payload_digest"),*t=auth.find("tag");
  std::uint64_t priority=0,lease=0;
  if(!n||!d||!t||!n->isString()||!d->isString()||!t->isString()||!uintField(auth,"coordinator_priority",priority)||!uintField(auth,"lease_ms",lease)) return deny("UNAUTHENTICATED","signature fields are incomplete");
  const std::string nonce=n->asString(), digest=sha256Hex(json::canonicalStringify(arguments));
  if(!isLowerHex(nonce,16)||!isLowerHex(d->asString(),64)||d->asString()!=digest||
     !isLowerHex(t->asString(),32)||lease<1000||lease>300000||priority>1000)
    return deny("UNAUTHENTICATED","signed request metadata is invalid");
  std::ostringstream canonical; canonical<<source<<'|'<<device_id_<<'|'<<request_id<<'|'<<capability<<'|'<<boot_nonce_<<'|'<<digest<<'|'<<nonce<<'|'<<priority<<'|'<<lease;
  if(!constantTimeHexEqual(t->asString(),hmacSha256Hex(key_,canonical.str(),16))) return deny("UNAUTHENTICATED","invalid signature");
  auto&seen=recent_nonces_[source]; if(std::find(seen.begin(),seen.end(),nonce)!=seen.end()) return deny("UNAUTHENTICATED","replayed request");
  seen.push_back(nonce); if(seen.size()>kRecentNonces) seen.pop_front(); return {true,"","",nonce};
}

TrustDecision TrustPolicy::verifyDelegatedScope(const std::string& capability,const json::Value& arguments,std::uint64_t now) const {
  if(!configured_||!arguments.isObject()) return deny("SCOPE_REQUIRED","signed scope delegation is required");
  const auto*token=arguments.find("_scope_delegation"); if(!token||!token->isObject()) return deny("SCOPE_REQUIRED","signed scope delegation is required");
  const auto*tag=token->find("tag"); if(!tag||!tag->isString()||!constantTimeHexEqual(tag->asString(),hmacSha256Hex(key_,json::canonicalStringify(*token,"tag")))) return deny("SCOPE_INVALID","scope delegation signature is invalid");
  const auto*cap=token->find("capability"),*dest=token->find("destination_node"),*arg_digest=token->find("arguments_digest");
  if(!cap||!dest||!arg_digest||cap->asString()!=capability||dest->asString()!=device_id_||arg_digest->asString()!=sha256Hex(json::canonicalStringify(arguments,"_scope_delegation"))) return deny("SCOPE_INVALID","scope delegation does not match this operation");
  std::uint64_t issued=0,expires=0; if(!uintField(*token,"issued_at_ms",issued)||!uintField(*token,"expires_at_ms",expires)||issued>now||now>=expires||expires-issued>300000) return deny("SCOPE_EXPIRED","scope delegation has expired or invalid lifetime");
  bool includes_valid=false,excludes_valid=false; const auto includes=networks(token->find("included_networks"),includes_valid),excludes=networks(token->find("excluded_networks"),excludes_valid);
  if(!includes_valid||!excludes_valid||includes.empty()) return deny("SCOPE_INVALID","scope networks are invalid");
  std::vector<std::string> targets; for(const char*name:{"network","target","host"})if(const auto*v=arguments.find(name);v&&v->isString()&&!v->asString().empty())targets.push_back(v->asString());
  if(const auto*hosts=arguments.find("hosts");hosts&&hosts->isArray())for(const auto&host:hosts->items())if(host.isString())targets.push_back(host.asString());
  if(targets.empty()) return deny("SCOPE_INVALID","operation contains no explicit target");
  for(const auto&target:targets){Network parsed;if(!parseNetwork(target,parsed))return deny("SCOPE_INVALID","target is not valid IPv4");bool included=std::any_of(includes.begin(),includes.end(),[&](const auto&n){return contains(n,parsed);});bool excluded=std::any_of(excludes.begin(),excludes.end(),[&](const auto&n){return overlaps(n,parsed);});if(!included||excluded)return deny("SCOPE_DENIED","target is outside delegated scope");}
  return {true,"","",""};
}

json::Value TrustPolicy::responseAuth(const std::string& destination,const std::string& request_id,
 const std::string& status,const json::Value& body,const std::string& nonce) const {
  json::Value auth=json::Value::makeObject();if(!configured_||nonce.empty())return auth;
  const std::string digest=sha256Hex(json::canonicalStringify(body));
  std::ostringstream canonical;canonical<<device_id_<<'|'<<destination<<'|'<<request_id<<'|'<<status<<'|'<<boot_nonce_<<'|'<<digest<<'|'<<nonce;
  auth.set("nonce",json::Value::makeString(nonce));auth.set("payload_digest",json::Value::makeString(digest));
  auth.set("tag",json::Value::makeString(hmacSha256Hex(key_,canonical.str(),16)));return auth;
}
} // namespace reconclave
