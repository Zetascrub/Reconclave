#include "http_server.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>

namespace reconclave {

namespace {

// Matches reconclave::kMaxPayloadBytes (common/protocol/include/reconclave/protocol.h);
// duplicated as a plain constant here rather than an include, since this
// bound is a wire-protocol property applying to any transport, not
// something that should require pulling the protocol library into a
// transport-layer header.
constexpr std::size_t kMaxBodyBytes = 16 * 1024;
constexpr std::size_t kMaxHeaderBytes = 8 * 1024;

const char* statusText(int status) {
  switch (status) {
    case 200: return "OK";
    case 400: return "Bad Request";
    case 404: return "Not Found";
    case 413: return "Payload Too Large";
    default: return "Internal Server Error";
  }
}

// Reads from `fd` until `terminator` has been seen in the accumulated
// buffer or `limit` bytes have been read. Returns false on a read error,
// closed connection before the terminator appeared, or exceeding `limit`.
bool readUntil(int fd, const std::string& terminator, std::size_t limit, std::string& buffer) {
  char chunk[1024];
  while (buffer.find(terminator) == std::string::npos) {
    if (buffer.size() >= limit) return false;
    ssize_t got = recv(fd, chunk, sizeof(chunk), 0);
    if (got <= 0) return false;
    buffer.append(chunk, static_cast<std::size_t>(got));
  }
  return true;
}

bool readExactly(int fd, std::size_t count, std::string& out) {
  out.clear();
  out.reserve(count);
  char chunk[1024];
  while (out.size() < count) {
    std::size_t want = std::min(sizeof(chunk), count - out.size());
    ssize_t got = recv(fd, chunk, want, 0);
    if (got <= 0) return false;
    out.append(chunk, static_cast<std::size_t>(got));
  }
  return true;
}

bool writeAll(int fd, const std::string& data) {
  std::size_t sent = 0;
  while (sent < data.size()) {
    ssize_t n = send(fd, data.data() + sent, data.size() - sent, 0);
    if (n <= 0) return false;
    sent += static_cast<std::size_t>(n);
  }
  return true;
}

}  // namespace

HttpServer::~HttpServer() {
  if (listen_fd_ >= 0) close(listen_fd_);
}

bool HttpServer::listen(std::uint16_t port) {
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) return false;

  int reuse = 1;
  setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = INADDR_ANY;
  addr.sin_port = htons(port);

  if (bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
    close(fd);
    return false;
  }
  if (::listen(fd, 8) != 0) {
    close(fd);
    return false;
  }

  listen_fd_ = fd;
  return true;
}

bool HttpServer::acceptAndHandle(const HttpHandler& handler) {
  int client_fd = accept(listen_fd_, nullptr, nullptr);
  if (client_fd < 0) return false;

  // Every response here is a single small JSON document with Connection:
  // close, so Nagle's algorithm just adds latency for no coalescing
  // benefit.
  int one = 1;
  setsockopt(client_fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

  std::string buffer;
  HttpResponse response;
  bool have_request = readUntil(client_fd, "\r\n\r\n", kMaxHeaderBytes, buffer);

  if (have_request) {
    std::size_t header_end = buffer.find("\r\n\r\n");
    std::string head = buffer.substr(0, header_end);
    std::string leftover = buffer.substr(header_end + 4);

    std::size_t line_end = head.find("\r\n");
    std::string request_line = head.substr(0, line_end == std::string::npos ? head.size() : line_end);

    HttpRequest request;
    std::size_t method_end = request_line.find(' ');
    std::size_t path_end = method_end == std::string::npos
                                ? std::string::npos
                                : request_line.find(' ', method_end + 1);
    if (method_end != std::string::npos && path_end != std::string::npos) {
      request.method = request_line.substr(0, method_end);
      request.path = request_line.substr(method_end + 1, path_end - method_end - 1);
    }

    std::size_t content_length = 0;
    bool has_content_length = false;
    std::size_t pos = line_end == std::string::npos ? head.size() : line_end + 2;
    while (pos < head.size()) {
      std::size_t next = head.find("\r\n", pos);
      std::string line = head.substr(pos, next == std::string::npos ? std::string::npos : next - pos);
      std::size_t colon = line.find(':');
      if (colon != std::string::npos) {
        std::string name = line.substr(0, colon);
        for (auto& c : name) c = static_cast<char>(::tolower(static_cast<unsigned char>(c)));
        if (name == "content-length") {
          std::string value = line.substr(colon + 1);
          std::size_t first = value.find_first_not_of(" \t");
          if (first != std::string::npos) {
            content_length = static_cast<std::size_t>(std::strtoul(value.c_str() + first, nullptr, 10));
            has_content_length = true;
          }
        }
      }
      if (next == std::string::npos) break;
      pos = next + 2;
    }

    bool malformed = request.method.empty();
    if (malformed) {
      response.status = 400;
      response.body = R"({"error":"invalid_request"})";
    } else if (has_content_length && content_length > kMaxBodyBytes) {
      malformed = true;
      response.status = 413;
      response.body = R"({"error":"invalid_size"})";
    } else if (has_content_length) {
      if (leftover.size() >= content_length) {
        request.body = leftover.substr(0, content_length);
      } else {
        std::string rest;
        if (readExactly(client_fd, content_length - leftover.size(), rest)) {
          request.body = leftover + rest;
        } else {
          malformed = true;
          response.status = 400;
          response.body = R"({"error":"truncated_body"})";
        }
      }
    }

    if (!malformed) {
      response = handler(request);
    }
  } else {
    response.status = 400;
    response.body = R"({"error":"invalid_request"})";
  }

  std::string out = "HTTP/1.1 " + std::to_string(response.status) + " " +
                     statusText(response.status) + "\r\n";
  out += "Content-Type: " + response.content_type + "\r\n";
  out += "Content-Length: " + std::to_string(response.body.size()) + "\r\n";
  out += "Connection: close\r\n\r\n";
  out += response.body;

  writeAll(client_fd, out);
  close(client_fd);
  return true;
}

}  // namespace reconclave
