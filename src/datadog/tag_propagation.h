#pragma once

// Some span tags are associated with the entire local trace, rather than just
// a single span within the trace.  These tags are added to the local root span
// before the trace is flushed.
//
// Among these root span tags, some are also propagated as trace context.
// Propagated tags are packaged into the "x-datadog-tags" header in a
// particular format (see the cpp file for a description of the format).
//
// This component provides serialization and deserialization routines for the
// "x-datadog-tags" header format.

#include <string>
#include <string_view>
#include <unordered_map>

#include "expected.h"

namespace datadog {
namespace tracing {

// Return a name->value mapping of tags parsed from the specified
// `header_value`, or return an `Error` if an error occurs.
Expected<std::unordered_map<std::string, std::string>> decode_tags(
    std::string_view header_value);

// Serialize the specified `trace_tags` into the propagation format and return
// the resulting string.
std::string encode_tags(
    const std::unordered_map<std::string, std::string>& trace_tags);

}  // namespace tracing
}  // namespace datadog
