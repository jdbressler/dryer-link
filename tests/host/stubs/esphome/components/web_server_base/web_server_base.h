#pragma once
// Just enough of ESPHome's ESP-IDF web server to run a handler against a request the simulator made up.
#include <cstring>
#include <map>
#include <span>
#include <string>
#include <utility>
#include <vector>
enum http_method { HTTP_GET = 1, HTTP_POST = 3 };
namespace esphome {
class StringRef {
 public:
  StringRef(const char *s, size_t len) : s_(s), len_(len) {}
  const char *c_str() const { return s_; }
  size_t size() const { return len_; }

 private:
  const char *s_;
  size_t len_;
};
inline bool operator==(const StringRef &lhs, const char *rhs) { return strlen(rhs) == lhs.size() && memcmp(lhs.c_str(), rhs, lhs.size()) == 0; }
inline bool operator!=(const StringRef &lhs, const char *rhs) { return !(lhs == rhs); }
namespace web_server_idf {
class AsyncWebServerResponse {
 public:
  void addHeader(const char *name, const char *value) { headers.emplace_back(name, value); }
  int code{200};
  std::string content_type;
  std::string body;
  std::vector<std::pair<std::string, std::string>> headers;
};
class AsyncWebServerRequest {
 public:
  static constexpr size_t URL_BUF_SIZE = 513;
  http_method method() const { return method_; }
  StringRef url_to(std::span<char, URL_BUF_SIZE> buffer) const {
    size_t n = url_.size() < URL_BUF_SIZE - 1 ? url_.size() : URL_BUF_SIZE - 1;
    memcpy(buffer.data(), url_.data(), n);
    buffer[n] = '\0';
    return StringRef(buffer.data(), n);
  }
  std::string arg(const char *name) {
    auto it = args_.find(name);
    return it == args_.end() ? std::string() : it->second;
  }
  AsyncWebServerResponse *beginResponse(int code, const char *content_type, const std::string &content) {
    response.code = code;
    response.content_type = content_type;
    response.body = content;
    return &response;
  }
  AsyncWebServerResponse *beginResponse(int code, const char *content_type, const uint8_t *data, size_t size) {
    response.code = code;
    response.content_type = content_type;
    response.body.assign((const char *) data, size);
    return &response;
  }
  void send(AsyncWebServerResponse *r) { sent = true; }
  void send(int code, const char *content_type = nullptr, const char *content = nullptr) {
    response.code = code;
    response.content_type = content_type != nullptr ? content_type : "";
    response.body = content != nullptr ? content : "";
    sent = true;
  }
  http_method method_{HTTP_GET};
  std::string url_;
  std::map<std::string, std::string> args_;
  AsyncWebServerResponse response;
  bool sent{false};
};
class AsyncWebHandler {
 public:
  virtual ~AsyncWebHandler() = default;
  virtual bool canHandle(AsyncWebServerRequest *request) const { return false; }
  virtual void handleRequest(AsyncWebServerRequest *request) {}
};
}  // namespace web_server_idf
namespace web_server_base {
class WebServerBase {
 public:
  void add_handler_without_auth(web_server_idf::AsyncWebHandler *handler) { handlers.push_back(handler); }
  // As in ESPHome: the pointers are kept, nothing is copied. No user name means the status page asks for no login.
  void set_auth_username(const char *auth_username) { auth_username_ = auth_username; }
  void set_auth_password(const char *auth_password) { auth_password_ = auth_password; }
  std::vector<web_server_idf::AsyncWebHandler *> handlers;
  const char *auth_username_{"admin"};  // the placeholder login from the configuration
  const char *auth_password_{"replaced-at-start-up"};
};
}  // namespace web_server_base
}  // namespace esphome
using namespace esphome::web_server_idf;  // NOLINT
