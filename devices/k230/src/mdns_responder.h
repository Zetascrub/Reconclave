// mDNS/DNS-SD responder for the K230's Reconclave service announcement.
//
// Wraps third_party/mdns.h (mjansson/mdns) into a small IPv4-only class
// that answers the exact queries tools/desktop-node/desktop_app.py's
// zeroconf.ServiceBrowser issues when discovering `_reconclave._tcp.local.`
// nodes. Adapted from that library's own responder example (mdns.c
// service_callback/service_mdns), trimmed to IPv4 and a single TXT record
// since this device is single-homed and only needs to carry its node ID.
#pragma once

#include <cstdint>
#include <string>

struct sockaddr_in;

namespace reconclave {

class MdnsResponder {
 public:
  MdnsResponder();
  ~MdnsResponder();

  MdnsResponder(const MdnsResponder&) = delete;
  MdnsResponder& operator=(const MdnsResponder&) = delete;

  // Opens the mDNS multicast socket, builds the PTR/SRV/A/TXT record set
  // for `hostname` advertising `service_type` (e.g. "_reconclave._tcp")
  // on `port` at `local_ipv4`, and sends the startup announcement. TXT
  // carries a single `txt_key=txt_value` pair (e.g. "device_id=rc-k230-01").
  // Returns false on failure.
  bool start(const std::string& hostname, const std::string& service_type, std::uint16_t port,
             const std::string& local_ipv4, const std::string& txt_key,
             const std::string& txt_value);

  int socketFd() const { return sock_; }

  // Reads and answers one pending query, if any. Call when select()/poll()
  // reports socketFd() as readable.
  void handleQuery();

  // Sends a goodbye (TTL 0) packet so browsers drop this instance promptly
  // rather than waiting out the announced TTL.
  void goodbye();

 private:
  int sock_ = -1;
  void* buffer_ = nullptr;
  std::size_t buffer_capacity_ = 0;

  // Backing storage for the non-owning mdns_string_t views the records
  // below point into; must outlive every announce/answer call.
  std::string service_;            // "_reconclave._tcp.local."
  std::string hostname_;
  std::string service_instance_;   // "<hostname>._reconclave._tcp.local."
  std::string hostname_qualified_; // "<hostname>.local."
  std::string txt_key_;
  std::string txt_value_;
  sockaddr_in* address_ipv4_ = nullptr;
  std::uint16_t port_ = 0;

  // Opaque storage for the mdns_record_t set (PTR/SRV/A/TXT), built once in
  // start() and reused for every announce/answer/goodbye. Type-erased here
  // so this header does not need to include third_party/mdns.h.
  void* records_ = nullptr;
};

}  // namespace reconclave
