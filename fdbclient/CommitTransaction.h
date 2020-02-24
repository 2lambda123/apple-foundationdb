/*
 * CommitTransaction.h
 *
 * This source file is part of the FoundationDB open source project
 *
 * Copyright 2013-2018 Apple Inc. and the FoundationDB project authors
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

#ifndef FLOW_FDBCLIENT_COMMITTRANSACTION_H
#define FLOW_FDBCLIENT_COMMITTRANSACTION_H
#pragma once

#include "fdbclient/FDBTypes.h"
#include "fdbclient/MutationRef.h"

// The versioned message has wire format : -1, version, messages
static const int32_t VERSION_HEADER = -1;


// A 'single key mutation' is one which affects exactly the value of the key specified by its param1
static inline bool isSingleKeyMutation(MutationRef::Type type) {
	return (MutationRef::SINGLE_KEY_MASK & (1<<type)) != 0;
}

// Returns true if the given type can be safely cast to MutationRef::Type and used as a parameter to
// isSingleKeyMutation, isAtomicOp, etc.  It does NOT mean that the type is a valid type of a MutationRef in any
// particular context.
static inline bool isValidMutationType(uint32_t type) {
	return (type < MutationRef::MAX_ATOMIC_OP);
}

// An 'atomic operation' is a single key mutation which sets the key specified by its param1 to a 
//   nontrivial function of the previous value of the key and param2, and thus requires a 
//   read/modify/write to implement.  (Basically a single key mutation other than a set)
static inline bool isAtomicOp(MutationRef::Type mutationType) {
	return (MutationRef::ATOMIC_MASK & (1<<mutationType)) != 0;
}

// Returns true for operations which do not obey the associative law (i.e. a*(b*c) == (a*b)*c) in all cases
// unless a, b, and c have equal lengths, in which case even these operations are associative.
static inline bool isNonAssociativeOp(MutationRef::Type mutationType) {
	return (MutationRef::NON_ASSOCIATIVE_MASK & (1<<mutationType)) != 0;
}

struct CommitTransactionRef {
	CommitTransactionRef() : read_snapshot(0) {}
	CommitTransactionRef(Arena &a, const CommitTransactionRef &from)
	  : read_conflict_ranges(a, from.read_conflict_ranges),
		write_conflict_ranges(a, from.write_conflict_ranges),
		mutations(a, from.mutations),
		read_snapshot(from.read_snapshot) {
	}
	VectorRef< KeyRangeRef > read_conflict_ranges;
	VectorRef< KeyRangeRef > write_conflict_ranges;
	VectorRef< MutationRef > mutations;
	Version read_snapshot;

	template <class Ar>
	force_inline void serialize( Ar& ar ) {
		serializer(ar, read_conflict_ranges, write_conflict_ranges, mutations, read_snapshot);
	}

	// Convenience for internal code required to manipulate these without the Native API
	void set( Arena& arena, KeyRef const& key, ValueRef const& value ) {
		mutations.push_back_deep(arena, MutationRef(MutationRef::SetValue, key, value));
		write_conflict_ranges.push_back(arena, singleKeyRange(key, arena));
	}

	void clear( Arena& arena, KeyRangeRef const& keys ) {
		mutations.push_back_deep(arena, MutationRef(MutationRef::ClearRange, keys.begin, keys.end));
		write_conflict_ranges.push_back_deep(arena, keys);
	}

	size_t expectedSize() const {
		return read_conflict_ranges.expectedSize() + write_conflict_ranges.expectedSize() + mutations.expectedSize();
	}
};

bool debugMutation( const char* context, Version version, MutationRef const& m );
bool debugKeyRange( const char* context, Version version, KeyRangeRef const& keyRange );

#endif
