/*
 * LockRange.actor.h
 *
 * This source file is part of the FoundationDB open source project
 *
 * Copyright 2013-2020 Apple Inc. and the FoundationDB project authors
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

#if defined(NO_INTELLISENSE) && !defined(FDBCLIENT_LOCKRANGE_ACTOR_G_H)
	#define FDBCLIENT_LOCKRANGE_ACTOR_G_H
	#include "fdbclient/LockRange.actor.g.h"
#elif !defined(FDBCLIENT_LOCKRANGE_ACTOR_H)
	#define FDBCLIENT_LOCKRANGE_ACTOR_H

#include <vector>
#include <map>

#include "fdbclient/NativeAPI.actor.h"
#include "fdbclient/FDBTypes.h"

#include "flow/actorcompiler.h" // has to be last include

// TODO: use LockRequest as parameters for lockRange(s) and unlockRange(s)

// (Un)Locks a range in the normal key space. If the database is already locked,
// then a database_locked error is thrown. If (part of) the range is already
// locked, then a range_locked error is thrown during commit.
ACTOR Future<Void> lockRange(Database cx, LockRequest request);
ACTOR Future<Void> lockRanges(Database cx, std::vector<LockRequest> requests);

class Transaction;

class RangeLockCache {
public:
	using Snapshot = Standalone<VectorRef<LockRequest>>;
	using SnapshotIterator = std::map<Version, Snapshot>::iterator;
	using Requests = Standalone<VectorRef<LockRequest>>;

	enum Reason {
		OK,
		DENIED_EXCLUSIVE_LOCK,
		DENIED_READ_LOCK, // Write access is denied because of read lock held
		DENIED_OLD_VERSION, // Request is denied because old lock version is used
		ALREADY_LOCKED, // Attempts to lock an already locked range
		ALREADY_UNLOCKED, // Attempts to release locks for an unlocked range
	};

	RangeLockCache() = default;

	// Adds lock requests for the given lock version.
	void add(Version version, const Requests& requests);
	void add(Version version, const LockRequest& request);

	// Expire cached snapshots or requests up to the given version.
	void expire(Version upTo);

	// Returns if the cache has data for the given lock version.
	bool hasVersion(Version version);

	// Returns if the key/range can be written/read for the given version.
	Reason check(KeyRef key, Version version, bool write = true);
	Reason check(KeyRangeRef range, Version version, bool write = true);
	Reason check(const LockRequest& request, Version version);

	// Client tries to add a lock request. If the request can proceed, reason
	// is set to OK and mutations are added to the transaction object.
	// Otherwise, reason gives the error and transaction object is intact.
	Reason tryAdd(Transaction* tr, const LockRequest& request);

	// Serializes all requests from the given version and on.
	Value getChanges(Version from);

	// PRE-CONDITION: hasVersion(version) must return true
	Snapshot getSnapshot(Version version);
	Value getSnapshotValue(Version version);

	void setSnapshot(Version version, Snapshot snapshot) {
		snapshots[version] = snapshot;
	}

	void setSnapshotValue(Version version, Value snapshot);

	// Returns snapshots & lock requests in string.
	std::string toString();

private:
	std::string toString(Version version, Snapshot snapshot);

	// Make sure a snapshot for the version exists.
	void ensureSnapshot(Version version);

	// From a base snapshot, builds the next snapshot and returns the iterator to it.
	SnapshotIterator buildSnapshot(SnapshotIterator it);

	// PRE-CONDITION: a and b do not overlap.
	static bool rangeLess(const KeyRangeRef& a, const KeyRangeRef& b) { return a.begin < b.begin; }

	// PRE-CONDITION: a and b do not overlap.
	static bool lockLess(const LockRequest& a, const LockRequest& b) { return rangeLess(a.range, b.range); }

	// A version ordered locked ranges
	std::map<Version, Snapshot> snapshots;
	std::map<Version, Requests> requests;

	Version lockVersion = invalidVersion; // the latest commit version of locks
	KeyRangeMap<LockStatus> locks; // locked key ranges
};

#include "flow/unactorcompiler.h"
#endif
