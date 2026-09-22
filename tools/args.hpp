// Fabric Evolution — command line argument handling for the shipped tools.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace fabric::evolution::tools {

// Minimal, strict argument parser: every option is either a flag or takes
// exactly one value. Unknown options and missing values are reported instead of
// being silently ignored.
class Arguments {
 public:
  // value_options lists the options that take a value; every other option is a
  // boolean flag and never consumes the following token.
  Arguments(int argc, char** argv, std::initializer_list<std::string_view> value_options);

  [[nodiscard]] bool has(std::string_view name) const;
  [[nodiscard]] std::optional<std::string> value(std::string_view name) const;
  [[nodiscard]] std::string value_or(std::string_view name, std::string fallback) const;
  [[nodiscard]] std::uint64_t u64_or(std::string_view name, std::uint64_t fallback) const;
  [[nodiscard]] std::uint16_t u16_or(std::string_view name, std::uint16_t fallback) const;
  [[nodiscard]] std::vector<std::string> positional() const;
  [[nodiscard]] const std::string& error() const noexcept { return error_; }
  [[nodiscard]] bool ok() const noexcept { return error_.empty(); }

 private:
  std::vector<std::pair<std::string, std::string>> options_;
  std::vector<std::string> positional_;
  mutable std::string error_;
};

}  // namespace fabric::evolution::tools
