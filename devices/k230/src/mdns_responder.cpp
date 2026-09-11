#include "mdns_responder.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>
#include <string_view>
#include <utility>

#include "../third_party/mdns.h"

namespace reconclave {

namespace {

constexpr std::size_t kBufferCapacity = 2048;

struct Records {
  mdns_record_t ptr{};
  mdns_record_t srv{};
  mdns_record_t a{};
  mdns_record_t txt{};
};

int queryCallback(int sock, const struct sockaddr* from, std::size_t addrlen,
                   mdns_entry_type_t entry, uint16_t /*query_id*/, uint16_t rtype, uint16_t rclass,
                   uint32_t /*ttl*/, const void* data, std::size_t size, std::size_t name_offset,
                   std::size_t /*name_length*/, std::size_t /*record_offset*/,
                   std::size_t /*record_length*/, void* user_data) {
  if (entry != MDNS_ENTRYTYPE_QUESTION) return 0;

  auto* bundle = static_cast<std::pair<Records*, MdnsResponder*>*>(user_data);
  Records* records = bundle->first;

  char name_buffer[256];
  std::size_t offset = name_offset;
  mdns_string_t name = mdns_string_extract(data, size, &offset, name_buffer, sizeof(name_buffer));

  const std::string_view service_name{records->ptr.name.str, records->ptr.name.length};
  const std::string_view instance_name{records->srv.name.str, records->srv.name.length};
  const std::string_view host_name{records->a.name.str, records->a.name.length};
  const std::string_view queried{name.str, name.length};

  uint16_t unicast = rclass & MDNS_UNICAST_RESPONSE;
  char send_buffer[kBufferCapacity];

  auto respond = [&](mdns_record_t answer, mdns_record_t* additional, std::size_t additional_count) {
    if (unicast) {
      mdns_query_answer_unicast(sock, from, addrlen, send_buffer, sizeof(send_buffer), 0,
                                 static_cast<mdns_record_type_t>(rtype), name.str, name.length,
                                 answer, 0, 0, additional, additional_count);
    } else {
      mdns_query_answer_multicast(sock, send_buffer, sizeof(send_buffer), answer, 0, 0, additional,
                                   additional_count);
    }
  };

  if (queried == service_name && (rtype == MDNS_RECORDTYPE_PTR || rtype == MDNS_RECORDTYPE_ANY)) {
    mdns_record_t additional[3] = {records->srv, records->a, records->txt};
    respond(records->ptr, additional, 3);
  } else if (queried == instance_name &&
             (rtype == MDNS_RECORDTYPE_SRV || rtype == MDNS_RECORDTYPE_ANY)) {
    mdns_record_t additional[2] = {records->a, records->txt};
    respond(records->srv, additional, 2);
  } else if (queried == host_name &&
             (rtype == MDNS_RECORDTYPE_A || rtype == MDNS_RECORDTYPE_ANY)) {
    mdns_record_t additional[1] = {records->txt};
    respond(records->a, additional, 1);
  }
  return 0;
}

}  // namespace

MdnsResponder::MdnsResponder() = default;

MdnsResponder::~MdnsResponder() {
  if (sock_ >= 0) close(sock_);
  delete static_cast<Records*>(records_);
  delete address_ipv4_;
  ::operator delete(buffer_);
}

bool MdnsResponder::start(const std::string& hostname, const std::string& service_type,
                           std::uint16_t port, const std::string& local_ipv4,
                           const std::string& txt_key, const std::string& txt_value) {
  sockaddr_in bind_addr{};
  bind_addr.sin_family = AF_INET;
  bind_addr.sin_addr.s_addr = INADDR_ANY;
  bind_addr.sin_port = htons(MDNS_PORT);
  sock_ = mdns_socket_open_ipv4(&bind_addr);
  if (sock_ < 0) return false;

  hostname_ = hostname;
  service_ = service_type + ".local.";
  service_instance_ = hostname_ + "." + service_;
  hostname_qualified_ = hostname_ + ".local.";
  txt_key_ = txt_key;
  txt_value_ = txt_value;
  port_ = port;

  address_ipv4_ = new sockaddr_in{};
  address_ipv4_->sin_family = AF_INET;
  address_ipv4_->sin_port = htons(port);
  if (inet_pton(AF_INET, local_ipv4.c_str(), &address_ipv4_->sin_addr) != 1) {
    return false;
  }

  buffer_ = ::operator new(kBufferCapacity);
  buffer_capacity_ = kBufferCapacity;

  auto* records = new Records{};
  mdns_string_t service_str{service_.c_str(), service_.size()};
  mdns_string_t instance_str{service_instance_.c_str(), service_instance_.size()};
  mdns_string_t host_str{hostname_qualified_.c_str(), hostname_qualified_.size()};

  records->ptr.name = service_str;
  records->ptr.type = MDNS_RECORDTYPE_PTR;
  records->ptr.data.ptr.name = instance_str;
  records->ptr.rclass = 0;
  records->ptr.ttl = 0;

  records->srv.name = instance_str;
  records->srv.type = MDNS_RECORDTYPE_SRV;
  records->srv.data.srv.name = host_str;
  records->srv.data.srv.port = port_;
  records->srv.data.srv.priority = 0;
  records->srv.data.srv.weight = 0;
  records->srv.rclass = 0;
  records->srv.ttl = 0;

  records->a.name = host_str;
  records->a.type = MDNS_RECORDTYPE_A;
  records->a.data.a.addr = *address_ipv4_;
  records->a.rclass = 0;
  records->a.ttl = 0;

  records->txt.name = instance_str;
  records->txt.type = MDNS_RECORDTYPE_TXT;
  records->txt.data.txt.key = mdns_string_t{txt_key_.c_str(), txt_key_.size()};
  records->txt.data.txt.value = mdns_string_t{txt_value_.c_str(), txt_value_.size()};
  records->txt.rclass = 0;
  records->txt.ttl = 0;

  records_ = records;

  mdns_record_t additional[3] = {records->srv, records->a, records->txt};
  mdns_announce_multicast(sock_, buffer_, buffer_capacity_, records->ptr, 0, 0, additional, 3);
  return true;
}

void MdnsResponder::handleQuery() {
  if (sock_ < 0 || records_ == nullptr) return;
  auto* records = static_cast<Records*>(records_);
  std::pair<Records*, MdnsResponder*> bundle{records, this};
  mdns_socket_listen(sock_, buffer_, buffer_capacity_, queryCallback, &bundle);
}

void MdnsResponder::goodbye() {
  if (sock_ < 0 || records_ == nullptr) return;
  auto* records = static_cast<Records*>(records_);
  mdns_record_t additional[3] = {records->srv, records->a, records->txt};
  mdns_goodbye_multicast(sock_, buffer_, buffer_capacity_, records->ptr, 0, 0, additional, 3);
}

}  // namespace reconclave
