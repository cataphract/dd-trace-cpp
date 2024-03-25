#pragma once

// This component provides a struct, `TracerConfig`, used to configure a
// `Tracer`.  `Tracer` is instantiated with a `FinalizedTracerConfig`, which
// must be obtained from the result of a call to `finalize_config`.

#include <cstddef>
#include <memory>
#include <variant>
#include <vector>

#include "clock.h"
#include "config.h"
#include "datadog_agent_config.h"
#include "error.h"
#include "expected.h"
#include "propagation_style.h"
#include "runtime_id.h"
#include "span_defaults.h"
#include "span_sampler_config.h"
#include "trace_sampler_config.h"

namespace datadog {
namespace tracing {

class Collector;
class Logger;
class SpanSampler;
class TraceSampler;

struct TracerConfig {
  // Set the service name.
  //
  // Overriden by the `DD_SERVICE` environment variables.
  Optional<std::string> service;

  // Set the type of service.
  Optional<std::string> service_type;

  // Set the application environment.
  //
  // Overriden by the `DD_ENV` environment variable.
  // Example: `prod`, `pre-prod` or `staging`.
  Optional<std::string> environment;

  // Set the service version.
  //
  // Overriden by the `DD_VERSION` environment variable.
  // Example values: `1.2.3`, `6c44da20`, `2020.02.13`.
  Optional<std::string> version;

  // Set the default name for spans.
  Optional<std::string> name;

  // Set global tags to be attached to every span.
  //
  // Overriden by the `DD_TAGS` environment variable.
  Optional<std::unordered_map<std::string, std::string>> tags;

  // `agent` configures a `DatadogAgent` collector instance.  See
  // `datadog_agent_config.h`.  Note that `agent` is ignored if `collector` is
  // set or if `report_traces` is `false`.
  DatadogAgentConfig agent;

  // `collector` is a `Collector` instance that the tracer will use to report
  // traces to Datadog.  If `collector` is null, then a `DatadogAgent` instance
  // will be created using the `agent` configuration.  Note that `collector` is
  // ignored if `report_traces` is `false`.
  std::shared_ptr<Collector> collector;

  // `report_traces` indicates whether traces generated by the tracer will be
  // sent to a collector (`true`) or discarded on completion (`false`).  If
  // `report_traces` is `false`, then both `agent` and `collector` are ignored.
  // `report_traces` is overridden by the `DD_TRACE_ENABLED` environment
  // variable.
  Optional<bool> report_traces;

  // `report_telemetry` indicates whether telemetry about the tracer will be
  // sent to a collector (`true`) or discarded on completion (`false`).  If
  // `report_telemetry` is `false`, then this feature is disabled.
  // `report_telemetry` is overridden by the
  // `DD_INSTRUMENTATION_TELEMETRY_ENABLED` environment variable.
  Optional<bool> report_telemetry;

  // `delegate_trace_sampling` indicates whether the tracer will consult a child
  // service for a trace sampling decision, and prefer the resulting decision
  // over its own, if appropriate.
  Optional<bool> delegate_trace_sampling;

  // `trace_sampler` configures trace sampling.  Trace sampling determines which
  // traces are sent to Datadog.  See `trace_sampler_config.h`.
  TraceSamplerConfig trace_sampler;

  // `span_sampler` configures span sampling.  Span sampling allows specified
  // spans to be sent to Datadog even when their enclosing trace is dropped by
  // the trace sampler.  See `span_sampler_config.h`.
  SpanSamplerConfig span_sampler;

  // `injection_styles` indicates with which tracing systems trace propagation
  // will be compatible when injecting (sending) trace context.
  // All styles indicated by `injection_styles` are used for injection.
  // `injection_styles` is overridden by the `DD_TRACE_PROPAGATION_STYLE_INJECT`
  // and `DD_TRACE_PROPAGATION_STYLE` environment variables.
  Optional<std::vector<PropagationStyle>> injection_styles;

  // `extraction_styles` indicates with which tracing systems trace propagation
  // will be compatible when extracting (receiving) trace context.
  // Extraction styles are applied in the order in which they appear in
  // `extraction_styles`. The first style that produces trace context or
  // produces an error determines the result of extraction.
  // `extraction_styles` is overridden by the
  // `DD_TRACE_PROPAGATION_STYLE_EXTRACT` and `DD_TRACE_PROPAGATION_STYLE`
  // environment variables.
  Optional<std::vector<PropagationStyle>> extraction_styles;

  // `report_hostname` indicates whether the tracer will include the result of
  // `gethostname` with traces sent to the collector.
  Optional<bool> report_hostname;

  // `max_tags_header_size` is the maximum allowed size, in bytes, of the
  // serialized value of the "X-Datadog-Tags" header used when injecting trace
  // context for propagation.  If the serialized value of the header would
  // exceed `tags_header_size`, the header will be omitted instead.
  Optional<std::size_t> max_tags_header_size;

  // `logger` specifies how the tracer will issue diagnostic messages.  If
  // `logger` is null, then it defaults to a logger that inserts into
  // `std::cerr`.
  std::shared_ptr<Logger> logger;

  // `log_on_startup` indicates whether the tracer will log a banner of
  // configuration information once initialized.
  // `log_on_startup` is overridden by the `DD_TRACE_STARTUP_LOGS` environment
  // variable.
  Optional<bool> log_on_startup;

  // `trace_id_128_bit` indicates whether the tracer will generate 128-bit trace
  // IDs.  If true, the tracer will generate 128-bit trace IDs. If false, the
  // tracer will generate 64-bit trace IDs. `trace_id_128_bit` is overridden by
  // the `DD_TRACE_128_BIT_TRACEID_GENERATION_ENABLED` environment variable.
  Optional<bool> generate_128bit_trace_ids;

  // `runtime_id` denotes the current run of the application in which the tracer
  // is embedded. If `runtime_id` is not specified, then it defaults to a
  // pseudo-randomly generated value. A server that contains multiple tracers,
  // such as those in the worker threads/processes of a reverse proxy, might
  // specify the same `runtime_id` for all tracer instances in the same run.
  Optional<RuntimeID> runtime_id;

  // `integration_name` is the name of the product integrating this library.
  // Example: "nginx", "envoy" or "istio".
  Optional<std::string> integration_name;
  // `integration_version` is the version of the product integrating this
  // library.
  // Example: "1.2.3", "6c44da20", "2020.02.13"
  Optional<std::string> integration_version;

  // This field allows for overriding the service name origin to default.
  //
  // Mainly exist for integration configurations purposes.
  // For instance, the default service name for the nginx integration will
  // resolve as 'nginx'. Without this customization, it would be reported as a
  // programmatic value in Datadog's Active Configuration, whereas it is
  // actually the default value for the integration.
  Optional<bool> report_service_as_default;
};

// `FinalizedTracerConfig` contains `Tracer` implementation details derived from
// a valid `TracerConfig` and accompanying environment.
// `FinalizedTracerConfig` must be obtained by calling `finalize_config`.
class FinalizedTracerConfig final {
  friend Expected<FinalizedTracerConfig> finalize_config(
      TracerConfig& config, const Clock& clock);
  FinalizedTracerConfig() = default;

 public:
  SpanDefaults defaults;

  std::variant<std::monostate, FinalizedDatadogAgentConfig,
               std::shared_ptr<Collector>>
      collector;

  FinalizedTraceSamplerConfig trace_sampler;
  FinalizedSpanSamplerConfig span_sampler;

  std::vector<PropagationStyle> injection_styles;
  std::vector<PropagationStyle> extraction_styles;

  bool report_hostname;
  std::size_t tags_header_size;
  std::shared_ptr<Logger> logger;
  bool log_on_startup;
  bool generate_128bit_trace_ids;
  bool report_telemetry;
  Optional<RuntimeID> runtime_id;
  Clock clock;
  std::string integration_name;
  std::string integration_version;
  bool delegate_trace_sampling;
  bool report_traces;
  std::unordered_map<ConfigName, ConfigMetadata> metadata;
};

// Return a `FinalizedTracerConfig` from the specified `config` and from any
// relevant environment variables.  If any configuration is invalid, return an
// `Error`.
// Optionally specify a `clock` used to calculate span start times, span
// durations, and timeouts.  If `clock` is not specified, then `default_clock`
// is used.
Expected<FinalizedTracerConfig> finalize_config(TracerConfig& config);
Expected<FinalizedTracerConfig> finalize_config(TracerConfig& config,
                                                const Clock& clock);

}  // namespace tracing
}  // namespace datadog
