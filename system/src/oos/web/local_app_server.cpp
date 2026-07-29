#include "oos/web/local_app_server.h"

#include <arpa/inet.h>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <utility>
#include <vector>

namespace oos::web {
namespace {

constexpr uint16_t kHttpPort = 8080;
constexpr size_t kMaximumRequestBytes = 16 * 1024;

bool sendAll(int socket, const void *data, size_t size) {
  const auto *bytes = static_cast<const uint8_t *>(data);
  while (size) {
    const ssize_t sent = send(socket, bytes, size, MSG_NOSIGNAL);
    if (sent < 0 && errno == EINTR)
      continue;
    if (sent <= 0)
      return false;
    bytes += sent;
    size -= static_cast<size_t>(sent);
  }
  return true;
}

std::string lower(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(), [](char value) {
    return static_cast<char>(std::tolower(static_cast<unsigned char>(value)));
  });
  return value;
}

bool originHost(const std::string &app_id, std::string &host) {
  if (app_id.empty())
    return false;
  std::string normalized;
  normalized.reserve(app_id.size() + sizeof(".localhost"));
  bool label_start = true;
  for (const unsigned char character : app_id) {
    char output = static_cast<char>(std::tolower(character));
    if (output == '_')
      output = '-';
    if ((!std::isalnum(static_cast<unsigned char>(output)) && output != '-' &&
         output != '.') ||
        (label_start && (output == '-' || output == '.')))
      return false;
    if (output == '.') {
      if (normalized.empty() || normalized.back() == '-' ||
          normalized.back() == '.')
        return false;
      label_start = true;
    } else {
      label_start = false;
    }
    normalized.push_back(output);
  }
  if (normalized.back() == '-' || normalized.back() == '.')
    return false;
  host = normalized + ".localhost";
  return true;
}

const char *contentType(const std::string &path) {
  const size_t dot = path.rfind('.');
  const std::string extension =
      dot == std::string::npos ? "" : lower(path.substr(dot));
  if (extension == ".html" || extension == ".htm")
    return "text/html; charset=utf-8";
  if (extension == ".css")
    return "text/css; charset=utf-8";
  if (extension == ".js" || extension == ".mjs")
    return "text/javascript; charset=utf-8";
  if (extension == ".json")
    return "application/json";
  if (extension == ".webmanifest")
    return "application/manifest+json";
  if (extension == ".webapp")
    return "application/x-web-app-manifest+json";
  if (extension == ".svg")
    return "image/svg+xml";
  if (extension == ".png")
    return "image/png";
  if (extension == ".jpg" || extension == ".jpeg")
    return "image/jpeg";
  if (extension == ".gif")
    return "image/gif";
  if (extension == ".ico")
    return "image/x-icon";
  if (extension == ".woff")
    return "font/woff";
  if (extension == ".woff2")
    return "font/woff2";
  if (extension == ".wasm")
    return "application/wasm";
  if (extension == ".mp3")
    return "audio/mpeg";
  if (extension == ".ogg")
    return "audio/ogg";
  return "application/octet-stream";
}

bool hexDigit(char value, uint8_t &result) {
  if (value >= '0' && value <= '9')
    result = static_cast<uint8_t>(value - '0');
  else if (value >= 'a' && value <= 'f')
    result = static_cast<uint8_t>(value - 'a' + 10);
  else if (value >= 'A' && value <= 'F')
    result = static_cast<uint8_t>(value - 'A' + 10);
  else
    return false;
  return true;
}

bool decodePath(const std::string &target, std::string &path) {
  const size_t end = target.find_first_of("?#");
  const std::string encoded = target.substr(0, end);
  if (encoded.empty() || encoded.front() != '/')
    return false;
  path.clear();
  for (size_t index = 1; index < encoded.size(); ++index) {
    if (encoded[index] != '%') {
      path.push_back(encoded[index]);
      continue;
    }
    if (index + 2 >= encoded.size())
      return false;
    uint8_t high = 0;
    uint8_t low = 0;
    if (!hexDigit(encoded[index + 1], high) ||
        !hexDigit(encoded[index + 2], low))
      return false;
    const char decoded = static_cast<char>((high << 4) | low);
    if (decoded == '\0' || decoded == '/' || decoded == '\\')
      return false;
    path.push_back(decoded);
    index += 2;
  }
  return true;
}

void sendError(int client, int status, const char *reason) {
  char body[128];
  const int body_size =
      std::snprintf(body, sizeof(body), "%d %s\n", status, reason);
  char response[512];
  const int response_size =
      std::snprintf(response, sizeof(response),
                    "HTTP/1.1 %d %s\r\n"
                    "Content-Type: text/plain; charset=utf-8\r\n"
                    "Content-Length: %d\r\nConnection: close\r\n\r\n",
                    status, reason, body_size);
  if (body_size <= 0 || response_size <= 0 ||
      static_cast<size_t>(response_size) >= sizeof(response))
    return;
  sendAll(client, response, static_cast<size_t>(response_size));
  sendAll(client, body, static_cast<size_t>(body_size));
}

bool sendHeader(int client, const char *type, uint64_t size) {
  char header[512];
  const int header_size = std::snprintf(
      header, sizeof(header),
      "HTTP/1.1 200 OK\r\nContent-Type: %s\r\nContent-Length: %llu\r\n"
      "Cache-Control: no-cache\r\nConnection: close\r\n\r\n",
      type, static_cast<unsigned long long>(size));
  return header_size > 0 && static_cast<size_t>(header_size) < sizeof(header) &&
         sendAll(client, header, static_cast<size_t>(header_size));
}

} // namespace

LocalAppServer::LocalAppServer(std::string app_id, std::string package_path,
                               std::string entrypoint)
    : app_id_(std::move(app_id)), package_path_(std::move(package_path)),
      entrypoint_(std::move(entrypoint)) {}

LocalAppServer::~LocalAppServer() { stop(); }

bool LocalAppServer::start() {
  error_.clear();
  if (listener_ >= 0)
    return true;
  if (!originHost(app_id_, host_)) {
    error_ = "application id cannot form a .localhost origin";
    return false;
  }
  origin_ = "http://" + host_ + ":" + std::to_string(kHttpPort);
  if (!archive_.open(package_path_.c_str())) {
    error_ = archive_.lastError();
    return false;
  }
  listener_ = socket(AF_INET, SOCK_STREAM, 0);
  if (listener_ < 0) {
    error_ = std::string("create HTTP listener: ") + std::strerror(errno);
    return false;
  }
  const int close_on_exec = fcntl(listener_, F_GETFD);
  if (close_on_exec >= 0)
    fcntl(listener_, F_SETFD, close_on_exec | FD_CLOEXEC);
  int reuse = 1;
  setsockopt(listener_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
  sockaddr_in address = {};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  address.sin_port = htons(kHttpPort);
  if (bind(listener_, reinterpret_cast<const sockaddr *>(&address),
           sizeof(address)) != 0 ||
      listen(listener_, 4) != 0) {
    error_ = std::string("bind HTTP app origin: ") + std::strerror(errno);
    close(listener_);
    listener_ = -1;
    return false;
  }
  stopping_ = false;
  const int thread_result =
      pthread_create(&thread_, nullptr, serveThread, this);
  if (thread_result != 0) {
    error_ =
        std::string("start HTTP app server: ") + std::strerror(thread_result);
    close(listener_);
    listener_ = -1;
    return false;
  }
  thread_started_ = true;
  return true;
}

void LocalAppServer::stop() {
  stopping_ = true;
  if (listener_ >= 0) {
    shutdown(listener_, SHUT_RDWR);
    close(listener_);
    listener_ = -1;
  }
  if (thread_started_) {
    pthread_join(thread_, nullptr);
    thread_started_ = false;
  }
  archive_.close();
}

std::string LocalAppServer::urlFor(const std::string &path) const {
  return origin_ + "/" + path;
}

void *LocalAppServer::serveThread(void *context) {
  static_cast<LocalAppServer *>(context)->serve();
  return nullptr;
}

void LocalAppServer::serve() {
  while (!stopping_) {
    const int client = accept(listener_, nullptr, nullptr);
    if (client < 0) {
      if (errno == EINTR)
        continue;
      if (!stopping_)
        std::fprintf(stderr, "OOS app HTTP accept failed: %s\n",
                     std::strerror(errno));
      break;
    }
    handleClient(client);
    close(client);
  }
}

void LocalAppServer::handleClient(int client) {
  std::string request;
  request.reserve(2048);
  char buffer[2048];
  while (request.find("\r\n\r\n") == std::string::npos &&
         request.size() < kMaximumRequestBytes) {
    const ssize_t count = recv(client, buffer, sizeof(buffer), 0);
    if (count < 0 && errno == EINTR)
      continue;
    if (count <= 0)
      return;
    request.append(buffer, static_cast<size_t>(count));
  }
  if (request.find("\r\n\r\n") == std::string::npos) {
    sendError(client, 431, "Request Header Fields Too Large");
    return;
  }
  const size_t line_end = request.find("\r\n");
  const size_t first_space = request.find(' ');
  const size_t second_space = first_space == std::string::npos
                                  ? std::string::npos
                                  : request.find(' ', first_space + 1);
  if (first_space == std::string::npos || second_space == std::string::npos ||
      second_space >= line_end) {
    sendError(client, 400, "Bad Request");
    return;
  }
  const std::string method = request.substr(0, first_space);
  const std::string target =
      request.substr(first_space + 1, second_space - first_space - 1);
  const std::string protocol =
      request.substr(second_space + 1, line_end - second_space - 1);
  const bool head = method == "HEAD";
  if ((!head && method != "GET") || protocol.rfind("HTTP/", 0) != 0) {
    sendError(client, 405, "Method Not Allowed");
    return;
  }
  std::string request_host;
  for (size_t start = line_end + 2; start < request.size();) {
    const size_t end = request.find("\r\n", start);
    if (end == std::string::npos || end == start)
      break;
    const std::string line = request.substr(start, end - start);
    const size_t colon = line.find(':');
    if (colon != std::string::npos && lower(line.substr(0, colon)) == "host") {
      size_t value = colon + 1;
      while (value < line.size() && line[value] == ' ')
        ++value;
      request_host = lower(line.substr(value));
      const size_t port = request_host.find(':');
      if (port != std::string::npos)
        request_host.resize(port);
    }
    start = end + 2;
  }
  if (request_host != host_) {
    sendError(client, 421, "Misdirected Request");
    return;
  }
  std::string path;
  if (!decodePath(target, path)) {
    sendError(client, 400, "Bad Request");
    return;
  }
  if (path.empty())
    path = entrypoint_;
  std::vector<uint8_t> bytes;
  if (!apps::validPackagePath(path) || !archive_.read(path.c_str(), bytes)) {
    sendError(client, 404, "Not Found");
    return;
  }
  const char *type = contentType(path);
  if (!sendHeader(client, type, bytes.size()) || head)
    return;
  sendAll(client, bytes.data(), bytes.size());
}

} // namespace oos::web
