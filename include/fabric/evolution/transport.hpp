// Fabric Evolution — real TCP transport with bounded threading.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// The runtime proves its distributed behaviour over independent OS processes and
// real kernel sockets. This header provides the socket primitives (connection,
// listener) and a server with a fixed worker pool, a bounded admission queue and
// a real shutdown sequence: admission stops first, then the accept thread is
// joined, then every live socket is shut down so blocked readers wake up, and
// only then are worker threads joined. No mutex is ever held across a join, and
// no handler is ever invoked while a server lock is held.

#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "fabric/evolution/framing.hpp"
#include "fabric/evolution/status.hpp"

namespace fabric::evolution {

using SocketHandle = std::uintptr_t;
inline constexpr SocketHandle kInvalidSocket = static_cast<SocketHandle>(~static_cast<SocketHandle>(0));

// Reference-counted platform socket initialisation (WSAStartup on Windows).
class SocketRuntime {
 public:
  [[nodiscard]] static Status ensure();
};

class TcpConnection {
 public:
  TcpConnection() = default;
  TcpConnection(SocketHandle handle, FrameLimits limits, std::string peer);
  ~TcpConnection();

  TcpConnection(const TcpConnection&) = delete;
  TcpConnection& operator=(const TcpConnection&) = delete;
  TcpConnection(TcpConnection&& other) noexcept;
  TcpConnection& operator=(TcpConnection&& other) noexcept;

  [[nodiscard]] bool is_open() const noexcept { return handle_ != kInvalidSocket; }
  [[nodiscard]] SocketHandle handle() const noexcept { return handle_; }
  [[nodiscard]] const std::string& peer() const noexcept { return peer_; }

  [[nodiscard]] Status send_frame(std::string_view payload, std::uint8_t flags = 0);
  [[nodiscard]] Result<Frame> receive_frame();
  void set_io_deadline_ms(std::uint64_t deadline_ms);

  // Half-closes the socket so a blocked reader on the peer side wakes up.
  void shutdown() noexcept;
  void close() noexcept;

 private:
  SocketHandle handle_ = kInvalidSocket;
  FrameDecoder decoder_;
  FrameLimits limits_;
  std::string peer_;
};

class TcpListener {
 public:
  TcpListener() = default;
  ~TcpListener();

  TcpListener(const TcpListener&) = delete;
  TcpListener& operator=(const TcpListener&) = delete;
  TcpListener(TcpListener&& other) noexcept;
  TcpListener& operator=(TcpListener&& other) noexcept;

  static Result<TcpListener> bind(const std::string& host, std::uint16_t port,
                                  std::size_t backlog = 64);
  [[nodiscard]] Result<TcpConnection> accept_one();
  [[nodiscard]] std::uint16_t port() const noexcept { return port_; }
  [[nodiscard]] bool is_open() const noexcept { return handle_ != kInvalidSocket; }
  void set_limits(FrameLimits limits) noexcept { limits_ = limits; }
  void close() noexcept;

 private:
  SocketHandle handle_ = kInvalidSocket;
  std::uint16_t port_ = 0;
  FrameLimits limits_;
};

struct RpcRequest {
  std::string id;
  std::string op;
  Json body = Json::object();
};

struct RpcResponse {
  std::string id;
  std::string op;
  Status status;
  Json body = Json::object();
  // Set by a handler that wants the connection closed without a reply. It is
  // used to model a lost acknowledgement deterministically in failure tests.
  bool suppress_response = false;
};

struct RequestContext {
  std::string peer;
  std::uint64_t connection_id = 0;
};

using RpcHandler = std::function<RpcResponse(const RpcRequest&, const RequestContext&)>;

[[nodiscard]] Result<RpcRequest> decode_rpc_request(std::string_view payload);
[[nodiscard]] std::string encode_rpc_response(const RpcResponse& response);

struct ServerOptions {
  std::string host = "127.0.0.1";
  std::uint16_t port = 0;
  std::size_t worker_threads = 16;
  std::size_t max_queued_connections = 64;
  std::size_t max_requests_per_connection = 1'000'000;
  std::uint64_t io_deadline_ms = 30000;
  FrameLimits frame_limits;
};

class FabricServer {
 public:
  FabricServer(ServerOptions options, RpcHandler handler);
  ~FabricServer();

  FabricServer(const FabricServer&) = delete;
  FabricServer& operator=(const FabricServer&) = delete;

  [[nodiscard]] Status start();
  // Idempotent. Must not be called from inside a handler.
  void stop();
  [[nodiscard]] bool running() const noexcept { return running_.load(); }
  [[nodiscard]] std::uint16_t port() const noexcept { return port_; }
  [[nodiscard]] std::uint64_t accepted_connections() const noexcept { return accepted_.load(); }
  [[nodiscard]] std::uint64_t rejected_connections() const noexcept { return rejected_.load(); }
  [[nodiscard]] std::uint64_t handled_requests() const noexcept { return handled_.load(); }
  [[nodiscard]] std::size_t active_workers() const noexcept { return active_workers_.load(); }

 private:
  void accept_loop();
  void worker_loop();
  void serve(TcpConnection connection, std::uint64_t connection_id);
  void register_socket(std::uint64_t connection_id, SocketHandle handle);
  void unregister_socket(std::uint64_t connection_id);
  void shutdown_all_sockets();

  ServerOptions options_;
  RpcHandler handler_;
  TcpListener listener_;
  std::atomic<bool> running_{false};
  std::atomic<bool> stopping_{false};
  std::uint16_t port_ = 0;
  std::thread accept_thread_;
  std::vector<std::thread> workers_;

  std::mutex queue_mutex_;
  std::condition_variable queue_ready_;
  std::deque<std::pair<TcpConnection, std::uint64_t>> queue_;
  bool queue_closed_ = false;

  std::mutex sockets_mutex_;
  std::map<std::uint64_t, SocketHandle> live_sockets_;

  std::atomic<std::uint64_t> next_connection_id_{1};
  std::atomic<std::uint64_t> accepted_{0};
  std::atomic<std::uint64_t> rejected_{0};
  std::atomic<std::uint64_t> handled_{0};
  std::atomic<std::size_t> active_workers_{0};
};

// Client helper: connects, performs one request/response exchange, disconnects.
[[nodiscard]] Result<RpcResponse> rpc_call(const std::string& host, std::uint16_t port,
                                           const RpcRequest& request,
                                           const ServerOptions& options);

}  // namespace fabric::evolution
