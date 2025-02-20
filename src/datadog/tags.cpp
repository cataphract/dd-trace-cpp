#include "tags.h"

#include "parse_util.h"

namespace datadog {
namespace tracing {
namespace tags {

const std::string environment = "env";
const std::string service_name = "service.name";
const std::string span_type = "span.type";
const std::string operation_name = "operation";
const std::string resource_name = "resource.name";
const std::string manual_keep = "manual.keep";
const std::string manual_drop = "manual.drop";
const std::string version = "version";

namespace internal {

const std::string propagation_error = "_dd.propagation_error";
const std::string decision_maker = "_dd.p.dm";
const std::string origin = "_dd.origin";
const std::string hostname = "_dd.hostname";
const std::string sampling_priority = "_sampling_priority_v1";
const std::string rule_sample_rate = "_dd.rule_psr";
const std::string rule_limiter_sample_rate = "_dd.limit_psr";
const std::string agent_sample_rate = "_dd.agent_psr";
const std::string span_sampling_mechanism = "_dd.span_sampling.mechanism";
const std::string span_sampling_rule_rate = "_dd.span_sampling.rule_rate";
const std::string span_sampling_limit = "_dd.span_sampling.max_per_second";

}  // namespace internal

bool is_internal(std::string_view tag_name) {
  return starts_with(tag_name, "_dd.");
}

}  // namespace tags
}  // namespace tracing
}  // namespace datadog
