// Fabric Evolution — independent OS process harness for distributed proof tests.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// The runtime claims distributed behaviour, so the tests exercise it the only
// honest way: real child processes with real stdout pipes that can be killed at
// an arbitrary moment and restarted. There is no in-process substitution here.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <optional>

#include "fabric/evolution/status.hpp"

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/types.h>
#endif

namespace fabric::evolution::test {

class ChildProcess {
 public:
  ChildProcess() = default;
  ~ChildProcess();

  ChildProcess(const ChildProcess&) = delete;
  ChildProcess& operator=(const ChildProcess&) = delete;
  ChildProcess(ChildProcess&& other) noexcept;
  ChildProcess& operator=(ChildProcess&& other) noexcept;

  // Starts the executable with the given arguments. stdout and stderr are
  // captured through an anonymous pipe.
  [[nodiscard]] static Result<ChildProcess> spawn(const std::string& executable,
                                                  const std::vector<std::string>& arguments);

  // Reads one line from the captured output. Returns an empty optional at EOF.
  [[nodiscard]] std::optional<std::string> read_line();
  // Reads until the process exits or the output stops; used after termination.
  [[nodiscard]] std::string drain();

  [[nodiscard]] bool running() const;
  // Hard kill. Returns true when the process was actually terminated.
  bool terminate();
  // Waits for exit without a deadline; returns the exit code.
  int wait();
  [[nodiscard]] bool has_exited() const { return exited_; }
  [[nodiscard]] int exit_code() const { return exit_code_; }
  [[nodiscard]] std::uint64_t pid() const { return pid_; }
  void close_pipe();

 private:
  void refresh_state();

  std::uint64_t pid_ = 0;
#ifdef _WIN32
  void* process_handle_ = nullptr;
  void* read_handle_ = nullptr;
#else
  int read_fd_ = -1;
#endif
  std::string buffer_;
  bool exited_ = false;
  int exit_code_ = -1;
};

// Parses "key=value" pairs out of a readiness line such as
// "fabric-evolution-node ready component=a port=54321".
[[nodiscard]] std::string parse_field(const std::string& line, const std::string& key);
[[nodiscard]] std::optional<std::uint16_t> parse_port(const std::string& line);

}  // namespace fabric::evolution::test
