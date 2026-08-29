#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace reconclave {

inline constexpr char kProtocolVersion[] = "reconclave/1";
inline constexpr std::size_t kMaxCapabilities = 64;
inline constexpr std::size_t kMaxRoles = 4;
inline constexpr std::size_t kMaxPayloadBytes = 16 * 1024;
inline constexpr std::size_t kMaxErrorMessageBytes = 256;

enum class MessageType { Announce, Request, Response, Event, Stream, Unknown };
enum class ResponseStatus { Accepted, Ok, Rejected, Error, Unknown };
enum class EventKind { Progress, Evidence, Complete, Failed, Cancelled, Unknown };

struct Envelope {
  std::string proto;
  MessageType type{MessageType::Unknown};
  std::string message_id;
  std::string source_node;
  std::string destination_node;
  std::uint64_t timestamp_ms{0};
  std::uint64_t sequence{0};
  std::string payload_json;
};

struct NodeAnnouncement {
  std::string device_id;
  std::string device_type;
  std::string firmware;
  std::vector<std::string> roles{"node"};
  std::vector<std::string> capabilities;
  std::string status{"ready"};
};

struct CapabilityRequest {
  std::string capability;
  std::string request_id;
  std::string arguments_json{"{}"};
};

struct CapabilityResponse {
  std::string request_id;
  ResponseStatus status{ResponseStatus::Unknown};
  std::string job_id;
  std::string result_json{"{}"};
  std::string error_code;
  std::string error_message;
};

struct JobEvent {
  std::string job_id;
  EventKind kind{EventKind::Unknown};
  unsigned progress_percent{0};
  std::string data_json{"{}"};
};

struct StreamChunk {
  std::string stream_id;
  std::uint32_t chunk_index{0};
  bool final_chunk{false};
  std::string encoding{"base64"};
  std::string data;
};

struct ValidationResult {
  bool valid{true};
  std::vector<std::string> errors;

  void reject(std::string reason);
};

MessageType messageTypeFromString(const std::string& value);
const char* toString(MessageType value);
bool isValidIdentifier(const std::string& value);
bool isValidCapability(const std::string& value);
ValidationResult validate(const Envelope& value);
ValidationResult validate(const NodeAnnouncement& value);
ValidationResult validate(const CapabilityRequest& value);
ValidationResult validate(const CapabilityResponse& value);
ValidationResult validate(const JobEvent& value);
ValidationResult validate(const StreamChunk& value);
ValidationResult validate(const Envelope& envelope, const NodeAnnouncement& payload);
ValidationResult validate(const Envelope& envelope, const CapabilityRequest& payload);
ValidationResult validate(const Envelope& envelope, const CapabilityResponse& payload);
ValidationResult validate(const Envelope& envelope, const JobEvent& payload);
ValidationResult validate(const Envelope& envelope, const StreamChunk& payload);

}  // namespace reconclave
