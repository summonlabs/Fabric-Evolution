// Fabric Evolution — command line interface end-to-end tests.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// The CLI is exercised as a real child process talking to a real controller
// process, which is how an operator would use it.

#include <string>
#include <vector>

#include "support/cluster.hpp"
#include "support/test_harness.hpp"

namespace fabric::evolution {
namespace {

[[nodiscard]] std::string endpoint_of(const test::EvolutionFixture& fixture) {
  return "127.0.0.1:" + std::to_string(fixture.controller_port());
}

}  // namespace

FABRIC_TEST(cli, plan_preflight_start_status_and_inspection) {
  test::EvolutionFixture fixture;
  const std::string manifest_path =
      test::write_manifest_file(fixture.root() + "/manifest.json", fixture.manifest());
  const std::string endpoint = endpoint_of(fixture);

  const test::CliResult plan =
      test::run_cli({"--controller", endpoint, "plan", "--manifest", manifest_path});
  FABRIC_CHECK_EQ(plan.exit_code, 0);
  FABRIC_CHECK(plan.output.find("campaign: campaign-e2e state=planned") != std::string::npos);
  FABRIC_CHECK(plan.output.find("decision: campaign.plan [accepted]") != std::string::npos);

  const test::CliResult preflight = test::run_cli({"--controller", endpoint, "preflight"});
  FABRIC_CHECK_EQ(preflight.exit_code, 0);
  FABRIC_CHECK(preflight.output.find("state=preflighting") != std::string::npos);

  const test::CliResult start = test::run_cli({"--controller", endpoint, "start"});
  FABRIC_CHECK_EQ(start.exit_code, 0);
  FABRIC_CHECK(start.output.find("state=completed") != std::string::npos);
  FABRIC_CHECK(start.output.find("phase=predecessor_retired") != std::string::npos);
  FABRIC_CHECK(start.output.find("mutating_owners=1") != std::string::npos);

  const test::CliResult status = test::run_cli({"--controller", endpoint, "status"});
  FABRIC_CHECK_EQ(status.exit_code, 0);
  FABRIC_CHECK(status.output.find("window: open=true") != std::string::npos);
  FABRIC_CHECK(status.output.find("source:") != std::string::npos);
  FABRIC_CHECK(status.output.find("target:") != std::string::npos);
}

FABRIC_TEST(cli, inspection_commands_return_documents) {
  test::EvolutionFixture fixture;
  fixture.plan();
  fixture.preflight();
  fixture.run();
  const std::string endpoint = endpoint_of(fixture);

  const test::CliResult authority = test::run_cli({"--controller", endpoint, "authority"});

  FABRIC_CHECK_EQ(authority.exit_code, 0);
  FABRIC_CHECK(authority.output.find("shard-0") != std::string::npos);

  const test::CliResult epoch = test::run_cli({"--controller", endpoint, "epoch"});
  FABRIC_CHECK_EQ(epoch.exit_code, 0);
  FABRIC_CHECK(epoch.output.find("handoff_phase") != std::string::npos);

  const test::CliResult handoff = test::run_cli({"--controller", endpoint, "handoff"});
  FABRIC_CHECK_EQ(handoff.exit_code, 0);
  FABRIC_CHECK(handoff.output.find("predecessor_retired") != std::string::npos);

  const test::CliResult explain =
      test::run_cli({"--controller", endpoint, "explain", "--topic", "compatibility"});
  FABRIC_CHECK_EQ(explain.exit_code, 0);
  FABRIC_CHECK(explain.output.find("\"satisfied\": true") != std::string::npos);

  const test::CliResult json_status = test::run_cli({"--controller", endpoint, "--json", "status"});
  FABRIC_CHECK_EQ(json_status.exit_code, 0);
  const JsonParseResult parsed = parse_json(json_status.output);
  FABRIC_CHECK(parsed.ok());
  FABRIC_CHECK_EQ(json_string_or(*parsed.value->find("campaign"), "state", ""),
                  std::string("completed"));
}

FABRIC_TEST(cli, refusals_are_reported_with_the_explanation) {
  test::EvolutionFixture fixture;
  const std::string endpoint = endpoint_of(fixture);
  const test::CliResult refused = test::run_cli({"--controller", endpoint, "preflight"});
  FABRIC_CHECK_EQ(refused.exit_code, 1);
  FABRIC_CHECK(refused.output.find("refused:") != std::string::npos);
}

FABRIC_TEST(cli, argument_errors_are_distinct_from_refusals) {
  test::EvolutionFixture fixture;
  const std::string endpoint = endpoint_of(fixture);
  FABRIC_CHECK_EQ(test::run_cli({}).exit_code, 2);
  FABRIC_CHECK_EQ(test::run_cli({"status"}).exit_code, 2);
  FABRIC_CHECK_EQ(test::run_cli({"--controller", endpoint, "nonsense"}).exit_code, 2);
  FABRIC_CHECK_EQ(test::run_cli({"--controller", endpoint, "plan"}).exit_code, 2);
  FABRIC_CHECK_EQ(test::run_cli({"--controller", endpoint, "explain"}).exit_code, 2);
  FABRIC_CHECK_EQ(test::run_cli({"--controller", "not-an-endpoint", "status"}).exit_code, 2);
  FABRIC_CHECK_EQ(test::run_cli({"--controller", endpoint, "plan", "--manifest", "/no/such/file"}).exit_code, 3);
  // Nothing is listening on this port.
  FABRIC_CHECK_EQ(test::run_cli({"--controller", "127.0.0.1:1", "--deadline-ms", "2000", "status"}).exit_code, 4);
}

}  // namespace fabric::evolution
