// Fabric Evolution — independent OS process harness (implementation).
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "support/process.hpp"

#include <cstring>
#include <thread>

#ifdef _WIN32
#include <array>
#else
#include <csignal>
#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace fabric::evolution::test {
namespace {

#ifdef _WIN32
[[nodiscard]] std::string quote_argument(const std::string& argument) {
  std::string out = "\"";
  for (char c : argument) {
    if (c == '"') {
      out += "\\\"";
    } else {
      out += c;
    }
  }
  out += "\"";
  return out;
}
#endif

}  // namespace

ChildProcess::~ChildProcess() {
  if (!exited_) {
    (void)terminate();
  }
  close_pipe();
#ifdef _WIN32
  // The process handle is a kernel resource: leaving it open keeps the process
  // object alive for the lifetime of the test run.
  if (process_handle_ != nullptr) {
    ::CloseHandle(static_cast<HANDLE>(process_handle_));
    process_handle_ = nullptr;
  }
#endif
}

ChildProcess::ChildProcess(ChildProcess&& other) noexcept
    : pid_(other.pid_),
#ifdef _WIN32
      process_handle_(other.process_handle_),
      read_handle_(other.read_handle_),
#else
      read_fd_(other.read_fd_),
#endif
      buffer_(std::move(other.buffer_)),
      exited_(other.exited_),
      exit_code_(other.exit_code_) {
  other.pid_ = 0;
#ifdef _WIN32
  other.process_handle_ = nullptr;
  other.read_handle_ = nullptr;
#else
  other.read_fd_ = -1;
#endif
}

ChildProcess& ChildProcess::operator=(ChildProcess&& other) noexcept {
  if (this != &other) {
    if (!exited_) {
      (void)terminate();
    }
    close_pipe();
#ifdef _WIN32
    if (process_handle_ != nullptr) {
      ::CloseHandle(static_cast<HANDLE>(process_handle_));
      process_handle_ = nullptr;
    }
#endif
    pid_ = other.pid_;
#ifdef _WIN32
    process_handle_ = other.process_handle_;
    read_handle_ = other.read_handle_;
    other.process_handle_ = nullptr;
    other.read_handle_ = nullptr;
#else
    read_fd_ = other.read_fd_;
    other.read_fd_ = -1;
#endif
    buffer_ = std::move(other.buffer_);
    exited_ = other.exited_;
    exit_code_ = other.exit_code_;
    other.pid_ = 0;
  }
  return *this;
}

void ChildProcess::close_pipe() {
#ifdef _WIN32
  if (read_handle_ != nullptr) {
    ::CloseHandle(static_cast<HANDLE>(read_handle_));
    read_handle_ = nullptr;
  }
#else
  if (read_fd_ >= 0) {
    ::close(read_fd_);
    read_fd_ = -1;
  }
#endif
}

Result<ChildProcess> ChildProcess::spawn(const std::string& executable,
                                         const std::vector<std::string>& arguments) {
#ifdef _WIN32
  SECURITY_ATTRIBUTES attributes{};
  attributes.nLength = sizeof(attributes);
  attributes.bInheritHandle = TRUE;

  HANDLE read_handle = nullptr;
  HANDLE write_handle = nullptr;
  if (::CreatePipe(&read_handle, &write_handle, &attributes, 0) == 0) {
    return Status::error(ErrorCode::IoFailure, "cannot create the child output pipe");
  }
  if (::SetHandleInformation(read_handle, HANDLE_FLAG_INHERIT, 0) == 0) {
    ::CloseHandle(read_handle);
    ::CloseHandle(write_handle);
    return Status::error(ErrorCode::IoFailure, "cannot configure the child output pipe");
  }

  std::string command_line = quote_argument(executable);
  for (const std::string& argument : arguments) {
    command_line.push_back(' ');
    command_line += quote_argument(argument);
  }

  // Only the output pipe is inherited. Without an explicit handle list every
  // inheritable handle in the parent (including sockets) would be duplicated into
  // every child, which leaks resources and can keep ports bound after a child dies.
  STARTUPINFOEXA startup{};
  startup.StartupInfo.cb = sizeof(STARTUPINFOEXA);
  startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
  startup.StartupInfo.hStdOutput = write_handle;
  startup.StartupInfo.hStdError = write_handle;
  startup.StartupInfo.hStdInput = nullptr;

  SIZE_T attribute_bytes = 0;
  ::InitializeProcThreadAttributeList(nullptr, 1, 0, &attribute_bytes);
  std::vector<char> attribute_storage(attribute_bytes == 0 ? 1 : attribute_bytes);
  startup.lpAttributeList = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attribute_storage.data());
  if (::InitializeProcThreadAttributeList(startup.lpAttributeList, 1, 0, &attribute_bytes) == 0) {
    ::CloseHandle(read_handle);
    ::CloseHandle(write_handle);
    return Status::error(ErrorCode::IoFailure, "cannot prepare the child handle list");
  }
  HANDLE inherited_handles[1] = {write_handle};
  if (::UpdateProcThreadAttribute(startup.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                                  inherited_handles, sizeof(inherited_handles), nullptr,
                                  nullptr) == 0) {
    ::DeleteProcThreadAttributeList(startup.lpAttributeList);
    ::CloseHandle(read_handle);
    ::CloseHandle(write_handle);
    return Status::error(ErrorCode::IoFailure, "cannot restrict the child handle list");
  }

  PROCESS_INFORMATION information{};
  std::vector<char> mutable_command(command_line.begin(), command_line.end());
  mutable_command.push_back('\0');
  const BOOL created = ::CreateProcessA(nullptr, mutable_command.data(), nullptr, nullptr, TRUE,
                                        EXTENDED_STARTUPINFO_PRESENT | CREATE_NO_WINDOW, nullptr,
                                        nullptr, &startup.StartupInfo, &information);
  ::DeleteProcThreadAttributeList(startup.lpAttributeList);
  ::CloseHandle(write_handle);
  if (created == 0) {
    ::CloseHandle(read_handle);
    return Status::error(ErrorCode::IoFailure, "cannot start child process",
                         Json::object({{"executable", Json(executable)},
                                       {"win32_error", Json(static_cast<std::uint64_t>(GetLastError()))}}));
  }
  ::CloseHandle(information.hThread);

  ChildProcess process;
  process.pid_ = static_cast<std::uint64_t>(information.dwProcessId);
  process.process_handle_ = information.hProcess;
  process.read_handle_ = read_handle;
  return process;
#else
  int pipe_descriptors[2] = {-1, -1};
  if (::pipe(pipe_descriptors) != 0) {
    return Status::error(ErrorCode::IoFailure, "cannot create the child output pipe");
  }
  const pid_t pid = ::fork();
  if (pid < 0) {
    ::close(pipe_descriptors[0]);
    ::close(pipe_descriptors[1]);
    return Status::error(ErrorCode::IoFailure, "cannot fork");
  }
  if (pid == 0) {
    ::dup2(pipe_descriptors[1], STDOUT_FILENO);
    ::dup2(pipe_descriptors[1], STDERR_FILENO);
    ::close(pipe_descriptors[0]);
    ::close(pipe_descriptors[1]);
    std::vector<char*> argv;
    argv.push_back(const_cast<char*>(executable.c_str()));
    for (const std::string& argument : arguments) {
      argv.push_back(const_cast<char*>(argument.c_str()));
    }
    argv.push_back(nullptr);
    ::execv(executable.c_str(), argv.data());
    ::_exit(127);
  }
  ::close(pipe_descriptors[1]);
  ChildProcess process;
  process.pid_ = static_cast<std::uint64_t>(pid);
  process.read_fd_ = pipe_descriptors[0];
  return process;
#endif
}

void ChildProcess::refresh_state() {
  if (exited_ || pid_ == 0) {
    return;
  }
#ifdef _WIN32
  if (process_handle_ == nullptr) {
    return;
  }
  DWORD code = 0;
  if (::GetExitCodeProcess(static_cast<HANDLE>(process_handle_), &code) != 0 && code != STILL_ACTIVE) {
    exited_ = true;
    exit_code_ = static_cast<int>(code);
  }
#else
  int status = 0;
  const pid_t result = ::waitpid(static_cast<pid_t>(pid_), &status, WNOHANG);
  if (result == static_cast<pid_t>(pid_)) {
    exited_ = true;
    exit_code_ = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
  }
#endif
}

bool ChildProcess::running() const {
  const_cast<ChildProcess*>(this)->refresh_state();
  return !exited_;
}

std::optional<std::string> ChildProcess::read_line() {
  if (pid_ == 0) {
    return std::nullopt;
  }
  while (true) {
    const std::size_t newline = buffer_.find('\n');
    if (newline != std::string::npos) {
      std::string line = buffer_.substr(0, newline);
      buffer_.erase(0, newline + 1);
      if (!line.empty() && line.back() == '\r') {
        line.pop_back();
      }
      return line;
    }
#ifdef _WIN32
    if (read_handle_ == nullptr) {
      return std::nullopt;
    }
    char byte = 0;
    DWORD read = 0;
    if (::ReadFile(static_cast<HANDLE>(read_handle_), &byte, 1, &read, nullptr) == 0 || read == 0) {
      close_pipe();
      return std::nullopt;
    }
#else
    if (read_fd_ < 0) {
      return std::nullopt;
    }
    char byte = 0;
    const ssize_t read = ::read(read_fd_, &byte, 1);
    if (read <= 0) {
      close_pipe();
      return std::nullopt;
    }
#endif
    buffer_.push_back(byte);
  }
}

std::string ChildProcess::drain() {
  std::string out;
  while (true) {
    const auto line = read_line();
    if (!line.has_value()) {
      break;
    }
    out += *line;
    out.push_back('\n');
  }
  return out + buffer_;
}

bool ChildProcess::terminate() {
  if (pid_ == 0) {
    return false;
  }
  refresh_state();
  if (exited_) {
    close_pipe();
    return false;
  }
#ifdef _WIN32
  if (process_handle_ == nullptr) {
    return false;
  }
  const BOOL killed = ::TerminateProcess(static_cast<HANDLE>(process_handle_), 1);
  const DWORD waited = ::WaitForSingleObject(static_cast<HANDLE>(process_handle_), 30000);
  // A process that did not exit within the wait is reported rather than assumed
  // gone, because several failure scenarios depend on the process really being dead.
  exited_ = waited == WAIT_OBJECT_0;
  exit_code_ = 1;
  close_pipe();
  return killed != 0 && exited_;
#else
  ::kill(static_cast<pid_t>(pid_), SIGKILL);
  int status = 0;
  ::waitpid(static_cast<pid_t>(pid_), &status, 0);
  exited_ = true;
  exit_code_ = -1;
  close_pipe();
  return true;
#endif
}

int ChildProcess::wait() {
  if (pid_ == 0) {
    return exit_code_;
  }
#ifdef _WIN32
  if (process_handle_ != nullptr) {
    ::WaitForSingleObject(static_cast<HANDLE>(process_handle_), INFINITE);
    DWORD code = 0;
    ::GetExitCodeProcess(static_cast<HANDLE>(process_handle_), &code);
    exit_code_ = static_cast<int>(code);
    exited_ = true;
  }
#else
  int status = 0;
  ::waitpid(static_cast<pid_t>(pid_), &status, 0);
  exited_ = true;
  exit_code_ = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
#endif
  return exit_code_;
}

std::string parse_field(const std::string& line, const std::string& key) {
  const std::string needle = key + "=";
  const std::size_t position = line.find(needle);
  if (position == std::string::npos) {
    return std::string();
  }
  const std::size_t start = position + needle.size();
  const std::size_t end = line.find(' ', start);
  return line.substr(start, end == std::string::npos ? std::string::npos : end - start);
}

std::optional<std::uint16_t> parse_port(const std::string& line) {
  const std::string text = parse_field(line, "port");
  if (text.empty()) {
    return std::nullopt;
  }
  try {
    const unsigned long value = std::stoul(text);
    if (value == 0 || value > 65535) {
      return std::nullopt;
    }
    return static_cast<std::uint16_t>(value);
  } catch (const std::exception&) {
    return std::nullopt;
  }
}

}  // namespace fabric::evolution::test
