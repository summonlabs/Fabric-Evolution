// Fabric Evolution — command line argument handling (implementation).
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "args.hpp"

#include <charconv>

namespace fabric::evolution::tools {

Arguments::Arguments(int argc, char** argv, std::initializer_list<std::string_view> value_options) {
  const auto takes_value = [&value_options](const std::string& name) {
    for (const std::string_view candidate : value_options) {
      if (candidate == name) {
        return true;
      }
    }
    return false;
  };
  for (int index = 1; index < argc; ++index) {
    const std::string token = argv[index];
    if (token.size() < 2 || token[0] != '-') {
      positional_.push_back(token);
      continue;
    }
    std::string name = token;
    std::string value;
    const std::size_t equals = token.find('=');
    if (equals != std::string::npos) {
      name = token.substr(0, equals);
      value = token.substr(equals + 1);
    } else if (takes_value(name) && index + 1 < argc) {
      value = argv[index + 1];
      ++index;
    }
    options_.emplace_back(name, value);
  }
}

bool Arguments::has(std::string_view name) const {
  for (const auto& option : options_) {
    if (option.first == name) {
      return true;
    }
  }
  return false;
}

std::optional<std::string> Arguments::value(std::string_view name) const {
  for (const auto& option : options_) {
    if (option.first == name) {
      return option.second;
    }
  }
  return std::nullopt;
}

std::string Arguments::value_or(std::string_view name, std::string fallback) const {
  const auto found = value(name);
  return found.has_value() ? *found : std::move(fallback);
}

std::uint64_t Arguments::u64_or(std::string_view name, std::uint64_t fallback) const {
  const auto found = value(name);
  if (!found.has_value() || found->empty()) {
    return fallback;
  }
  std::uint64_t parsed = 0;
  const auto result = std::from_chars(found->data(), found->data() + found->size(), parsed);
  if (result.ec != std::errc{} || result.ptr != found->data() + found->size()) {
    error_ = "option " + std::string(name) + " expects an integer";
    return fallback;
  }
  return parsed;
}

std::uint16_t Arguments::u16_or(std::string_view name, std::uint16_t fallback) const {
  const std::uint64_t parsed = u64_or(name, fallback);
  if (parsed > 65535) {
    error_ = "option " + std::string(name) + " is out of range";
    return fallback;
  }
  return static_cast<std::uint16_t>(parsed);
}

std::vector<std::string> Arguments::positional() const { return positional_; }

}  // namespace fabric::evolution::tools
