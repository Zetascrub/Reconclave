#include "reconclave/protocol.h"

#include <algorithm>
#include <cctype>
#include <unordered_set>
#include <utility>

namespace reconclave {
namespace {

bool isIdentifierCharacter(char c) {
  const auto value = static_cast<unsigned char>(c);
  return std::isalnum(value) != 0 || c == '-' || c == '_' || c == '.';
}

void requireIdentifier(ValidationResult& result, const std::string& value,
                       const char* field) {
  if (!isValidIdentifier(value)) {
    result.reject(std::string(field) + " must be a non-empty portable identifier");
  }
}

void append(ValidationResult& destination, const ValidationResult& source) {
  for (const auto& error : source.errors) destination.reject(error);
}

void requireEnvelopeType(ValidationResult& result, const Envelope& envelope,
                         MessageType expected) {
  append(result, validate(envelope));
  if (envelope.type != expected) result.reject("envelope type does not match payload");
}

}  // namespace

void ValidationResult::reject(std::string reason) {
  valid = false;
  errors.push_back(std::move(reason));
}

MessageType messageTypeFromString(const std::string& value) {
  if (value == "announce") return MessageType::Announce;
  if (value == "request") return MessageType::Request;
  if (value == "response") return MessageType::Response;
  if (value == "event") return MessageType::Event;
  if (value == "stream") return MessageType::Stream;
  return MessageType::Unknown;
}

const char* toString(MessageType value) {
  switch (value) {
    case MessageType::Announce: return "announce";
    case MessageType::Request: return "request";
    case MessageType::Response: return "response";
    case MessageType::Event: return "event";
    case MessageType::Stream: return "stream";
    case MessageType::Unknown: return "unknown";
  }
  return "unknown";
}

bool isValidIdentifier(const std::string& value) {
  return !value.empty() && value.size() <= 64 &&
         std::all_of(value.begin(), value.end(), isIdentifierCharacter);
}

bool isValidCapability(const std::string& value) {
  if (value.empty() || value.size() > 96 || value.front() == '.' || value.back() == '.') {
    return false;
  }
  bool previous_dot = false;
  for (const char c : value) {
    if (c == '.') {
      if (previous_dot) return false;
      previous_dot = true;
      continue;
    }
    previous_dot = false;
    const auto v = static_cast<unsigned char>(c);
    if (std::islower(v) == 0 && std::isdigit(v) == 0 && c != '_' && c != '-') return false;
  }
  return value.find('.') != std::string::npos;
}

ValidationResult validate(const Envelope& value) {
  ValidationResult result;
  if (value.proto != kProtocolVersion) result.reject("unsupported protocol version");
  if (value.type == MessageType::Unknown) result.reject("unknown message type");
  requireIdentifier(result, value.message_id, "message_id");
  requireIdentifier(result, value.source_node, "source_node");
  if (!value.destination_node.empty()) requireIdentifier(result, value.destination_node, "destination_node");
  if (value.timestamp_ms == 0) result.reject("timestamp_ms must be non-zero");
  if (value.sequence == 0) result.reject("sequence must be non-zero");
  if (value.payload_json.empty()) result.reject("payload must be present");
  if (value.payload_json.size() > kMaxPayloadBytes) result.reject("payload exceeds size limit");
  return result;
}

ValidationResult validate(const NodeAnnouncement& value) {
  ValidationResult result;
  requireIdentifier(result, value.device_id, "device_id");
  requireIdentifier(result, value.device_type, "device_type");
  if (value.firmware.empty() || value.firmware.size() > 32) result.reject("invalid firmware version");
  if (value.roles.empty() || value.roles.size() > kMaxRoles) result.reject("invalid role count");
  std::unordered_set<std::string> unique_roles;
  for (const auto& role : value.roles) {
    if (role != "node" && role != "coordinator" && role != "analysis") {
      result.reject("invalid role: " + role);
    }
    if (!unique_roles.insert(role).second) result.reject("duplicate role: " + role);
  }
  if (unique_roles.find("node") == unique_roles.end()) result.reject("node role is required");
  if (value.capabilities.size() > kMaxCapabilities) result.reject("too many capabilities");
  std::unordered_set<std::string> unique;
  for (const auto& capability : value.capabilities) {
    if (!isValidCapability(capability)) result.reject("invalid capability: " + capability);
    if (!unique.insert(capability).second) result.reject("duplicate capability: " + capability);
  }
  std::unordered_set<std::string> described;
  for (const auto& descriptor : value.capability_descriptors) {
    if (!isValidCapability(descriptor.id)) result.reject("invalid capability descriptor");
    if (unique.find(descriptor.id) == unique.end()) {
      result.reject("descriptor references unadvertised capability: " + descriptor.id);
    }
    if (!described.insert(descriptor.id).second) {
      result.reject("duplicate capability descriptor: " + descriptor.id);
    }
    if (descriptor.version == 0 || descriptor.version > 65535) result.reject("invalid capability version");
    if (descriptor.permission != "public" && descriptor.permission != "trusted") {
      result.reject("invalid capability permission");
    }
    if (descriptor.features.size() > kMaxCapabilityFeatures) result.reject("too many capability features");
    for (const auto& feature : descriptor.features) requireIdentifier(result, feature, "capability feature");
    if (descriptor.weight == 0 || descriptor.weight > 100) result.reject("invalid capability weight");
    if (descriptor.max_concurrency == 0 || descriptor.max_concurrency > 1024) {
      result.reject("invalid capability concurrency");
    }
  }
  if (value.status != "ready" && value.status != "busy" && value.status != "degraded") {
    result.reject("unknown node status");
  }
  return result;
}

ValidationResult validate(const CapabilityRequest& value) {
  ValidationResult result;
  if (!isValidCapability(value.capability)) result.reject("invalid capability");
  requireIdentifier(result, value.request_id, "request_id");
  if (value.arguments_json.empty()) result.reject("arguments must be present");
  if (value.arguments_json.size() > kMaxPayloadBytes) result.reject("arguments exceed size limit");
  return result;
}

ValidationResult validate(const CapabilityResponse& value) {
  ValidationResult result;
  requireIdentifier(result, value.request_id, "request_id");
  if (value.status == ResponseStatus::Unknown) result.reject("unknown response status");
  if (!value.job_id.empty() && !isValidIdentifier(value.job_id)) result.reject("invalid job_id");
  if (value.status == ResponseStatus::Accepted && value.job_id.empty()) {
    result.reject("accepted response requires job_id");
  }
  if ((value.status == ResponseStatus::Rejected || value.status == ResponseStatus::Error) &&
      value.error_code.empty()) {
    result.reject("failure response requires error_code");
  }
  if (!value.error_code.empty() && !isValidIdentifier(value.error_code)) {
    result.reject("invalid error_code");
  }
  if (value.error_message.size() > kMaxErrorMessageBytes) {
    result.reject("error_message exceeds size limit");
  }
  if (value.result_json.empty()) result.reject("result must be present");
  if (value.result_json.size() > kMaxPayloadBytes) result.reject("result exceeds size limit");
  return result;
}

ValidationResult validate(const JobEvent& value) {
  ValidationResult result;
  requireIdentifier(result, value.job_id, "job_id");
  if (value.kind == EventKind::Unknown) result.reject("unknown event kind");
  if (value.progress_percent > 100) result.reject("progress_percent exceeds 100");
  if (value.data_json.empty()) result.reject("event data must be present");
  if (value.data_json.size() > kMaxPayloadBytes) result.reject("event data exceeds size limit");
  return result;
}

ValidationResult validate(const StreamChunk& value) {
  ValidationResult result;
  requireIdentifier(result, value.stream_id, "stream_id");
  if (value.encoding != "base64") result.reject("unsupported stream encoding");
  if (value.data.empty() && !value.final_chunk) result.reject("non-final stream chunk is empty");
  if (value.data.size() > kMaxPayloadBytes) result.reject("stream chunk exceeds size limit");
  return result;
}

ValidationResult validate(const Envelope& envelope, const NodeAnnouncement& payload) {
  ValidationResult result;
  requireEnvelopeType(result, envelope, MessageType::Announce);
  append(result, validate(payload));
  if (!envelope.destination_node.empty()) result.reject("announcement must be broadcast");
  if (envelope.source_node != payload.device_id) {
    result.reject("announcement device_id must match envelope source_node");
  }
  return result;
}

ValidationResult validate(const Envelope& envelope, const CapabilityRequest& payload) {
  ValidationResult result;
  requireEnvelopeType(result, envelope, MessageType::Request);
  append(result, validate(payload));
  if (envelope.destination_node.empty()) result.reject("request requires destination_node");
  return result;
}

ValidationResult validate(const Envelope& envelope, const CapabilityResponse& payload) {
  ValidationResult result;
  requireEnvelopeType(result, envelope, MessageType::Response);
  append(result, validate(payload));
  if (envelope.destination_node.empty()) result.reject("response requires destination_node");
  return result;
}

ValidationResult validate(const Envelope& envelope, const JobEvent& payload) {
  ValidationResult result;
  requireEnvelopeType(result, envelope, MessageType::Event);
  append(result, validate(payload));
  if (envelope.destination_node.empty()) result.reject("event requires destination_node");
  return result;
}

ValidationResult validate(const Envelope& envelope, const StreamChunk& payload) {
  ValidationResult result;
  requireEnvelopeType(result, envelope, MessageType::Stream);
  append(result, validate(payload));
  if (envelope.destination_node.empty()) result.reject("stream requires destination_node");
  return result;
}

}  // namespace reconclave
