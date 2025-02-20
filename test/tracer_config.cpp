#include <datadog/id_generator.h>
#include <datadog/propagation_styles.h>
#include <datadog/threaded_event_scheduler.h>
#include <datadog/tracer.h>
#include <datadog/tracer_config.h>

#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <optional>
#include <ostream>
#include <stdexcept>
#include <string>
#include <system_error>

#include "mocks/collectors.h"
#include "mocks/event_schedulers.h"
#include "mocks/loggers.h"
#include "test.h"
#ifdef _MSC_VER
#include <winbase.h>  // SetEnvironmentVariable
#else
#include <stdlib.h>  // setenv, unsetenv
#endif

namespace datadog {
namespace tracing {

std::ostream& operator<<(std::ostream& stream,
                         const PropagationStyles& styles) {
  stream << '{';
  const char* separator = "";
  if (styles.datadog) {
    stream << "Datadog";
    separator = ", ";
  }
  if (styles.b3) {
    stream << separator << "B3";
    separator = ", ";
  }
  return stream << '}';
}

}  // namespace tracing
}  // namespace datadog

using namespace datadog::tracing;

namespace {

// For the lifetime of this object, set a specified environment variable.
// Restore any previous value (or unset the value if it was unset) afterward.
class EnvGuard {
  std::string name_;
  std::optional<std::string> former_value_;

 public:
  EnvGuard(std::string name, std::string value) : name_(std::move(name)) {
    const char* current = std::getenv(name_.c_str());
    if (current) {
      former_value_ = current;
    }
    set_value(value);
  }

  ~EnvGuard() {
    if (former_value_) {
      set_value(*former_value_);
    } else {
      unset();
    }
  }

  void set_value(const std::string& value) {
#ifdef _MSC_VER
    ::SetEnvironmentVariable(name_.c_str(), value.c_str());
#else
    const bool overwrite = true;
    ::setenv(name_.c_str(), value.c_str(), overwrite);
#endif
  }

  void unset() {
#ifdef _MSC_VER
    ::SetEnvironmentVariable(name_.c_str(), NULL);
#else
    ::unsetenv(name_.c_str());
#endif
  }
};

// For brevity when we're tabulating a lot of test cases with parse
// `std::optional<...>` data members.
const auto x = std::nullopt;

// Here's an attempt at a portable secure temporary file.
// There's no standard solution, and it's generally hard on Windows.
class SomewhatSecureTemporaryFile : public std::fstream {
  std::filesystem::path path_;

 public:
  SomewhatSecureTemporaryFile() try {
    namespace fs = std::filesystem;

    // The goal is to create a file whose name is like
    // "/tmp/342394898324/239489029034", where the directory under /tmp has
    // permissions such that only the current user can read/write/cd it.
    const auto tmp = fs::temp_directory_path();
    const int max_attempts = 5;
    for (int i = 0; i < max_attempts; ++i) {
      const auto dir = tmp / std::to_string(default_id_generator());
      std::error_code err;
      if (!fs::create_directory(dir, err)) {
        continue;
      }
      fs::permissions(dir, fs::perms(0700), fs::perm_options::replace, err);
      if (err) {
        continue;
      }
      const auto file = dir / std::to_string(default_id_generator());
      if (fs::exists(file, err) || err) {
        continue;
      }
      // We did it!
      open(file, std::ios::in | std::ios::out | std::ios::app);
      path_ = file;
      return;
    }
    throw std::runtime_error("exhausted all attempts");
  } catch (const std::exception& error) {
    std::cerr << "Unable to create a temporary file: " << error.what() << '\n';
    // `path_` is empty, and this `fstream` is not open.
  }

  ~SomewhatSecureTemporaryFile() {
    if (path_ != std::filesystem::path{}) {
      std::error_code ignored;
      std::filesystem::remove_all(path_.parent_path(), ignored);
    }
  }

  const std::filesystem::path& path() const { return path_; }
};

}  // namespace

TEST_CASE("TracerConfig::defaults") {
  TracerConfig config;

  SECTION("service is required") {
    SECTION("empty") {
      auto finalized = finalize_config(config);
      REQUIRE(!finalized);
      REQUIRE(finalized.error().code == Error::SERVICE_NAME_REQUIRED);
    }
    SECTION("nonempty") {
      config.defaults.service = "testsvc";
      auto finalized = finalize_config(config);
      REQUIRE(finalized);
    }
  }

  SECTION("DD_SERVICE overrides service") {
    const EnvGuard guard{"DD_SERVICE", "foosvc"};
    config.defaults.service = "testsvc";
    auto finalized = finalize_config(config);
    REQUIRE(finalized);
    REQUIRE(finalized->defaults.service == "foosvc");
  }

  SECTION("DD_ENV overrides environment") {
    const EnvGuard guard{"DD_ENV", "prod"};
    config.defaults.environment = "dev";
    config.defaults.service = "required";
    auto finalized = finalize_config(config);
    REQUIRE(finalized);
    REQUIRE(finalized->defaults.environment == "prod");
  }

  SECTION("DD_VERSION overrides version") {
    const EnvGuard guard{"DD_VERSION", "v2"};
    config.defaults.version = "v1";
    config.defaults.service = "required";
    auto finalized = finalize_config(config);
    REQUIRE(finalized);
    REQUIRE(finalized->defaults.version == "v2");
  }

  SECTION("DD_TAGS") {
    struct TestCase {
      std::string name;
      std::string dd_tags;
      std::unordered_map<std::string, std::string> expected_tags;
      std::optional<Error::Code> expected_error;
    };

    auto test_case = GENERATE(values<TestCase>({
        {"empty", "", {}, std::nullopt},
        {"missing colon", "foo", {}, Error::TAG_MISSING_SEPARATOR},
        {"trailing comma",
         "foo:bar, baz:123,",
         {},
         Error::TAG_MISSING_SEPARATOR},
        {"overwrite value", "foo:baz", {{"foo", "baz"}}, std::nullopt},
        {"additional values",
         "baz:123, bam:three",
         {{"baz", "123"}, {"bam", "three"}},
         std::nullopt},
        {"commas optional",
         "baz:123 bam:three",
         {{"baz", "123"}, {"bam", "three"}},
         std::nullopt},
        {"last one wins",
         "baz:123 baz:three",
         {{"baz", "three"}},
         std::nullopt},
    }));

    // This will be overriden by the DD_TAGS environment variable.
    config.defaults.tags = {{"foo", "bar"}};
    config.defaults.service = "required";

    CAPTURE(test_case.name);
    const EnvGuard guard{"DD_TAGS", test_case.dd_tags};
    auto finalized = finalize_config(config);
    if (test_case.expected_error) {
      REQUIRE(!finalized);
      REQUIRE(finalized.error().code == *test_case.expected_error);

    } else {
      REQUIRE(finalized);
      REQUIRE(finalized->defaults.tags == test_case.expected_tags);
    }
  }
}

TEST_CASE("TracerConfig::log_on_startup") {
  TracerConfig config;
  config.defaults.service = "testsvc";
  const auto logger = std::make_shared<MockLogger>();
  config.logger = logger;

  SECTION("default is true") {
    {
      auto finalized = finalize_config(config);
      REQUIRE(finalized);
      const Tracer tracer{*finalized};
      (void)tracer;
    }
    REQUIRE(logger->startup_count() == 1);
    // This check is weak, but better than nothing.
    REQUIRE(logger->first_startup().size() > 0);
  }

  SECTION("false silences the startup message") {
    {
      config.log_on_startup = false;
      auto finalized = finalize_config(config);
      REQUIRE(finalized);
      const Tracer tracer{*finalized};
      (void)tracer;
    }
    REQUIRE(logger->startup_count() == 0);
  }

  SECTION("overridden by DD_TRACE_STARTUP_LOGS") {
    struct TestCase {
      std::string name;
      std::string dd_trace_startup_logs;
      bool expect_startup_log;
    };

    auto test_case = GENERATE(values<TestCase>({
        {"DD_TRACE_STARTUP_LOGS=''", "", true},
        {"DD_TRACE_STARTUP_LOGS='0'", "0", false},
        {"DD_TRACE_STARTUP_LOGS='false'", "false", false},
        {"DD_TRACE_STARTUP_LOGS='FaLsE'", "FaLsE", false},
        {"DD_TRACE_STARTUP_LOGS='no'", "no", false},
        {"DD_TRACE_STARTUP_LOGS='n'", "n", true},
        {"DD_TRACE_STARTUP_LOGS='1'", "1", true},
        {"DD_TRACE_STARTUP_LOGS='true'", "true", true},
        {"DD_TRACE_STARTUP_LOGS='goldfish'", "goldfish", true},
    }));

    CAPTURE(test_case.name);
    const EnvGuard guard{"DD_TRACE_STARTUP_LOGS",
                         test_case.dd_trace_startup_logs};
    {
      auto finalized = finalize_config(config);
      REQUIRE(finalized);
      const Tracer tracer{*finalized};
      (void)tracer;
    }
    REQUIRE(logger->startup_count() == int(test_case.expect_startup_log));
  }
}

TEST_CASE("TracerConfig::report_traces") {
  TracerConfig config;
  config.defaults.service = "testsvc";
  const auto collector = std::make_shared<MockCollector>();
  config.collector = collector;
  config.logger = std::make_shared<NullLogger>();

  SECTION("default is true") {
    {
      auto finalized = finalize_config(config);
      REQUIRE(finalized);
      Tracer tracer{*finalized};
      auto span = tracer.create_span();
      (void)span;
    }
    REQUIRE(collector->chunks.size() == 1);
    REQUIRE(collector->chunks.front().size() == 1);
  }

  SECTION("false disables collection") {
    {
      config.report_traces = false;
      auto finalized = finalize_config(config);
      REQUIRE(finalized);
      Tracer tracer{*finalized};
      auto span = tracer.create_span();
      (void)span;
    }
    REQUIRE(collector->chunks.size() == 0);
  }

  SECTION("overridden by DD_TRACE_ENABLED") {
    struct TestCase {
      std::string name;
      std::string dd_trace_enabled;
      bool original_value;
      bool expect_spans;
    };

    auto test_case = GENERATE(values<TestCase>({
        {"falsy override ('false')", "false", true, false},
        {"falsy override ('0')", "0", true, false},
        {"falsy consistent ('false')", "false", false, false},
        {"falsy consistent ('0')", "0", false, false},
        {"truthy override ('true')", "true", false, true},
        {"truthy override ('1')", "1", false, true},
        {"truthy consistent ('true')", "true", true, true},
        {"truthy consistent ('1')", "1", true, true},
    }));

    CAPTURE(test_case.name);
    const EnvGuard guard{"DD_TRACE_ENABLED", test_case.dd_trace_enabled};
    config.report_traces = test_case.original_value;
    {
      auto finalized = finalize_config(config);
      REQUIRE(finalized);
      Tracer tracer{*finalized};
      auto span = tracer.create_span();
      (void)span;
    }
    if (test_case.expect_spans) {
      REQUIRE(collector->chunks.size() == 1);
      REQUIRE(collector->chunks.front().size() == 1);
    } else {
      REQUIRE(collector->chunks.size() == 0);
    }
  }
}

TEST_CASE("TracerConfig::agent") {
  TracerConfig config;
  config.defaults.service = "testsvc";

  SECTION("event_scheduler") {
    SECTION("default") {
      auto finalized = finalize_config(config);
      REQUIRE(finalized);
      const auto* const agent =
          std::get_if<FinalizedDatadogAgentConfig>(&finalized->collector);
      REQUIRE(agent);
      REQUIRE(
          dynamic_cast<ThreadedEventScheduler*>(agent->event_scheduler.get()));
    }

    SECTION("custom") {
      auto scheduler = std::make_shared<MockEventScheduler>();
      config.agent.event_scheduler = scheduler;
      auto finalized = finalize_config(config);
      REQUIRE(finalized);
      const auto* const agent =
          std::get_if<FinalizedDatadogAgentConfig>(&finalized->collector);
      REQUIRE(agent);
      REQUIRE(agent->event_scheduler == scheduler);
    }
  }

  SECTION("flush interval") {
    SECTION("cannot be zero") {
      config.agent.flush_interval_milliseconds = 0;
      auto finalized = finalize_config(config);
      REQUIRE(!finalized);
      REQUIRE(finalized.error().code ==
              Error::DATADOG_AGENT_INVALID_FLUSH_INTERVAL);
    }

    SECTION("cannot be negative") {
      config.agent.flush_interval_milliseconds = -1337;
      auto finalized = finalize_config(config);
      REQUIRE(!finalized);
      REQUIRE(finalized.error().code ==
              Error::DATADOG_AGENT_INVALID_FLUSH_INTERVAL);
    }
  }

  SECTION("url") {
    SECTION("parsing") {
      struct TestCase {
        std::string url;
        std::optional<Error::Code> expected_error;
        std::string expected_scheme = "";
        std::string expected_authority = "";
        std::string expected_path = "";
      };

      auto test_case = GENERATE(values<TestCase>({
          {"http://dd-agent:8126", std::nullopt, "http", "dd-agent:8126", ""},
          {"http://dd-agent:8126/", std::nullopt, "http", "dd-agent:8126", "/"},
          {"https://dd-agent:8126/", std::nullopt, "https", "dd-agent:8126",
           "/"},
          {"unix:///var/run/datadog/trace-agent.sock", std::nullopt, "unix",
           "/var/run/datadog/trace-agent.sock"},
          {"unix://var/run/datadog/trace-agent.sock",
           Error::URL_UNIX_DOMAIN_SOCKET_PATH_NOT_ABSOLUTE},
          {"http+unix:///run/datadog/trace-agent.sock", std::nullopt,
           "http+unix", "/run/datadog/trace-agent.sock"},
          {"https+unix:///run/datadog/trace-agent.sock", std::nullopt,
           "https+unix", "/run/datadog/trace-agent.sock"},
          {"tcp://localhost:8126", Error::URL_UNSUPPORTED_SCHEME},
          {"/var/run/datadog/trace-agent.sock", Error::URL_MISSING_SEPARATOR},
      }));

      CAPTURE(test_case.url);
      config.agent.url = test_case.url;
      auto finalized = finalize_config(config);
      if (test_case.expected_error) {
        REQUIRE(!finalized);
        REQUIRE(finalized.error().code == *test_case.expected_error);
      } else {
        REQUIRE(finalized);
        const auto* const agent =
            std::get_if<FinalizedDatadogAgentConfig>(&finalized->collector);
        REQUIRE(agent);
        REQUIRE(agent->url.scheme == test_case.expected_scheme);
        REQUIRE(agent->url.authority == test_case.expected_authority);
        REQUIRE(agent->url.path == test_case.expected_path);
      }
    }

    SECTION("environment variables override") {
      struct TestCase {
        std::string name;
        std::optional<std::string> env_host;
        std::optional<std::string> env_port;
        std::optional<std::string> env_url;
        std::string expected_scheme;
        std::string expected_authority;
      };

      auto test_case = GENERATE(values<TestCase>({
          {"override host with default port", "dd-agent", x, x, "http",
           "dd-agent:8126"},
          {"override port and host", "dd-agent", "8080", x, "http",
           "dd-agent:8080"},
          {"override port with default host", x, "8080", x, "http",
           "localhost:8080"},
          // A bogus port number will cause an error in the TCPClient, not
          // during configuration.  For the purposes of configuration, any
          // value is accepted.
          {"we don't parse port", x, "bogus", x, "http", "localhost:bogus"},
          {"even empty is ok", x, "", x, "http", "localhost:"},
          {"URL", x, x, "http://dd-agent:8080", "http", "dd-agent:8080"},
          {"URL overrides scheme", x, x, "https://dd-agent:8080", "https",
           "dd-agent:8080"},
          {"URL overrides host", "localhost", x, "http://dd-agent:8080", "http",
           "dd-agent:8080"},
          {"URL overrides port", x, "8126", "http://dd-agent:8080", "http",
           "dd-agent:8080"},
          {"URL overrides port and host", "localhost", "8126",
           "http://dd-agent:8080", "http", "dd-agent:8080"},
      }));

      CAPTURE(test_case.name);
      std::optional<EnvGuard> host_guard;
      if (test_case.env_host) {
        host_guard.emplace("DD_AGENT_HOST", *test_case.env_host);
      }
      std::optional<EnvGuard> port_guard;
      if (test_case.env_port) {
        port_guard.emplace("DD_TRACE_AGENT_PORT", *test_case.env_port);
      }
      std::optional<EnvGuard> url_guard;
      if (test_case.env_url) {
        url_guard.emplace("DD_TRACE_AGENT_URL", *test_case.env_url);
      }

      auto finalized = finalize_config(config);
      REQUIRE(finalized);
      const auto* const agent =
          std::get_if<FinalizedDatadogAgentConfig>(&finalized->collector);
      REQUIRE(agent);
      REQUIRE(agent->url.scheme == test_case.expected_scheme);
      REQUIRE(agent->url.authority == test_case.expected_authority);
    }
  }
}

TEST_CASE("TracerConfig::trace_sampler") {
  TracerConfig config;
  config.defaults.service = "testsvc";

  SECTION("default is no rules") {
    auto finalized = finalize_config(config);
    REQUIRE(finalized);
    REQUIRE(finalized->trace_sampler.rules.size() == 0);
  }

  SECTION("one sampling rule") {
    auto& rules = config.trace_sampler.rules;
    rules.resize(rules.size() + 1);
    TraceSamplerConfig::Rule& rule = rules.back();

    SECTION("yields one sampling rule") {
      auto finalized = finalize_config(config);
      REQUIRE(finalized);
      REQUIRE(finalized->trace_sampler.rules.size() == 1);
      // and the default sample_rate is 100%
      REQUIRE(finalized->trace_sampler.rules.front().sample_rate == 1.0);
    }

    SECTION("has to have a valid sample_rate") {
      auto rate = GENERATE(std::nan(""), -0.5, 1.3,
                           std::numeric_limits<double>::infinity(),
                           -std::numeric_limits<double>::infinity(), 42);
      rule.sample_rate = rate;
      auto finalized = finalize_config(config);
      REQUIRE(!finalized);
      REQUIRE(finalized.error().code == Error::RATE_OUT_OF_RANGE);
    }
  }

  SECTION("two sampling rules") {
    auto& rules = config.trace_sampler.rules;
    rules.resize(rules.size() + 2);
    rules[0].sample_rate = 0.5;
    rules[1].sample_rate = 0.6;
    auto finalized = finalize_config(config);
    REQUIRE(finalized);
    REQUIRE(finalized->trace_sampler.rules.size() == 2);
    REQUIRE(finalized->trace_sampler.rules[0].sample_rate == 0.5);
    REQUIRE(finalized->trace_sampler.rules[1].sample_rate == 0.6);
  }

  SECTION("global sample_rate creates a catch-all rule") {
    config.trace_sampler.sample_rate = 0.25;
    auto finalized = finalize_config(config);
    REQUIRE(finalized);
    REQUIRE(finalized->trace_sampler.rules.size() == 1);
    const auto& rule = finalized->trace_sampler.rules.front();
    REQUIRE(rule.sample_rate == 0.25);
    REQUIRE(rule.service == "*");
    REQUIRE(rule.name == "*");
    REQUIRE(rule.resource == "*");
    REQUIRE(rule.tags.empty());
  }

  SECTION("DD_TRACE_SAMPLE_RATE") {
    SECTION("sets the global sample_rate") {
      const EnvGuard guard{"DD_TRACE_SAMPLE_RATE", "0.5"};
      auto finalized = finalize_config(config);
      REQUIRE(finalized);
      REQUIRE(finalized->trace_sampler.rules.size() == 1);
      REQUIRE(finalized->trace_sampler.rules.front().sample_rate == 0.5);
    }

    SECTION("overrides TraceSamplerConfig::sample_rate") {
      config.trace_sampler.sample_rate = 0.25;
      const EnvGuard guard{"DD_TRACE_SAMPLE_RATE", "0.5"};
      auto finalized = finalize_config(config);
      REQUIRE(finalized);
      REQUIRE(finalized->trace_sampler.rules.size() == 1);
      REQUIRE(finalized->trace_sampler.rules.front().sample_rate == 0.5);
    }

    SECTION("has to have a valid value") {
      struct TestCase {
        std::string name;
        std::string env_value;
        Error::Code expected_error;
      };

      auto test_case = GENERATE(values<TestCase>({
          {"empty", "", Error::INVALID_DOUBLE},
          {"nonsense", "nonsense", Error::INVALID_DOUBLE},
          {"trailing space", "0.23   ", Error::INVALID_DOUBLE},
          {"out of range of double", "123e9999999999", Error::INVALID_DOUBLE},
          {"NaN", "NaN", Error::INVALID_DOUBLE},
          {"nan", "nan", Error::INVALID_DOUBLE},
          {"inf", "inf", Error::INVALID_DOUBLE},
          {"Inf", "Inf", Error::INVALID_DOUBLE},
          {"below range", "-0.1", Error::RATE_OUT_OF_RANGE},
          {"above range", "1.1", Error::RATE_OUT_OF_RANGE},
      }));

      CAPTURE(test_case.name);

      const EnvGuard guard{"DD_TRACE_SAMPLE_RATE", test_case.env_value};
      auto finalized = finalize_config(config);
      REQUIRE(!finalized);
      REQUIRE(finalized.error().code == test_case.expected_error);
    }
  }

  SECTION("max_per_second") {
    SECTION("defaults to 200") {
      auto finalized = finalize_config(config);
      REQUIRE(finalized);
      REQUIRE(finalized->trace_sampler.max_per_second == 200);
    }

    SECTION("must be >0 and a finite number") {
      auto limit = GENERATE(0.0, -1.0, std::nan(""),
                            std::numeric_limits<double>::infinity(),
                            -std::numeric_limits<double>::infinity());

      CAPTURE(limit);
      CAPTURE(std::fpclassify(limit));

      config.trace_sampler.max_per_second = limit;
      auto finalized = finalize_config(config);
      REQUIRE(!finalized);
      REQUIRE(finalized.error().code == Error::MAX_PER_SECOND_OUT_OF_RANGE);
    }
  }

  SECTION("DD_TRACE_RATE_LIMIT") {
    SECTION("overrides SpanSamplerConfig::max_per_second") {
      const EnvGuard guard{"DD_TRACE_RATE_LIMIT", "120"};
      auto finalized = finalize_config(config);
      REQUIRE(finalized);
      REQUIRE(finalized->trace_sampler.max_per_second == 120);
    }

    SECTION("has to have a valid value") {
      struct TestCase {
        std::string name;
        std::string env_value;
        Error::Code expected_error;
      };

      auto test_case = GENERATE(values<TestCase>({
          {"empty", "", Error::INVALID_DOUBLE},
          {"nonsense", "nonsense", Error::INVALID_DOUBLE},
          {"trailing space", "23   ", Error::INVALID_DOUBLE},
          {"out of range of double", "123e9999999999", Error::INVALID_DOUBLE},
          {"NaN", "NaN", Error::INVALID_DOUBLE},
          {"nan", "nan", Error::INVALID_DOUBLE},
          {"inf", "inf", Error::INVALID_DOUBLE},
          {"Inf", "Inf", Error::INVALID_DOUBLE},
          {"below range", "-0.1", Error::MAX_PER_SECOND_OUT_OF_RANGE},
          {"zero (also below range)", "0", Error::MAX_PER_SECOND_OUT_OF_RANGE},
      }));

      CAPTURE(test_case.name);

      const EnvGuard guard{"DD_TRACE_RATE_LIMIT", test_case.env_value};
      auto finalized = finalize_config(config);
      REQUIRE(!finalized);
      REQUIRE(finalized.error().code == test_case.expected_error);
    }
  }

  SECTION("DD_TRACE_SAMPLING_RULES") {
    SECTION("sets sampling rules and overrides TraceSampler::rules") {
      TraceSamplerConfig::Rule config_rule;
      config_rule.service = "whatever";
      config.trace_sampler.rules.push_back(config_rule);

      auto rules_json = R"json([
        {"service": "poohbear", "name": "get.honey", "sample_rate": 0},
        {"tags": {"error": "*"}, "resource": "/admin/*"}
      ])json";

      const EnvGuard guard{"DD_TRACE_SAMPLING_RULES", rules_json};
      auto finalized = finalize_config(config);
      REQUIRE(finalized);

      const auto& rules = finalized->trace_sampler.rules;
      CAPTURE(rules_json);
      CAPTURE(rules);
      REQUIRE(rules.size() == 2);
      REQUIRE(rules[0].service == "poohbear");
      REQUIRE(rules[0].name == "get.honey");
      REQUIRE(rules[0].sample_rate == 0);
      REQUIRE(rules[0].tags.size() == 0);
      REQUIRE(rules[1].service == "*");
      REQUIRE(rules[1].name == "*");
      REQUIRE(rules[1].sample_rate == 1);
      REQUIRE(rules[1].tags.size() == 1);
      REQUIRE(rules[1].tags.at("error") == "*");
      REQUIRE(rules[1].resource == "/admin/*");
    }

    SECTION("must be valid") {
      struct TestCase {
        std::string name;
        std::string json;
        Error::Code expected_error;
      };

      auto test_case = GENERATE(values<TestCase>({
          {"invalid JSON", "this is clearly not JSON",
           Error::TRACE_SAMPLING_RULES_INVALID_JSON},
          {"barely not JSON", "[true,]",
           Error::TRACE_SAMPLING_RULES_INVALID_JSON},
          {"must be array",
           R"json({"service": "you forgot the square brackets"})json",
           Error::TRACE_SAMPLING_RULES_WRONG_TYPE},
          {"service must be a string", R"json([{"service": 123}])json",
           Error::RULE_PROPERTY_WRONG_TYPE},
          {"name must be a string", R"json([{"name": null}])json",
           Error::RULE_PROPERTY_WRONG_TYPE},
          {"resource must be a string", R"json([{"resource": false}])json",
           Error::RULE_PROPERTY_WRONG_TYPE},
          {"'tags' property must be an object",
           R"json([{"tags": ["foo:bar"]}])json",
           Error::RULE_PROPERTY_WRONG_TYPE},
          {"tag values must be strings",
           R"json([{"tags": {"foo": "two", "error": false}}])json",
           Error::RULE_TAG_WRONG_TYPE},
          {"each rule must be an object", R"json([["service", "wrong!"]])json",
           Error::RULE_WRONG_TYPE},
          {"sample_rate must be a number", R"json([{"sample_rate": true}])json",
           Error::TRACE_SAMPLING_RULES_SAMPLE_RATE_WRONG_TYPE},
          {"no unknown properties", R"json([{"extension": "denied!"}])json",
           Error::TRACE_SAMPLING_RULES_UNKNOWN_PROPERTY},
      }));

      CAPTURE(test_case.name);

      const EnvGuard guard{"DD_TRACE_SAMPLING_RULES", test_case.json};
      auto finalized = finalize_config(config);
      REQUIRE(!finalized);
      REQUIRE(finalized.error().code == test_case.expected_error);
    }
  }
}

TEST_CASE("TracerConfig::span_sampler") {
  TracerConfig config;
  config.defaults.service = "testsvc";

  SECTION("default is no rules") {
    auto finalized = finalize_config(config);
    REQUIRE(finalized);
    REQUIRE(finalized->span_sampler.rules.size() == 0);
  }

  SECTION("one sampling rule") {
    auto& rules = config.span_sampler.rules;
    rules.resize(rules.size() + 1);
    SpanSamplerConfig::Rule& rule = rules.back();

    SECTION("yields one sampling rule") {
      auto finalized = finalize_config(config);
      REQUIRE(finalized);
      REQUIRE(finalized->span_sampler.rules.size() == 1);
      // the default sample_rate is 100%
      REQUIRE(finalized->span_sampler.rules.front().sample_rate == 1.0);
      // the default max_per_second is null (unlimited)
      REQUIRE(!finalized->span_sampler.rules.front().max_per_second);
    }

    SECTION("has to have a valid sample_rate") {
      auto rate = GENERATE(std::nan(""), -0.5, 1.3,
                           std::numeric_limits<double>::infinity(),
                           -std::numeric_limits<double>::infinity(), 42);
      CAPTURE(rate);
      rule.sample_rate = rate;
      auto finalized = finalize_config(config);
      REQUIRE(!finalized);
      REQUIRE(finalized.error().code == Error::RATE_OUT_OF_RANGE);
    }

    SECTION("has to have a valid max_per_second (if not null)") {
      auto limit =
          GENERATE(0.0, -1.0, std::numeric_limits<double>::infinity(),
                   -std::numeric_limits<double>::infinity(), std::nan(""));
      CAPTURE(limit);
      rule.max_per_second = limit;
      auto finalized = finalize_config(config);
      REQUIRE(!finalized);
      REQUIRE(finalized.error().code == Error::MAX_PER_SECOND_OUT_OF_RANGE);
    }
  }

  SECTION("two sampling rules") {
    auto& rules = config.span_sampler.rules;
    rules.resize(rules.size() + 2);
    rules[0].sample_rate = 0.5;
    rules[1].sample_rate = 0.6;
    rules[1].max_per_second = 10;
    auto finalized = finalize_config(config);
    REQUIRE(finalized);
    REQUIRE(finalized->span_sampler.rules.size() == 2);
    REQUIRE(finalized->span_sampler.rules[0].sample_rate == 0.5);
    REQUIRE(!finalized->span_sampler.rules[0].max_per_second);
    REQUIRE(finalized->span_sampler.rules[1].sample_rate == 0.6);
    REQUIRE(finalized->span_sampler.rules[1].max_per_second == 10);
  }

  SECTION("DD_SPAN_SAMPLING_RULES") {
    SECTION(
        "sets the span sampling rules, and overrides "
        "TracerConfig::span_sampler.rules") {
      SpanSamplerConfig::Rule config_rule;
      config_rule.service = "foosvc";
      config_rule.max_per_second = 9.2;
      config.span_sampler.rules.push_back(config_rule);

      auto rules_json = R"json([
        {"name": "mysql2.query", "max_per_second": 100},
        {"max_per_second": 10, "sample_rate": 0.1}
      ])json";

      const EnvGuard guard{"DD_SPAN_SAMPLING_RULES", rules_json};
      auto finalized = finalize_config(config);
      REQUIRE(finalized);
      const auto& rules = finalized->span_sampler.rules;
      REQUIRE(rules.size() == 2);
      REQUIRE(rules[0].service == "*");
      REQUIRE(rules[0].name == "mysql2.query");
      REQUIRE(rules[0].resource == "*");
      REQUIRE(rules[0].sample_rate == 1.0);
      REQUIRE(rules[0].max_per_second == 100);
      REQUIRE(rules[1].service == "*");
      REQUIRE(rules[1].name == "*");
      REQUIRE(rules[1].resource == "*");
      REQUIRE(rules[1].max_per_second == 10);
      REQUIRE(rules[1].sample_rate == 0.1);
    }

    SECTION("must be valid") {
      struct TestCase {
        std::string name;
        std::string json;
        Error::Code expected_error;
      };

      auto test_case = GENERATE(values<TestCase>({
          {"invalid JSON", "this is clearly not JSON",
           Error::SPAN_SAMPLING_RULES_INVALID_JSON},
          {"barely not JSON", "[true,]",
           Error::SPAN_SAMPLING_RULES_INVALID_JSON},
          {"must be array",
           R"json({"service": "you forgot the square brackets"})json",
           Error::SPAN_SAMPLING_RULES_WRONG_TYPE},
          {"service must be a string", R"json([{"service": 123}])json",
           Error::RULE_PROPERTY_WRONG_TYPE},
          {"name must be a string", R"json([{"name": null}])json",
           Error::RULE_PROPERTY_WRONG_TYPE},
          {"resource must be a string", R"json([{"resource": false}])json",
           Error::RULE_PROPERTY_WRONG_TYPE},
          {"'tags' property must be an object",
           R"json([{"tags": ["foo:bar"]}])json",
           Error::RULE_PROPERTY_WRONG_TYPE},
          {"tag values must be strings",
           R"json([{"tags": {"foo": "two", "error": false}}])json",
           Error::RULE_TAG_WRONG_TYPE},
          {"each rule must be an object", R"json([["service", "wrong!"]])json",
           Error::RULE_WRONG_TYPE},
          {"sample_rate must be a number", R"json([{"sample_rate": true}])json",
           Error::SPAN_SAMPLING_RULES_SAMPLE_RATE_WRONG_TYPE},
          {"max_per_second must be a number (or absent)",
           R"json([{"max_per_second": false}])json",
           Error::SPAN_SAMPLING_RULES_MAX_PER_SECOND_WRONG_TYPE},
          {"no unknown properties", R"json([{"extension": "denied!"}])json",
           Error::SPAN_SAMPLING_RULES_UNKNOWN_PROPERTY},
      }));

      CAPTURE(test_case.name);

      const EnvGuard guard{"DD_SPAN_SAMPLING_RULES", test_case.json};
      auto finalized = finalize_config(config);
      REQUIRE(!finalized);
      REQUIRE(finalized.error().code == test_case.expected_error);
    }

    SECTION("DD_SPAN_SAMPLING_RULES_FILE") {
      SECTION("successful usage") {
        const auto logger = std::make_shared<MockLogger>();
        config.logger = logger;

        // This rule will be overridden.
        SpanSamplerConfig::Rule config_rule;
        config_rule.service = "foosvc";
        config_rule.max_per_second = 9.2;
        config.span_sampler.rules.push_back(config_rule);

        auto rules_file_json = R"json([
        {"name": "mysql2.query"},
        {"resource": "/admin*"},
        {"max_per_second": 10, "sample_rate": 0.1}
      ])json";

        SomewhatSecureTemporaryFile file;
        REQUIRE(file.is_open());
        file << rules_file_json;
        file.close();
        const EnvGuard guard{"DD_SPAN_SAMPLING_RULES_FILE",
                             file.path().string()};

        SECTION("overrides SpanSamplerConfig::rules") {
          auto finalized = finalize_config(config);
          REQUIRE(finalized);
          const auto& rules = finalized->span_sampler.rules;
          REQUIRE(rules.size() == 3);
          REQUIRE(rules[0].name == "mysql2.query");
          REQUIRE(rules[1].resource == "/admin*");
          REQUIRE(rules[2].max_per_second == 10);
          REQUIRE(rules[2].sample_rate == 0.1);
        }

        SECTION("doesn't override DD_SPAN_SAMPLING_RULES, but logs an error") {
          auto rules_json = R"json([
            {"name": "mysql2.query", "max_per_second": 100},
            {"max_per_second": 10, "sample_rate": 0.1}
          ])json";

          const EnvGuard guard{"DD_SPAN_SAMPLING_RULES", rules_json};
          auto finalized = finalize_config(config);
          REQUIRE(finalized);
          const auto& rules = finalized->span_sampler.rules;
          REQUIRE(rules.size() == 2);
          REQUIRE(rules[0].name == "mysql2.query");
          REQUIRE(rules[0].max_per_second == 100);
          REQUIRE(rules[1].max_per_second == 10);
          REQUIRE(rules[1].sample_rate == 0.1);

          REQUIRE(logger->error_count() == 1);
        }
      }

      SECTION("failed usage") {
        SECTION("unable to open") {
          std::filesystem::path defunct;
          {
            SomewhatSecureTemporaryFile file;
            REQUIRE(file.is_open());
            defunct = file.path();
          }
          const EnvGuard guard{"DD_SPAN_SAMPLING_RULES_FILE", defunct.string()};
          auto finalized = finalize_config(config);
          REQUIRE(!finalized);
          REQUIRE(finalized.error().code == Error::SPAN_SAMPLING_RULES_FILE_IO);
        }

        SECTION("unable to parse") {
          SomewhatSecureTemporaryFile file;
          REQUIRE(file.is_open());
          // We could do any of the failures tested in the "must be valid"
          // section, since it's the same parser. Instead, just to cover the
          // code path specific to DD_SPAN_SAMPLING_RULES_FILE, pick any error,
          // e.g. invalid JSON.
          file << "this is clearly not JSON";
          file.close();
          const EnvGuard guard{"DD_SPAN_SAMPLING_RULES_FILE",
                               file.path().string()};
          auto finalized = finalize_config(config);
          REQUIRE(!finalized);
          REQUIRE(finalized.error().code ==
                  Error::SPAN_SAMPLING_RULES_INVALID_JSON);
        }
      }
    }
  }
}

TEST_CASE("TracerConfig propagation styles") {
  TracerConfig config;
  config.defaults.service = "testsvc";

  SECTION("injection_styles") {
    SECTION("defaults to just Datadog") {
      auto finalized = finalize_config(config);
      REQUIRE(finalized);
      REQUIRE(finalized->injection_styles.datadog);
      REQUIRE(!finalized->injection_styles.b3);
    }

    SECTION("need at least one") {
      config.injection_styles.datadog = false;
      config.injection_styles.b3 = false;
      auto finalized = finalize_config(config);
      REQUIRE(!finalized);
      REQUIRE(finalized.error().code == Error::MISSING_SPAN_INJECTION_STYLE);
    }

    SECTION("DD_PROPAGATION_STYLE_INJECT") {
      SECTION("overrides injection_styles") {
        const EnvGuard guard{"DD_PROPAGATION_STYLE_INJECT", "B3"};
        auto finalized = finalize_config(config);
        REQUIRE(finalized);
        REQUIRE(!finalized->injection_styles.datadog);
        REQUIRE(finalized->injection_styles.b3);
      }

      SECTION("parsing") {
        struct TestCase {
          int line;
          std::string env_value;
          std::optional<Error::Code> expected_error;
          PropagationStyles expected_styles = PropagationStyles{};
        };

        // clang-format off
        auto test_case = GENERATE(values<TestCase>({
          {__LINE__, "Datadog", x, {true, false}},
          {__LINE__, "DaTaDoG", x, {true, false}},
          {__LINE__, "B3", x, {false, true}},
          {__LINE__, "b3", x, {false, true}},
          {__LINE__, "Datadog B3", x, {true, true}},
          {__LINE__, "B3 Datadog", x, {true, true}},
          {__LINE__, "b3 datadog", x, {true, true}},
          {__LINE__, "b3, datadog", x, {true, true}},
          {__LINE__, "b3,datadog", x, {true, true}},
          {__LINE__, "b3,             datadog", x, {true, true}},
          {__LINE__, "b3,,datadog", Error::UNKNOWN_PROPAGATION_STYLE},
          {__LINE__, "b3,datadog,w3c", Error::UNKNOWN_PROPAGATION_STYLE},
          {__LINE__, "b3,datadog,datadog", x, {true, true}},
          {__LINE__, "  b3 b3 b3, b3 , b3, b3, b3   , b3 b3 b3  ", x, {false, true}},
        }));
        // clang-format on

        CAPTURE(test_case.line);
        CAPTURE(test_case.env_value);

        const EnvGuard guard{"DD_PROPAGATION_STYLE_INJECT",
                             test_case.env_value};
        auto finalized = finalize_config(config);
        if (test_case.expected_error) {
          REQUIRE(!finalized);
          REQUIRE(finalized.error().code == *test_case.expected_error);
        } else {
          REQUIRE(finalized);
          REQUIRE(finalized->injection_styles.datadog ==
                  test_case.expected_styles.datadog);
          REQUIRE(finalized->injection_styles.b3 ==
                  test_case.expected_styles.b3);
        }
      }
    }
  }

  // This section is very much like "injection_styles", above.
  SECTION("extraction_styles") {
    SECTION("defaults to just Datadog") {
      auto finalized = finalize_config(config);
      REQUIRE(finalized);
      REQUIRE(finalized->extraction_styles.datadog);
      REQUIRE(!finalized->extraction_styles.b3);
    }

    SECTION("need at least one") {
      config.extraction_styles.datadog = false;
      config.extraction_styles.b3 = false;
      auto finalized = finalize_config(config);
      REQUIRE(!finalized);
      REQUIRE(finalized.error().code == Error::MISSING_SPAN_EXTRACTION_STYLE);
    }

    SECTION("DD_PROPAGATION_STYLE_EXTRACT") {
      SECTION("overrides extraction_styles") {
        const EnvGuard guard{"DD_PROPAGATION_STYLE_EXTRACT", "B3"};
        auto finalized = finalize_config(config);
        REQUIRE(finalized);
        REQUIRE(!finalized->extraction_styles.datadog);
        REQUIRE(finalized->extraction_styles.b3);
      }

      // It's the same as for injection styles, so let's omit most of the
      // section.  Keep only an example where parsing fails, so we cover the
      // error handling code in `TracerConfig`.
      SECTION("parsing failure") {
        const EnvGuard guard{"DD_PROPAGATION_STYLE_EXTRACT", "b3,,datadog"};
        auto finalized = finalize_config(config);
        REQUIRE(!finalized);
        REQUIRE(finalized.error().code == Error::UNKNOWN_PROPAGATION_STYLE);
      }
    }
  }
}
