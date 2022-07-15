#ifndef TESTPROBE_H_
#define TESTPROBE_H_

#include "flow/Knobs.h"
#include "flow/Trace.h"

namespace probe {

struct ICodeProbe;

enum class AnnotationType { Decoration, Assertion, Context };
enum class ExecutionContext { Simulation, Net2 };

namespace context {
struct Net2 {
	constexpr static AnnotationType type = AnnotationType::Context;
	constexpr bool operator()(ExecutionContext context) const { return context == ExecutionContext::Net2; }
};
struct Sim2 {
	constexpr static AnnotationType type = AnnotationType::Context;
	constexpr bool operator()(ExecutionContext context) const { return context == ExecutionContext::Net2; }
};

constexpr Net2 net2;
constexpr Sim2 sim2;

template <class Left, class Right>
struct OrContext {
	typename std::remove_const<Left>::type left;
	typename std::remove_const<Right>::type right;
	constexpr OrContext(Left left, Right right) : left(left), right(right) {}
	constexpr bool operator()(ExecutionContext context) const { return left(context) || right(context); }
};

template <class Left, class Right>
constexpr std::enable_if_t<Left::type == AnnotationType::Context && Right::type == AnnotationType::Context,
                           OrContext<Left, Right>>
operator|(Left const& lhs, Right const& rhs) {
	return OrContext<Left, Right>(lhs, rhs);
}

} // namespace context

namespace assert {
struct NoSim {
	constexpr static AnnotationType type = AnnotationType::Assertion;
	bool operator()(ICodeProbe* self) const;
};
struct SimOnly {
	constexpr static AnnotationType type = AnnotationType::Assertion;
	bool operator()(ICodeProbe* self) const;
};

template <class Left, class Right>
struct AssertOr {
	typename std::remove_const<Left>::type left;
	typename std::remove_const<Right>::type right;
	constexpr AssertOr() {}
	constexpr bool operator()(ICodeProbe* self) const { return left(self) || right(self); }
};
template <class Left, class Right>
struct AssertAnd {
	typename std::remove_const<Left>::type left;
	typename std::remove_const<Right>::type right;
	constexpr AssertAnd() {}
	constexpr bool operator()(ICodeProbe* self) const { return left(self) && right(self); }
};
template <class T>
struct AssertNot {
	typename std::remove_const<T>::type other;
	constexpr bool operator()(ICodeProbe* self) const { return !other(self); }
};

template <class Left, class Right>
constexpr std::enable_if_t<Left::type == AnnotationType::Assertion && Right::type == AnnotationType::Assertion,
                           AssertOr<Left, Right>>
operator||(Left const& lhs, Right const& rhs) {
	return AssertOr<Left, Right>();
}
template <class Left, class Right>
constexpr std::enable_if_t<Left::type == AnnotationType::Assertion && Right::type == AnnotationType::Assertion,
                           AssertAnd<Left, Right>>
operator&&(Left const& lhs, Right const& rhs) {
	return AssertAnd<Left, Right>();
}

template <class T>
constexpr std::enable_if_t<T::type == AnnotationType::Assertion, AssertNot<T>> operator!(T const&) {
	return AssertNot<T>();
}

constexpr SimOnly simOnly;
constexpr auto noSim = !simOnly;

} // namespace assert

template <class... Args>
struct CodeProbeAnnotations;

template <>
struct CodeProbeAnnotations<> {
	static constexpr bool providesContext = false;
	void hit(ICodeProbe* self) {}
	void trace(const ICodeProbe*, BaseTraceEvent&, bool) const {}
	constexpr bool expectContext(ExecutionContext context, bool prevHadSomeContext = false) const {
		return !prevHadSomeContext;
	}
};

template <class Head, class... Tail>
struct CodeProbeAnnotations<Head, Tail...> {
	using HeadType = typename std::remove_const<Head>::type;
	using ChildType = CodeProbeAnnotations<Tail...>;

	static constexpr bool providesContext = HeadType::type == AnnotationType::Context || ChildType::providesContext;
	static_assert(HeadType::type != AnnotationType::Context || !ChildType::providesContext,
	              "Only one context annotation can be used");

	HeadType head;
	ChildType tail;

	void hit(ICodeProbe* self) {
		if constexpr (Head::type == AnnotationType::Assertion) {
			ASSERT(head(self));
		}
		tail.hit(self);
	}
	void trace(const ICodeProbe* self, BaseTraceEvent& evt, bool condition) const {
		if constexpr (Head::type == AnnotationType::Decoration) {
			head.trace(self, evt, condition);
		}
		tail.trace(self, evt, condition);
	}
	// This should behave like the following:
	// 1. If no context is passed in the code probe, we expect to see this in every context
	// 2. Otherwise we will return true iff the execution context we're looking for has been passed to the probe
	constexpr bool expectContext(ExecutionContext context, bool prevHadSomeContext = false) const {
		if constexpr (HeadType::type == AnnotationType::Context) {
			if constexpr (head(context)) {
				return true;
			} else {
				tail.expectContext(context, true);
			}
		} else {
			tail.expectContext(context, prevHadSomeContext);
		}
	}
};

struct ICodeProbe {
	virtual ~ICodeProbe();

	virtual const char* filename() const = 0;
	virtual unsigned line() const = 0;
	virtual const char* comment() const = 0;
	virtual const char* condition() const = 0;
	virtual const char* compilationUnit() const = 0;
	virtual void trace(bool) const = 0;
	virtual bool wasHit() const = 0;
	virtual unsigned hitCount() const = 0;

	static void printProbesXML();
	static void printProbesJSON();
};

void registerProbe(ICodeProbe const& probe);
void printMissedProbes();

template <class FileName, class Condition, class Comment, class CompUnit, unsigned Line, class Annotations>
struct CodeProbeImpl : ICodeProbe {
	static CodeProbeImpl* instancePtr() { return &_instance; }
	static CodeProbeImpl& instance() { return _instance; }
	void hit() {
		if (_hitCount++ == 0) {
			trace(true);
		}
		annotations.hit(this);
	}

	void trace(bool condition) const override {
		TraceEvent evt(intToSeverity(FLOW_KNOBS->CODE_COV_TRACE_EVENT_SEVERITY), "CodeCoverage");
		evt.detail("File", FileName::value())
		    .detail("Line", Line)
		    .detail("Condition", Condition::value())
		    .detail("ProbeHit", condition)
		    .detail("Comment", Comment::value());
		annotations.trace(this, evt, condition);
	}
	bool wasHit() const override { return _hitCount > 0; }
	unsigned hitCount() const override { return _hitCount; }

	const char* filename() const override { return FileName::value(); }
	unsigned line() const override { return Line; }
	const char* comment() const override { return Comment::value(); }
	const char* condition() const override { return Condition::value(); }
	const char* compilationUnit() const override { return CompUnit::value(); }

private:
	CodeProbeImpl() { registerProbe(*this); }
	inline static CodeProbeImpl _instance;
	unsigned _hitCount = 0;
	Annotations annotations;
};

template <class FileName, class Condition, class Comment, class CompUnit, unsigned Line, class... Annotations>
CodeProbeImpl<FileName, Condition, Comment, CompUnit, Line, CodeProbeAnnotations<Annotations...>>& probeInstance(
    Annotations&... annotations) {
	return CodeProbeImpl<FileName, Condition, Comment, CompUnit, Line, CodeProbeAnnotations<Annotations...>>::
	    instance();
}

} // namespace probe

#ifdef COMPILATION_UNIT
#define CODE_PROBE_QUOTE(x) #x
#define CODE_PROBE_EXPAND_AND_QUOTE(x) CODE_PROBE_QUOTE(x)
#define CODE_PROBE_COMPILATION_UNIT CODE_PROBE_EXPAND_AND_QUOTE(COMPILATION_UNIT)
#else
#define CODE_PROBE_COMPILATION_UNIT "COMPILATION_UNIT not set"
#endif

#define _CODE_PROBE_IMPL(file, line, condition, comment, compUnit, fileType, condType, commentType, compUnitType, ...) \
	struct fileType {                                                                                                  \
		constexpr static const char* value() { return file; }                                                          \
	};                                                                                                                 \
	struct condType {                                                                                                  \
		constexpr static const char* value() { return #condition; }                                                    \
	};                                                                                                                 \
	struct commentType {                                                                                               \
		constexpr static const char* value() { return comment; }                                                       \
	};                                                                                                                 \
	struct compUnitType {                                                                                              \
		constexpr static const char* value() { return compUnit; }                                                      \
	};                                                                                                                 \
	if (condition) {                                                                                                   \
		probe::probeInstance<fileType, condType, commentType, compUnitType, line>(__VA_ARGS__).hit();                  \
	}

#define _CODE_PROBE_T2(type, counter) type##counter
#define _CODE_PROBE_T(type, counter) _CODE_PROBE_T2(type, counter)

#define CODE_PROBE(condition, comment, ...)                                                                            \
	do {                                                                                                               \
		_CODE_PROBE_IMPL(__FILE__,                                                                                     \
		                 __LINE__,                                                                                     \
		                 condition,                                                                                    \
		                 comment,                                                                                      \
		                 CODE_PROBE_COMPILATION_UNIT,                                                                  \
		                 _CODE_PROBE_T(FileType, __COUNTER__),                                                         \
		                 _CODE_PROBE_T(CondType, __COUNTER__),                                                         \
		                 _CODE_PROBE_T(CommentType, __COUNTER__),                                                      \
		                 _CODE_PROBE_T(CompilationUnitType, __COUNTER__),                                              \
		                 __VA_ARGS__)                                                                                  \
	} while (false)

#endif // TESTPROBE_H_
