#pragma once

// This component provides a `struct`, `SpanData`, that contains all data fields
// relevant to `Span`. `SpanData` is what is consumed by `Collector`.

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

#include "clock.h"
#include "expected.h"

namespace datadog {
namespace tracing {

struct SpanConfig;
struct SpanDefaults;

struct SpanData {
  std::string service;
  std::string service_type;
  std::string name;
  std::string resource;
  std::uint64_t trace_id = 0;
  std::uint64_t span_id = 0;
  std::uint64_t parent_id = 0;
  TimePoint start;
  Duration duration = Duration::zero();
  bool error = false;
  std::unordered_map<std::string, std::string> tags;
  std::unordered_map<std::string, double> numeric_tags;

  std::optional<std::string_view> environment() const;
  std::optional<std::string_view> version() const;

  // Modify the properties of this object to honor the specified `config` and
  // `defaults`.  The properties of `config`, if set, override the properties of
  // `defaults`. Use the specified `clock` to provide a start none of none is
  // specified in `config`.
  void apply_config(const SpanDefaults& defaults, const SpanConfig& config,
                    const Clock& clock);
};

// Append to the specified `destination` the MessagePack representation of the
// specified `span`.
Expected<void> msgpack_encode(std::string& destination, const SpanData& span);

}  // namespace tracing
}  // namespace datadog
