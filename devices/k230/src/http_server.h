// Minimal single-connection-at-a-time HTTP/1.1 server.
//
// Matches the transport tools/desktop-node/reconclave_node.py and the P4's
// esp_http_server actually implement (plain HTTP + JSON, not WebSocket —
// see devices/k230/README.md for why). Handles exactly enough HTTP to serve
// GET /reconclave/v1/announce and POST /reconclave/v1/message: a request
// line, headers up to Content-Length, and a bounded body. No keep-alive,
// chunked transfer, or pipelining — each accepted connection is closed
// after one response, which is all this protocol's fixed two-endpoint
// contract needs.
#pragma once

#include <cstdint>
#include <functional>
#include <string>

namespace reconclave {

struct HttpRequest {
  std::string method;
  std::string path;
  std::string body;
};

struct HttpResponse {
  int status = 200;
  std::string content_type = "application/json";
  std::string body;
};

using HttpHandler = std::function<HttpResponse(const HttpRequest&)>;

class HttpServer {
 public:
  ~HttpServer();

  // Binds and listens on `port` across all interfaces. Returns false on
  // failure (errno set).
  bool listen(std::uint16_t port);

  int socketFd() const { return listen_fd_; }

  // Accepts exactly one pending connection, reads its request, invokes
  // `handler`, writes the response, and closes the connection. Intended to
  // be called when select()/poll() reports socketFd() as readable. Returns
  // false if accept() itself failed (the caller's event loop should keep
  // running regardless — a single bad connection should not take the
  // server down).
  bool acceptAndHandle(const HttpHandler& handler);

 private:
  int listen_fd_ = -1;
};

}  // namespace reconclave
