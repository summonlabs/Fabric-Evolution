// Fabric Evolution — test runner entry point.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "support/test_harness.hpp"

#if defined(_WIN32) && defined(_DEBUG)
#include <crtdbg.h>
#endif

namespace fabric::evolution::test {
namespace {

Registry* g_registry = nullptr;

}  // namespace

Registry& Registry::instance() {
  static Registry registry;
  return registry;
}

bool Registry::add(std::string suite, std::string name, void (*body)()) {
  g_registry = &instance();
  cases_.push_back(TestCase{std::move(suite), std::move(name), body});
  return true;
}

int run_all(const RunOptions& options) {
  const Registry& registry = Registry::instance();
  std::vector<const TestCase*> selected;
  for (const TestCase& test_case : registry.cases()) {
    const std::string full = test_case.suite + "." + test_case.name;
    if (!options.filter.empty() && full.find(options.filter) == std::string::npos) {
      continue;
    }
    selected.push_back(&test_case);
  }

  if (options.list_only) {
    for (const TestCase* test_case : selected) {
      std::printf("%s.%s\n", test_case->suite.c_str(), test_case->name.c_str());
    }
    return 0;
  }

#if defined(_WIN32) && defined(_DEBUG)
  // The Debug configuration runs with the MSVC runtime checks enabled. Turning on
  // the debug heap makes every test case validate the heap before it returns, so
  // an overwrite or a double free is reported at the test that caused it rather
  // than at process exit. Release builds have no debug heap, so this is compiled
  // out entirely.
  const int previous_flags = _CrtSetDbgFlag(_CRTDBG_REPORT_FLAG);
  _CrtSetDbgFlag(previous_flags | _CRTDBG_ALLOC_MEM_DF | _CRTDBG_LEAK_CHECK_DF);
  std::printf("debug heap validation: enabled\n");
#endif

  std::size_t passed = 0;
  std::size_t failed = 0;
  std::vector<std::string> failures;

  for (unsigned iteration = 0; iteration < options.repeat; ++iteration) {
    for (const TestCase* test_case : selected) {
      const std::string full = test_case->suite + "." + test_case->name;
      try {
        test_case->body();
        ++passed;
        if (options.verbose) {
          std::printf("[ PASS ] %s\n", full.c_str());
        }
      } catch (const std::exception& error) {
        ++failed;
        failures.push_back(full + " -- " + error.what());
        std::printf("[ FAIL ] %s\n         %s\n", full.c_str(), error.what());
      } catch (...) {
        ++failed;
        failures.push_back(full + " -- unknown exception");
        std::printf("[ FAIL ] %s\n         unknown exception\n", full.c_str());
      }
#if defined(_WIN32) && defined(_DEBUG)
      if (_CrtCheckMemory() == 0) {
        ++failed;
        failures.push_back(full + " -- debug heap validation failed");
        std::printf("[ FAIL ] %s\n         debug heap validation failed\n", full.c_str());
      }
#endif
    }
  }

#if defined(_WIN32) && defined(_DEBUG)
  if (_CrtCheckMemory() == 0) {
    std::printf("\nfinal debug heap validation failed\n");
    return 1;
  }
#endif
  std::printf("\n%zu passed, %zu failed (%zu test cases, %zu selected, repeat=%u)\n", passed, failed,
              registry.cases().size(), selected.size(), options.repeat);
  if (failed != 0) {
    std::printf("\nfailures:\n");
    for (const std::string& failure : failures) {
      std::printf("  %s\n", failure.c_str());
    }
    return 1;
  }
  return 0;
}

}  // namespace fabric::evolution::test

int main(int argc, char** argv) {
  fabric::evolution::test::RunOptions options;
  for (int index = 1; index < argc; ++index) {
    const std::string argument = argv[index];
    if (argument == "--list") {
      options.list_only = true;
    } else if (argument == "--verbose" || argument == "-v") {
      options.verbose = true;
    } else if (argument.rfind("--filter=", 0) == 0) {
      options.filter = argument.substr(std::strlen("--filter="));
    } else if (argument.rfind("--repeat=", 0) == 0) {
      options.repeat = static_cast<unsigned>(std::stoul(argument.substr(std::strlen("--repeat="))));
      if (options.repeat == 0) {
        options.repeat = 1;
      }
    } else {
      std::printf("unknown argument: %s\n", argument.c_str());
      return 2;
    }
  }
  return fabric::evolution::test::run_all(options);
}
