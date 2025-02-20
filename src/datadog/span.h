#pragma once

// This component defines a class, `Span`, that represents an extent of time in
// which some operation of interest occurs, such as an RPC request, database
// query, calculation, etc.
//
// `Span` objects are created by calling member functions on `Tracer` or on
// another `Span` object.  They are not instantiated directly.
//
// A `Span` has a start time, an end time, and a name (sometimes called its
// "operation name").  A span is associated with a service, a resource (such as
// the URL endpoint in an HTTP request), and arbitrary key/value string pairs
// known as tags.
//
// A `Span` can have at most one parent and can have zero or more children. The
// operation that a `Span` represents is a subtask of the operation that its
// parent represents, and the children of a `Span` represent subtasks of its
// operation.
//
// For example, an HTTP server might create a `Span` for each request processed.
// The `Span` begins when the server begins reading the request, and ends when
// the server has finished writing the response or reporting an error.  The
// first child of the request span might represent the reading and parsing of
// the HTTP request's headers.  The second child of the request span might
// represent the dispatch of the request handling to an endpoint-specific
// handler.  That child might itself have children, such as a database query or
// a request to an authentication service.
//
// The complete set of spans that are related to each other via the parent/child
// relationship is called a trace.
//
// A trace can extend across processes and networks via trace context
// propagation.  A `Span` can be _extracted_ from its external parent via
// `Tracer::extract_span`, and a `Span` can be _injected_ via `Span::inject`
// into an outside context from which its external children might be extracted.
//
// If an error occurs during the operation that a span represents, the error can
// be noted in the span via the `set_error` family of member functions.
//
// A `Span` is finished when it is destroyed.  The end time can be overridden
// via the `set_end_time` member function prior to the span's destruction.

#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <string_view>

#include "clock.h"
#include "error.h"
#include "id_generator.h"

namespace datadog {
namespace tracing {

class DictWriter;
struct SpanConfig;
struct SpanData;
class TraceSegment;

class Span {
  std::shared_ptr<TraceSegment> trace_segment_;
  SpanData* data_;
  IDGenerator generate_span_id_;
  Clock clock_;
  std::optional<std::chrono::steady_clock::time_point> end_time_;

 public:
  // Create a span whose properties are stored in the specified `data` and that
  // is associated with the specified `trace_segment`.  Optionally specify
  // `generate_span_id` to generate IDs of child spans, and a `clock` to
  // determine start and end times.  If `generate_span_id` and `clock` are not
  // specified`, then `default_id_generator` and `default_clock` are used
  // instead respectively.
  Span(SpanData* data, const std::shared_ptr<TraceSegment>& trace_segment,
       const IDGenerator& generate_span_id, const Clock& clock);
  Span(const Span&) = delete;
  Span(Span&&) = default;
  Span& operator=(Span&&) = default;

  // Finish this span and submit it to the associated trace segment.  If
  // `set_end_time` has not been called on this span, then set this span's end
  // time to the current time.
  ~Span();

  // Return a span that is a child of this span.  Use the optionally specified
  // `config` to determine the properties of the child span.  If `config` is not
  // specified, then the child span's properties are determined by the
  // `SpanDefaults` that were used to configure the `Tracer` to which this span
  // is related.  The child span's start time is the current time unless
  // overridden in `config`.
  Span create_child(const SpanConfig& config) const;
  Span create_child() const;

  // Return this span's ID (span ID).
  std::uint64_t id() const;
  // Return the ID of the trace of which this span is a part.
  std::uint64_t trace_id() const;
  // Return the ID of this span's parent span, or return null if this span has
  // no parent.
  std::optional<std::uint64_t> parent_id() const;
  // Return the start time of this span.
  TimePoint start_time() const;
  // Return whether this span has been marked as an error having occurred during
  // its extent.
  bool error() const;

  // Return the value of the tag having the specified `name`, or return null if
  // there is no such tag.
  std::optional<std::string_view> lookup_tag(std::string_view name) const;
  // Overwrite the tag having the specified `name` so that it has the specified
  // `value`, or create a new tag.
  void set_tag(std::string_view name, std::string_view value);
  // Delete the tag having the specified `name` if it exists.
  void remove_tag(std::string_view name);

  // Set the name of the service associated with this span, e.g.
  // "ingress-nginx-useast1".
  void set_service_name(std::string_view);
  // Set the type of the service associated with this span, e.g. "web".
  void set_service_type(std::string_view);
  // Set the name of the operation that this span represents, e.g.
  // "handle.request", "execute.query", or "healthcheck".
  void set_name(std::string_view);
  // Set the name of the resource associated with the operation that this span
  // represents, e.g. "/api/v1/info" or "select count(*) from users".
  void set_resource_name(std::string_view);
  // Set whether an error occurred during the extent of this span.  If `false`,
  // then error-related tags will be removed from this span as well.
  void set_error(bool);
  // Associate a message with the error that occurred during the extent of this
  // span.  This also has the effect of calling `set_error(true)`.
  void set_error_message(std::string_view);
  // Associate an error type with the error that occurred during the extent of
  // this span.  This also has the effect of calling `set_error(true)`.
  void set_error_type(std::string_view);
  // Associate a call stack with the error that occurred during the extent of
  // this span.  This also has the effect of calling `set_error(true)`.
  void set_error_stack(std::string_view);
  // Set end time of this span.  Doing so will override the default behavior of
  // using the current time in the destructor.
  void set_end_time(std::chrono::steady_clock::time_point);

  // Write information about this span and its trace into the specified `writer`
  // for purposes of trace propagation.
  void inject(DictWriter& writer) const;

  // Return a reference to this span's trace segment.  The trace segment has
  // member functions that effect the trace as a whole, such as
  // `TraceSegment::override_sampling_priority`.
  TraceSegment& trace_segment();
  const TraceSegment& trace_segment() const;
};

}  // namespace tracing
}  // namespace datadog
