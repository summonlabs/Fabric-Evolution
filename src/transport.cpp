// Fabric Evolution — real TCP transport (implementation).
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "fabric/evolution/transport.hpp"

#include <cerrno>
#include <cstdio>
#include <cstring>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace fabric::evolution {
namespace {

constexpr std::size_t kReceiveChunkBytes = 16U * 1024U;
constexpr int kBacklogDefault = 64;

[[nodiscard]] int last_socket_error() noexcept {
#ifdef _WIN32
  return WSAGetLastError();
#else
  return errno;
#endif
}

[[nodiscard]] bool would_block(int error) noexcept {
#ifdef _WIN32
  return error == WSAETIMEDOUT || error == WSAEWOULDBLOCK;
#else
  return error == EAGAIN || error == EWOULDBLOCK || error == ETIMEDOUT;
#endif
}

void close_socket(SocketHandle handle) noexcept {
  if (handle == kInvalidSocket) {
    return;
  }
#ifdef _WIN32
  ::closesocket(static_cast<SOCKET>(handle));
#else
  ::close(static_cast<int>(handle));
#endif
}

void shutdown_socket(SocketHandle handle) noexcept {
  if (handle == kInvalidSocket) {
    return;
  }
#ifdef _WIN32
  ::shutdown(static_cast<SOCKET>(handle), SD_BOTH);
#else
  ::shutdown(static_cast<int>(handle), SHUT_RDWR);
#endif
}

[[nodiscard]] Status apply_deadline(SocketHandle handle, std::uint64_t deadline_ms) {
  if (deadline_ms == 0) {
    return Status::success();
  }
  if (deadline_ms > 3600000ULL) {
    return Status::error(ErrorCode::BoundsExceeded, "socket deadline exceeds the permitted maximum");
  }
#ifdef _WIN32
  const DWORD value = static_cast<DWORD>(deadline_ms);
  if (::setsockopt(static_cast<SOCKET>(handle), SOL_SOCKET, SO_RCVTIMEO,
                   reinterpret_cast<const char*>(&value), sizeof(value)) != 0 ||
      ::setsockopt(static_cast<SOCKET>(handle), SOL_SOCKET, SO_SNDTIMEO,
                   reinterpret_cast<const char*>(&value), sizeof(value)) != 0) {
    return Status::error(ErrorCode::IoFailure, "cannot configure socket deadline");
  }
#else
  struct timeval value {};
  value.tv_sec = static_cast<time_t>(deadline_ms / 1000ULL);
  value.tv_usec = static_cast<suseconds_t>((deadline_ms % 1000ULL) * 1000ULL);
  if (::setsockopt(static_cast<int>(handle), SOL_SOCKET, SO_RCVTIMEO, &value, sizeof(value)) != 0 ||
      ::setsockopt(static_cast<int>(handle), SOL_SOCKET, SO_SNDTIMEO, &value, sizeof(value)) != 0) {
    return Status::error(ErrorCode::IoFailure, "cannot configure socket deadline");
  }
#endif
  return Status::success();
}

void configure_socket(SocketHandle handle) {
#ifdef _WIN32
  BOOL enabled = TRUE;
  ::setsockopt(static_cast<SOCKET>(handle), IPPROTO_TCP, TCP_NODELAY,
               reinterpret_cast<const char*>(&enabled), sizeof(enabled));
#else
  int enabled = 1;
  ::setsockopt(static_cast<int>(handle), IPPROTO_TCP, TCP_NODELAY, &enabled, sizeof(enabled));
#endif
}

[[nodiscard]] Status send_all(SocketHandle handle, std::string_view data) {
  std::size_t sent = 0;
  while (sent < data.size()) {
    const std::size_t remaining = data.size() - sent;
    const int chunk = static_cast<int>(remaining > static_cast<std::size_t>(INT32_MAX)
                                           ? static_cast<std::size_t>(INT32_MAX)
                                           : remaining);
#ifdef _WIN32
    const int written = ::send(static_cast<SOCKET>(handle), data.data() + sent, chunk, 0);
#else
    const int written = static_cast<int>(::send(static_cast<int>(handle), data.data() + sent, chunk,
                                                MSG_NOSIGNAL));
#endif
    if (written <= 0) {
      const int error = last_socket_error();
      if (would_block(error)) {
        return Status::error(ErrorCode::DeadlineExceeded, "socket send deadline elapsed");
      }
      return Status::error(ErrorCode::IoFailure, "socket send failed",
                           Json::object({{"error", Json(error)}}));
    }
    sent += static_cast<std::size_t>(written);
  }
  return Status::success();
}

[[nodiscard]] std::string describe_peer(const sockaddr_storage& address) {
  char buffer[INET6_ADDRSTRLEN + 16] = {};
  if (address.ss_family == AF_INET) {
    const auto* ipv4 = reinterpret_cast<const sockaddr_in*>(&address);
    char text[INET_ADDRSTRLEN] = {};
#ifdef _WIN32
    InetNtopA(AF_INET, &ipv4->sin_addr, text, sizeof(text));
#else
    ::inet_ntop(AF_INET, &ipv4->sin_addr, text, sizeof(text));
#endif
    std::snprintf(buffer, sizeof(buffer), "%s:%u", text, static_cast<unsigned>(ntohs(ipv4->sin_port)));
  } else if (address.ss_family == AF_INET6) {
    const auto* ipv6 = reinterpret_cast<const sockaddr_in6*>(&address);
    char text[INET6_ADDRSTRLEN] = {};
#ifdef _WIN32
    InetNtopA(AF_INET6, &ipv6->sin6_addr, text, sizeof(text));
#else
    ::inet_ntop(AF_INET6, &ipv6->sin6_addr, text, sizeof(text));
#endif
    std::snprintf(buffer, sizeof(buffer), "[%s]:%u", text,
                  static_cast<unsigned>(ntohs(ipv6->sin6_port)));
  } else {
    std::snprintf(buffer, sizeof(buffer), "unknown");
  }
  return std::string(buffer);
}

}  // namespace

Status SocketRuntime::ensure() {
#ifdef _WIN32
  static std::once_flag once;
  static Status result = Status::success();
  std::call_once(once, []() {
    WSADATA data{};
    const int code = WSAStartup(MAKEWORD(2, 2), &data);
    if (code != 0) {
      result = Status::error(ErrorCode::IoFailure, "WSAStartup failed", Json::object({{"code", Json(code)}}));
    }
  });
  return result;
#else
  return Status::success();
#endif
}

TcpConnection::TcpConnection(SocketHandle handle, FrameLimits limits, std::string peer)
    : handle_(handle), decoder_(limits), limits_(limits), peer_(std::move(peer)) {}

TcpConnection::~TcpConnection() { close(); }

TcpConnection::TcpConnection(TcpConnection&& other) noexcept
    : handle_(other.handle_), decoder_(other.decoder_), limits_(other.limits_), peer_(std::move(other.peer_)) {
  other.handle_ = kInvalidSocket;
}

TcpConnection& TcpConnection::operator=(TcpConnection&& other) noexcept {
  if (this != &other) {
    close();
    handle_ = other.handle_;
    decoder_ = other.decoder_;
    limits_ = other.limits_;
    peer_ = std::move(other.peer_);
    other.handle_ = kInvalidSocket;
  }
  return *this;
}

void TcpConnection::set_io_deadline_ms(std::uint64_t deadline_ms) {
  if (handle_ != kInvalidSocket) {
    (void)apply_deadline(handle_, deadline_ms);
  }
}

Status TcpConnection::send_frame(std::string_view payload, std::uint8_t flags) {
  if (handle_ == kInvalidSocket) {
    return Status::error(ErrorCode::IoFailure, "connection is closed");
  }
  auto encoded = encode_frame(payload, flags, limits_);
  if (!encoded.ok()) {
    return encoded.status();
  }
  return send_all(handle_, encoded.value());
}

Result<Frame> TcpConnection::receive_frame() {
  if (handle_ == kInvalidSocket) {
    return Status::error(ErrorCode::IoFailure, "connection is closed");
  }
  std::vector<Frame> frames;
  std::string chunk;
  chunk.resize(kReceiveChunkBytes);
  while (true) {
    const int received = static_cast<int>(
#ifdef _WIN32
        ::recv(static_cast<SOCKET>(handle_), chunk.data(), static_cast<int>(chunk.size()), 0)
#else
        ::recv(static_cast<int>(handle_), chunk.data(), chunk.size(), 0)
#endif
    );
    if (received == 0) {
      const Status finished = decoder_.finish();
      if (!finished.ok()) {
        return finished;
      }
      return Status::error(ErrorCode::IoFailure, "peer closed the connection");
    }
    if (received < 0) {
      const int error = last_socket_error();
      if (would_block(error)) {
        return Status::error(ErrorCode::DeadlineExceeded, "socket receive deadline elapsed");
      }
      return Status::error(ErrorCode::IoFailure, "socket receive failed", Json::object({{"error", Json(error)}}));
    }
    const Status fed = decoder_.feed(std::string_view(chunk.data(), static_cast<std::size_t>(received)), frames);
    if (!fed.ok()) {
      return fed;
    }
    if (!frames.empty()) {
      return frames.front();
    }
  }
}

void TcpConnection::shutdown() noexcept { shutdown_socket(handle_); }

void TcpConnection::close() noexcept {
  if (handle_ != kInvalidSocket) {
    shutdown_socket(handle_);
    close_socket(handle_);
    handle_ = kInvalidSocket;
  }
}

TcpListener::~TcpListener() { close(); }

TcpListener::TcpListener(TcpListener&& other) noexcept
    : handle_(other.handle_), port_(other.port_), limits_(other.limits_) {
  other.handle_ = kInvalidSocket;
  other.port_ = 0;
}

TcpListener& TcpListener::operator=(TcpListener&& other) noexcept {
  if (this != &other) {
    close();
    handle_ = other.handle_;
    port_ = other.port_;
    limits_ = other.limits_;
    other.handle_ = kInvalidSocket;
    other.port_ = 0;
  }
  return *this;
}

void TcpListener::close() noexcept {
  if (handle_ != kInvalidSocket) {
    close_socket(handle_);
    handle_ = kInvalidSocket;
  }
}

Result<TcpListener> TcpListener::bind(const std::string& host, std::uint16_t port, std::size_t backlog) {
  const Status runtime = SocketRuntime::ensure();
  if (!runtime.ok()) {
    return runtime;
  }
  if (backlog == 0 || backlog > 4096) {
    return Status::error(ErrorCode::BoundsExceeded, "listen backlog is outside the permitted range");
  }

  addrinfo hints{};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_protocol = IPPROTO_TCP;
  hints.ai_flags = AI_PASSIVE;
  addrinfo* results = nullptr;
  const std::string service = std::to_string(port);
  const char* node = host.empty() ? nullptr : host.c_str();
  if (::getaddrinfo(node, service.c_str(), &hints, &results) != 0 || results == nullptr) {
    return Status::error(ErrorCode::IoFailure, "cannot resolve listen address",
                         Json::object({{"host", Json(host)}, {"port", Json(static_cast<std::uint64_t>(port))}}));
  }

  SocketHandle handle = kInvalidSocket;
  std::uint16_t bound_port = 0;
  for (addrinfo* entry = results; entry != nullptr; entry = entry->ai_next) {
#ifdef _WIN32
    const SOCKET candidate =
        ::socket(entry->ai_family, entry->ai_socktype, entry->ai_protocol);
    if (candidate == INVALID_SOCKET) {
      continue;
    }
    const SocketHandle candidate_handle = static_cast<SocketHandle>(candidate);
#else
    const int candidate = ::socket(entry->ai_family, entry->ai_socktype, entry->ai_protocol);
    if (candidate < 0) {
      continue;
    }
    const SocketHandle candidate_handle = static_cast<SocketHandle>(candidate);
#endif
    int reuse = 1;
#ifdef _WIN32
    ::setsockopt(candidate, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse), sizeof(reuse));
#else
    ::setsockopt(candidate, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
#endif
    if (::bind(static_cast<decltype(candidate)>(candidate_handle), entry->ai_addr,
               static_cast<int>(entry->ai_addrlen)) != 0) {
      close_socket(candidate_handle);
      continue;
    }
    if (::listen(static_cast<decltype(candidate)>(candidate_handle), static_cast<int>(backlog)) != 0) {
      close_socket(candidate_handle);
      continue;
    }
    sockaddr_storage local{};
    socklen_t local_length = sizeof(local);
    if (::getsockname(static_cast<decltype(candidate)>(candidate_handle),
                      reinterpret_cast<sockaddr*>(&local), &local_length) != 0) {
      close_socket(candidate_handle);
      continue;
    }
    if (local.ss_family == AF_INET) {
      bound_port = ntohs(reinterpret_cast<const sockaddr_in*>(&local)->sin_port);
    } else if (local.ss_family == AF_INET6) {
      bound_port = ntohs(reinterpret_cast<const sockaddr_in6*>(&local)->sin6_port);
    }
    handle = candidate_handle;
    break;
  }
  ::freeaddrinfo(results);

  if (handle == kInvalidSocket) {
    return Status::error(ErrorCode::IoFailure, "cannot bind a listening socket",
                         Json::object({{"host", Json(host)}, {"port", Json(static_cast<std::uint64_t>(port))}}));
  }

  TcpListener listener;
  listener.handle_ = handle;
  listener.port_ = bound_port;
  return listener;
}

Result<TcpConnection> TcpListener::accept_one() {
  if (handle_ == kInvalidSocket) {
    return Status::error(ErrorCode::IoFailure, "listener is closed");
  }
  sockaddr_storage address{};
  socklen_t length = sizeof(address);
#ifdef _WIN32
  const SOCKET accepted =
      ::accept(static_cast<SOCKET>(handle_), reinterpret_cast<sockaddr*>(&address), &length);
  if (accepted == INVALID_SOCKET) {
#else
  const int accepted =
      ::accept(static_cast<int>(handle_), reinterpret_cast<sockaddr*>(&address), &length);
  if (accepted < 0) {
#endif
    const int error = last_socket_error();
    return Status::error(ErrorCode::IoFailure, "accept failed", Json::object({{"error", Json(error)}}));
  }
  const SocketHandle accepted_handle = static_cast<SocketHandle>(accepted);
  configure_socket(accepted_handle);
  return TcpConnection(accepted_handle, limits_, describe_peer(address));
}

Result<RpcRequest> decode_rpc_request(std::string_view payload) {
  const JsonParseResult parsed = parse_json(payload);
  if (!parsed.ok()) {
    return malformed("rpc request is not valid JSON: " + parsed.error);
  }
  const Json& document = *parsed.value;
  if (!document.is_object()) {
    return malformed("rpc request must be an object");
  }
  RpcRequest request;
  request.id = json_string_or(document, "id", "");
  const auto op = json_string(document, "op");
  if (!op.has_value() || op->empty()) {
    return malformed("rpc request is missing op");
  }
  request.op = *op;
  if (request.id.size() > 128) {
    return Status::error(ErrorCode::BoundsExceeded, "rpc request id is too long");
  }
  if (const Json* body = document.find("body")) {
    if (!body->is_object()) {
      return malformed("rpc request body must be an object");
    }
    request.body = *body;
  }
  return request;
}

std::string encode_rpc_response(const RpcResponse& response) {
  Json out = Json::object();
  out.set("id", Json(response.id));
  out.set("op", Json(response.op));
  out.set("status", response.status.to_json());
  out.set("body", response.body);
  return out.dump();
}

FabricServer::FabricServer(ServerOptions options, RpcHandler handler)
    : options_(std::move(options)), handler_(std::move(handler)) {}

FabricServer::~FabricServer() { stop(); }

Status FabricServer::start() {
  if (running_.load()) {
    return Status::error(ErrorCode::AlreadyExists, "server is already running");
  }
  if (options_.worker_threads == 0 || options_.worker_threads > 256) {
    return Status::error(ErrorCode::BoundsExceeded, "worker thread count is outside the permitted range");
  }
  if (options_.max_queued_connections == 0 || options_.max_queued_connections > 4096) {
    return Status::error(ErrorCode::BoundsExceeded,
                         "queued connection limit is outside the permitted range");
  }
  auto listener = TcpListener::bind(options_.host, options_.port, kBacklogDefault);
  if (!listener.ok()) {
    return listener.status();
  }
  listener_ = listener.take();
  listener_.set_limits(options_.frame_limits);
  port_ = listener_.port();
  stopping_.store(false);
  {
    std::lock_guard<std::mutex> lock(queue_mutex_);
    queue_.clear();
    queue_closed_ = false;
  }
  workers_.clear();
  workers_.reserve(options_.worker_threads);
  for (std::size_t index = 0; index < options_.worker_threads; ++index) {
    workers_.emplace_back([this]() { worker_loop(); });
  }
  running_.store(true);
  accept_thread_ = std::thread([this]() { accept_loop(); });
  return Status::success();
}

void FabricServer::stop() {
  if (stopping_.exchange(true)) {
    return;
  }
  running_.store(false);
  listener_.close();  // wakes a blocked accept
  if (accept_thread_.joinable()) {
    accept_thread_.join();
  }
  {
    std::lock_guard<std::mutex> lock(queue_mutex_);
    queue_closed_ = true;
    queue_.clear();
  }
  queue_ready_.notify_all();
  // Waking blocked readers before joining is what keeps shutdown free of
  // lock inversion: no worker can be waiting on a socket forever.
  shutdown_all_sockets();
  for (std::thread& worker : workers_) {
    if (worker.joinable()) {
      worker.join();
    }
  }
  workers_.clear();
  shutdown_all_sockets();
  {
    std::lock_guard<std::mutex> lock(sockets_mutex_);
    live_sockets_.clear();
  }
}

void FabricServer::register_socket(std::uint64_t connection_id, SocketHandle handle) {
  std::lock_guard<std::mutex> lock(sockets_mutex_);
  live_sockets_[connection_id] = handle;
}

void FabricServer::unregister_socket(std::uint64_t connection_id) {
  std::lock_guard<std::mutex> lock(sockets_mutex_);
  live_sockets_.erase(connection_id);
}

void FabricServer::shutdown_all_sockets() {
  std::vector<SocketHandle> handles;
  {
    std::lock_guard<std::mutex> lock(sockets_mutex_);
    handles.reserve(live_sockets_.size());
    for (const auto& entry : live_sockets_) {
      handles.push_back(entry.second);
    }
  }
  for (SocketHandle handle : handles) {
    shutdown_socket(handle);
  }
}

void FabricServer::accept_loop() {
  while (!stopping_.load()) {
    auto connection = listener_.accept_one();
    if (!connection.ok()) {
      if (stopping_.load()) {
        return;
      }
      continue;
    }
    if (stopping_.load()) {
      return;
    }
    TcpConnection accepted = connection.take();
    accepted.set_io_deadline_ms(options_.io_deadline_ms);
    const std::uint64_t connection_id = next_connection_id_.fetch_add(1);
    register_socket(connection_id, accepted.handle());
    accepted_.fetch_add(1);
    bool queued = false;
    {
      // The queue lock is never held while the socket registry lock is taken:
      // the two locks are strictly sequential, never nested.
      std::lock_guard<std::mutex> lock(queue_mutex_);
      if (!queue_closed_ && queue_.size() < options_.max_queued_connections) {
        queue_.emplace_back(std::move(accepted), connection_id);
        queued = true;
      }
    }
    if (queued) {
      queue_ready_.notify_one();
    } else {
      rejected_.fetch_add(1);
      unregister_socket(connection_id);
    }  // the rejected connection is destroyed here, closing its socket
  }
}

void FabricServer::worker_loop() {
  while (true) {
    std::pair<TcpConnection, std::uint64_t> entry;
    {
      std::unique_lock<std::mutex> lock(queue_mutex_);
      queue_ready_.wait(lock, [this]() { return queue_closed_ || !queue_.empty(); });
      if (queue_.empty()) {
        return;  // closed and drained
      }
      entry = std::move(queue_.front());
      queue_.pop_front();
    }
    active_workers_.fetch_add(1);
    serve(std::move(entry.first), entry.second);
    active_workers_.fetch_sub(1);
  }
}

void FabricServer::serve(TcpConnection connection, std::uint64_t connection_id) {
  const RequestContext context{connection.peer(), connection_id};
  std::size_t served = 0;
  while (!stopping_.load()) {
    auto frame = connection.receive_frame();
    if (!frame.ok()) {
      break;
    }
    if (served >= options_.max_requests_per_connection) {
      break;
    }
    ++served;
    auto request = decode_rpc_request(frame.value().payload);
    RpcResponse response;
    if (!request.ok()) {
      response.id = "";
      response.op = "";
      response.status = request.status();
    } else {
      response.id = request.value().id;
      response.op = request.value().op;
      try {
        response = handler_(request.value(), context);
      } catch (const std::exception& error) {
        response.status = Status::error(ErrorCode::Internal, std::string("handler threw: ") + error.what());
      } catch (...) {
        response.status = Status::error(ErrorCode::Internal, "handler threw an unknown exception");
      }
    }
    handled_.fetch_add(1);
    if (response.suppress_response) {
      break;  // modelled lost acknowledgement: the connection closes with no reply
    }
    const std::string payload = encode_rpc_response(response);
    const Status sent = connection.send_frame(payload, static_cast<std::uint8_t>(FrameFlags::Response));
    if (!sent.ok()) {
      break;
    }
  }
  unregister_socket(connection_id);
  connection.close();
}

Result<RpcResponse> rpc_call(const std::string& host, std::uint16_t port, const RpcRequest& request,
                             const ServerOptions& options) {
  const Status runtime = SocketRuntime::ensure();
  if (!runtime.ok()) {
    return runtime;
  }
  addrinfo hints{};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_protocol = IPPROTO_TCP;
  addrinfo* results = nullptr;
  const std::string service = std::to_string(port);
  if (::getaddrinfo(host.c_str(), service.c_str(), &hints, &results) != 0 || results == nullptr) {
    return Status::error(ErrorCode::IoFailure, "cannot resolve peer address",
                         Json::object({{"host", Json(host)}, {"port", Json(static_cast<std::uint64_t>(port))}}));
  }
  SocketHandle handle = kInvalidSocket;
  for (addrinfo* entry = results; entry != nullptr; entry = entry->ai_next) {
#ifdef _WIN32
    const SOCKET candidate = ::socket(entry->ai_family, entry->ai_socktype, entry->ai_protocol);
    if (candidate == INVALID_SOCKET) {
      continue;
    }
#else
    const int candidate = ::socket(entry->ai_family, entry->ai_socktype, entry->ai_protocol);
    if (candidate < 0) {
      continue;
    }
#endif
    if (::connect(static_cast<decltype(candidate)>(candidate), entry->ai_addr,
                  static_cast<int>(entry->ai_addrlen)) == 0) {
      handle = static_cast<SocketHandle>(candidate);
      break;
    }
    close_socket(static_cast<SocketHandle>(candidate));
  }
  ::freeaddrinfo(results);
  if (handle == kInvalidSocket) {
    return Status::error(ErrorCode::IoFailure, "cannot connect to peer",
                         Json::object({{"host", Json(host)}, {"port", Json(static_cast<std::uint64_t>(port))}}));
  }
  configure_socket(handle);
  TcpConnection connection(handle, options.frame_limits, host + ":" + service);
  connection.set_io_deadline_ms(options.io_deadline_ms);
  Json document = Json::object();
  document.set("id", Json(request.id));
  document.set("op", Json(request.op));
  document.set("body", request.body);
  const Status sent = connection.send_frame(document.dump());
  if (!sent.ok()) {
    return sent;
  }
  auto frame = connection.receive_frame();
  if (!frame.ok()) {
    return frame.status();
  }
  const JsonParseResult parsed = parse_json(frame.value().payload);
  if (!parsed.ok()) {
    return malformed("rpc response is not valid JSON: " + parsed.error);
  }
  const Json& response_document = *parsed.value;
  if (!response_document.is_object()) {
    return malformed("rpc response must be an object");
  }
  RpcResponse response;
  response.id = json_string_or(response_document, "id", "");
  response.op = json_string_or(response_document, "op", "");
  const Json* status = response_document.find("status");
  if (status == nullptr || !status->is_object()) {
    return malformed("rpc response is missing status");
  }
  const auto code_text = json_string(*status, "code");
  if (!code_text.has_value()) {
    return malformed("rpc response status is missing code");
  }
  ErrorCode code = ErrorCode::Internal;
  bool found = false;
  for (std::uint8_t index = 0; index <= static_cast<std::uint8_t>(ErrorCode::Internal); ++index) {
    const auto candidate = static_cast<ErrorCode>(index);
    if (to_string(candidate) == *code_text) {
      code = candidate;
      found = true;
      break;
    }
  }
  if (!found) {
    return malformed("rpc response status code is unknown: " + *code_text);
  }
  response.status = code == ErrorCode::Ok
                        ? Status::success()
                        : Status::error(code, json_string_or(*status, "message", ""),
                                        status->find("detail") == nullptr ? Json() : *status->find("detail"));
  if (const Json* body = response_document.find("body")) {
    // The body may be any JSON value: several admin operations answer with an
    // array or a scalar, and dropping those would silently lose the response.
    response.body = *body;
  }
  return response;
}

}  // namespace fabric::evolution
