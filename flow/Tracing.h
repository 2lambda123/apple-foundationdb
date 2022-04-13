/*
 * Tracing.h
 *
 * This source file is part of the FoundationDB open source project
 *
 * Copyright 2013-2022 Apple Inc. and the FoundationDB project authors
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#pragma once

#include "fdbclient/FDBTypes.h"
#include "flow/IRandom.h"
#include <unordered_set>
#include <atomic>

struct Location {
	StringRef name;
};

inline Location operator"" _loc(const char* str, size_t size) {
	return Location{ StringRef(reinterpret_cast<const uint8_t*>(str), size) };
}

enum class TraceFlags : uint8_t { unsampled = 0b00000000, sampled = 0b00000001 };

inline TraceFlags operator&(TraceFlags lhs, TraceFlags rhs) {
	return static_cast<TraceFlags>(static_cast<std::underlying_type_t<TraceFlags>>(lhs) &
	                               static_cast<std::underlying_type_t<TraceFlags>>(rhs));
}

struct SpanContext {
	UID traceID;
	uint64_t spanID;
	TraceFlags m_Flags;
	SpanContext() : traceID(UID()), spanID(0), m_Flags(TraceFlags::unsampled) {}
	SpanContext(UID traceID, uint64_t spanID, TraceFlags flags) : traceID(traceID), spanID(spanID), m_Flags(flags) {}
	SpanContext(UID traceID, uint64_t spanID) : traceID(traceID), spanID(spanID), m_Flags(TraceFlags::unsampled) {}
	SpanContext(Arena arena, const SpanContext& span)
	  : traceID(span.traceID), spanID(span.spanID), m_Flags(span.m_Flags) {}
	bool isSampled() const { return (m_Flags & TraceFlags::sampled) == TraceFlags::sampled; }
	std::string toString() const { return format("%016llx%016llx%016llx", traceID.first(), traceID.second(), spanID); };
	bool isValid() const { return traceID.first() != 0 && traceID.second() != 0; }

	template <class Ar>
	void serialize(Ar& ar) {
		serializer(ar, traceID, spanID, m_Flags);
	}

	// template <class Ar>
	// void serialize_unversioned(
	//     Ar& ar) { 
	// 	serializer(ar, traceID, spanID, m_Flags);
	// }

	// template <class Ar>
	// void load(Ar& ar, SpanContext& spanContext) {
	// 	spanContext.serialize_unversioned(ar);
	// }

	// template <class Ar>
	// void save(Ar& ar, SpanContext const& spanContext) {
	// 	const_cast<SpanContext&>(spanContext).serialize_unversioned(ar);
	// }
};

// Span
//
// Span is a tracing implementation which, for the most part, complies with the W3C Trace Context specification
// https://www.w3.org/TR/trace-context/ and the OpenTelemetry API
// https://github.com/open-telemetry/opentelemetry-specification/blob/main/specification/trace/api.md.
//
// The major differences between Span and the 7.0 Span implementation, which is based off the OpenTracing.io
// specification https://opentracing.io/ are as follows.
// https://github.com/open-telemetry/opentelemetry-specification/blob/main/specification/trace/api.md#span
//
// OpenTelemetry Spans have...
// 1. A SpanContext which consists of 3 attributes.
//
// TraceId - A valid trace identifier is a 16-byte array with at least one non-zero byte.
// SpanId - A valid span identifier is an 8-byte array with at least one non-zero byte.
// TraceFlags - 1 byte, bit field for flags.
//
// TraceState is not implemented, specifically we do not provide some of the following APIs
// https://www.w3.org/TR/trace-context/#mutating-the-tracestate-field In particular APIs to delete/update a specific,
// arbitrary key/value pair, as this complies with the OTEL specification where SpanContexts are immutable.
// 2. A begin/end and those values are serialized, unlike the Span implementation which has an end but serializes with a
// begin and calculated duration field.
// 3. A SpanKind
// https://github.com/open-telemetry/opentelemetry-specification/blob/main/specification/trace/api.md#spankind
// 4. A SpanStatus
// https://github.com/open-telemetry/opentelemetry-specification/blob/main/specification/trace/api.md#set-status
// 5. A singular parent SpanContext, which may optionally be null, as opposed to our Span implementation which allows
// for a list of parents.
// 6. An "attributes" rather than "tags", however the implementation is essentially the same, a set of key/value of
// strings, stored here as a SmallVectorRef<KeyValueRef> rather than map as a convenience.
// https://github.com/open-telemetry/opentelemetry-specification/blob/main/specification/common/common.md#attributes
// 7. An optional list of linked SpanContexts.
// https://github.com/open-telemetry/opentelemetry-specification/blob/main/specification/trace/api.md#specifying-links
// 8. An optional list of timestamped Events.
// https://github.com/open-telemetry/opentelemetry-specification/blob/main/specification/trace/api.md#add-events

enum class SpanKind : uint8_t { INTERNAL = 0, CLIENT = 1, SERVER = 2, PRODUCER = 3, CONSUMER = 4 };

enum class SpanStatus : uint8_t { UNSET = 0, OK = 1, ERR = 2 };

struct SpanEventRef {
	SpanEventRef() {}
	SpanEventRef(const StringRef& name,
	             const double& time,
	             const SmallVectorRef<KeyValueRef>& attributes = SmallVectorRef<KeyValueRef>())
	  : name(name), time(time), attributes(attributes) {}
	SpanEventRef(Arena& arena, const SpanEventRef& other)
	  : name(arena, other.name), time(other.time), attributes(arena, other.attributes) {}
	StringRef name;
	double time = 0.0;
	SmallVectorRef<KeyValueRef> attributes;
};

class Span {
public:
	Span(const SpanContext& context,
	     const Location& location,
	     const SpanContext& parentContext,
	     const std::initializer_list<SpanContext>& links = {})
	  : context(context), location(location), parentContext(parentContext), links(arena, links.begin(), links.end()),
	    begin(g_network->now()) {
		// We've simplified the logic here, essentially we're now always setting trace and span ids and relying on the
		// TraceFlags to determine if we're sampling. Therefore if the parent is sampled, we simply overwrite this
		// span's traceID with the parent trace id.
		if (parentContext.isSampled()) {
			this->context.traceID = UID(parentContext.traceID.first(), parentContext.traceID.second());
			this->context.m_Flags = TraceFlags::sampled;
		} else {
			// However there are two other cases.
			// 1. A legitamite parent span exists but it was not selected for tracing.
			// 2. There is no actual parent, just a default arg parent provided by the constructor AND the "child" span
			// was selected for sampling. For case 1. we handle below by marking the child as unsampled. For case 2 we
			// needn't do anything, and can rely on the values in this OTELSpan
			if (parentContext.traceID.first() != 0 && parentContext.traceID.second() != 0 &&
			    parentContext.spanID != 0) {
				this->context.m_Flags = TraceFlags::unsampled;
			}
		}
		this->kind = SpanKind::SERVER;
		this->status = SpanStatus::OK;
		this->attributes.push_back(
		    this->arena, KeyValueRef("address"_sr, StringRef(this->arena, g_network->getLocalAddress().toString())));
	}

	Span(const Location& location,
	     const SpanContext& parent = SpanContext(),
	     const std::initializer_list<SpanContext>& links = {})
	  : Span(SpanContext(UID(deterministicRandom()->randomUInt64(), deterministicRandom()->randomUInt64()), // traceID
	                     deterministicRandom()->randomUInt64(), // spanID
	                     deterministicRandom()->random01() < FLOW_KNOBS->TRACING_SAMPLE_RATE // sampled or unsampled
	                         ? TraceFlags::sampled
	                         : TraceFlags::unsampled),
	         location,
	         parent,
	         links) {}

	Span(const SpanContext& context, const Location& location) : Span(context, location, SpanContext()) {}

	Span(const Location& location, const SpanContext parent, const SpanContext& link)
	  : Span(location, parent, { link }) {}

	// Span(const Location& location) : Span(location, SpanContext()) {}
	//  NOTE: This constructor is primarly for unit testing until we sort out how to enable/disable a Knob dynamically
	//  in a test.
	Span(const Location& location,
	     const std::function<double()>& rateProvider,
	     const SpanContext& parent = SpanContext(),
	     const std::initializer_list<SpanContext>& links = {})
	  : Span(SpanContext(UID(deterministicRandom()->randomUInt64(), deterministicRandom()->randomUInt64()),
	                     deterministicRandom()->randomUInt64(),
	                     deterministicRandom()->random01() < rateProvider() ? TraceFlags::sampled
	                                                                        : TraceFlags::unsampled),
	         location,
	         parent,
	         links) {}

	Span(const Span&) = delete;
	Span(Span&& o) {
		arena = std::move(o.arena);
		context = o.context;
		location = o.location;
		parentContext = std::move(o.parentContext);
		kind = o.kind;
		begin = o.begin;
		end = o.end;
		links = std::move(o.links);
		events = std::move(o.events);
		status = o.status;
		o.context = SpanContext();
		o.parentContext = SpanContext();
		o.kind = SpanKind::INTERNAL;
		o.begin = 0.0;
		o.end = 0.0;
		o.status = SpanStatus::UNSET;
	}
	Span() {}
	~Span();
	Span& operator=(Span&& o);
	Span& operator=(const Span&) = delete;
	void swap(Span& other) {
		std::swap(arena, other.arena);
		std::swap(context, other.context);
		std::swap(location, other.location);
		std::swap(parentContext, other.parentContext);
		std::swap(kind, other.kind);
		std::swap(status, other.status);
		std::swap(begin, other.begin);
		std::swap(end, other.end);
		std::swap(links, other.links);
		std::swap(events, other.events);
	}

	Span& addLink(const SpanContext& linkContext) {
		links.push_back(arena, linkContext);
		return *this;
	}

	Span& addLinks(const std::initializer_list<SpanContext>& linkContexts = {}) {
		for (auto const& sc : linkContexts) {
			links.push_back(arena, sc);
		}
		return *this;
	}

	Span& addEvent(const SpanEventRef& event) {
		events.push_back_deep(arena, event);
		return *this;
	}

	Span& addEvent(const StringRef& name,
	               const double& time,
	               const SmallVectorRef<KeyValueRef>& attrs = SmallVectorRef<KeyValueRef>()) {
		return addEvent(SpanEventRef(name, time, attrs));
	}

	Span& addAttribute(const StringRef& key, const StringRef& value) {
		attributes.push_back_deep(arena, KeyValueRef(key, value));
		return *this;
	}

	Span& addParentOrLink(const SpanContext& other) {
		if (!parentContext.isValid()) {
			parentContext = other;
		} else {
			links.push_back(arena, other);
		}
		return *this;
	}

	Arena arena;
	SpanContext context;
	Location location;
	SpanContext parentContext;
	SpanKind kind;
	SmallVectorRef<SpanContext> links;
	double begin = 0.0, end = 0.0;
	SmallVectorRef<KeyValueRef> attributes; // not necessarily sorted
	SmallVectorRef<SpanEventRef> events;
	SpanStatus status;
};

// The user selects a tracer using a string passed to fdbserver on boot.
// Clients should not refer to TracerType directly, and mappings of names to
// values in this enum can change without notice.
enum class TracerType {
	DISABLED = 0,
	NETWORK_LOSSY = 1,
	SIM_END = 2, // Any tracers that come after SIM_END will not be tested in simulation
	LOG_FILE = 3
};

struct ITracer {
	virtual ~ITracer();
	virtual TracerType type() const = 0;
	// passed ownership to the tracer
	// virtual void trace(Span const& span) = 0;
	virtual void trace(Span const& span) = 0;
};

void openTracer(TracerType type);

template <class T>
struct SpannedDeque : Deque<T> {
	Span span;
	explicit SpannedDeque(Location loc) : span(loc) {}
	SpannedDeque(SpannedDeque&& other) : Deque<T>(std::move(other)), span(std::move(other.span)) {}
	SpannedDeque(SpannedDeque const&) = delete;
	SpannedDeque& operator=(SpannedDeque const&) = delete;
	SpannedDeque& operator=(SpannedDeque&& other) {
		*static_cast<Deque<T>*>(this) = std::move(other);
		span = std::move(other.span);
	}
};
