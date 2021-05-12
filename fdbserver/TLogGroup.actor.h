/*
 * TLogGroup.h
 *
 * This source file is part of the FoundationDB open source project
 *
 * Copyright 2013-2021 Apple Inc. and the FoundationDB project authors
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

// When actually compiled (NO_INTELLISENSE), include the generated version of
// this file. In intellisense use the source version.
#if defined(NO_INTELLISENSE) && !defined(FDBSERVER_TLOGROUP_ACTOR_G_H)
#define FDBSERVER_TLOGROUP_ACTOR_G_H
#include "fdbserver/TLogGroup.actor.g.h"
#elif !defined(FDBSERVER_TLOGROUP_ACTOR_H)
#define FDBSERVER_TLOGROUP_ACTOR_H

#include <unordered_map>
#include <vector>

#include "fdbclient/CommitProxyInterface.h"
#include "fdbclient/NativeAPI.actor.h"
#include "fdbserver/IKeyValueStore.h"
#include "fdbrpc/Locality.h"
#include "fdbserver/WorkerInterface.actor.h"
#include "flow/FastRef.h"
#include "flow/IRandom.h"
#include "flow/network.h"

#include "flow/actorcompiler.h" // This must be the last #include.

// TODO: Monitor the groups, and if new tLogs need to added/removed, as workers are removed/added.
// TODO: Add unit-test for serialization.

struct TLogWorkerData;
class TLogGroup;
class TLogGroupCollection;

typedef Reference<TLogWorkerData> TLogWorkerDataRef;
typedef Reference<TLogGroup> TLogGroupRef;
typedef Reference<TLogGroupCollection> TLogGroupCollectionRef;

// `TLogGroupCollection` manages, recruits and tracks all the TLogGroups in the system.
// TODO: TLogGroupCollection for HA (satellite and remote), either same class or separate.
class TLogGroupCollection : public ReferenceCounted<TLogGroupCollection> {
public:
	// Construct a TLogGroupCollection, where each group has 'groupSize' servers and satifies
	// the contraints set by ReplicaitonPolicy 'policy'
	explicit TLogGroupCollection(const Reference<IReplicationPolicy>& policy, int numGroups, int groupSize);

	// Returns list of groups recruited by this collection.
	const std::vector<TLogGroupRef>& groups() const;

	// Returns the size of each TLogGroup.
	int groupSize() const;

	// Returns the number of TLogGroups we want to keep in collection. May be not be
	// equal to number of groups currently recruited/active.
	int targetGroupSize() const;

	// Add 'logWorkers' to current collection of workers that can be recruited into a TLogGroup.
	void addWorkers(const std::vector<WorkerInterface>& logWorkers);

	// Build a collection of groups and recruit workers into each group as per the ReplicationPolicy
	// and group size set in the parent class.
	void recruitEverything();

	// Add mutations to store state to given txnStoreState transaction request 'recoveryCommitReq'.
	void storeState(CommitTransactionRequest* recoveryCommitReq);

	// Loads TLogGroupCollection state from given IKeyValueStore, which will be txnStoreState passed
	// by master.
	void loadState(const Standalone<RangeResultRef>& store);

private:
	// Returns a LocalityMap of all the workers inside 'recruitMap', but ignore the workers
	// given in 'ignoreServers'.
	LocalityMap<TLogWorkerData> buildLocalityMap(const std::unordered_set<UID>& ignoreServers);

	// ReplicationPolicy defined for this collection. The members of group must satisfy
	// this replication policy, or else will not be part of a group.
	const Reference<IReplicationPolicy> policy;

	// Size of each group, set once during intialization.
	const int GROUP_SIZE;

	// Number of groups the collection is configured to recruit.
	int targetNumGroups;

	// List of TLogGroup's managed by this collection.
	std::vector<TLogGroupRef> recruitedGroups;

	// A map from UID or workers to their corresponding TLogWorkerData objects.
	// This map contains both recruited and unrecruited workers.
	std::unordered_map<UID, TLogWorkerDataRef> recruitMap;
};

// Represents a single TLogGroup which consists of TLog workers.
class TLogGroup : public ReferenceCounted<TLogGroup> {
public:
	explicit TLogGroup() : groupId(deterministicRandom()->randomUniqueID()) {}
	explicit TLogGroup(const UID& groupId) : groupId(groupId) {}

	const UID& id() const { return groupId; }

	// Add 'workerData' to this group.
	void addServer(const TLogWorkerDataRef& workerData);

	// Returns list of servers that are recruited for this group.
	std::vector<TLogWorkerDataRef> servers() const;

	// Returns the number of servers recruited in this group, including failed ones.
	int size() const;

	Standalone<StringRef> toValue() const;

	static TLogGroupRef fromValue(UID groupId,
	                              StringRef value,
	                              const std::unordered_map<UID, TLogWorkerDataRef>& recruits);

	std::vector<UID> serverIds() const;

	std::string toString() const;

private:
	const UID groupId;

	// Map from worker UID to TLogWorkerData
	// TODO: Can be an unordered_set.
	std::unordered_map<UID, TLogWorkerDataRef> serverMap;
};

// Represents an individual TLogWorker in this collection. A TLogGroup is a set of TLogWorkerData.
struct TLogWorkerData : public ReferenceCounted<TLogWorkerData> {
	const UID id;

	// Locality associated with the current worker.
	const LocalityData locality;
	const NetworkAddress address;

	TLogWorkerData(const UID& id, const NetworkAddress& addr, const LocalityData& locality)
	  : id(id), address(addr), locality(locality) {}

	// Converts a WorkerInterface to TLogWorkerData.
	static TLogWorkerDataRef fromInterface(const WorkerInterface& interf) {
		return makeReference<TLogWorkerData>(interf.id(), interf.address(), interf.locality);
	}

	// Returns user-readable string representation of this object.
	std::string toString() const;

	bool operator==(const TLogWorkerData& other) const {
		// TODO: Is NetworkAddress enough?
		return other.id == id;
	}
};

// User-defined hash function for TLogWorkerData and TLogGroup.
namespace std {
template <>
struct hash<TLogWorkerData> {
	std::size_t operator()(const TLogWorkerData& w) const noexcept { return w.id.hash(); }
};

template <>
struct hash<TLogGroup> {
	std::size_t operator()(const TLogGroup& w) const noexcept { return w.id().hash(); }
};
} // namespace std

#include "flow/unactorcompiler.h"
#endif /* FDBSERVER_TLOGROUP_ACTOR_H */
