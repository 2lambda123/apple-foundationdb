/*
 * MetaclusterMoveWorkload.actor.cpp
 *
 * This source file is part of the FoundationDB open source project
 *
 * Copyright 2013-2023 Apple Inc. and the FoundationDB project authors
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
#include <cstdint>
#include <limits>
#include "fdbclient/BackupAgent.actor.h"
#include "fdbclient/ClusterConnectionMemoryRecord.h"
#include "fdbclient/FDBOptions.g.h"
#include "fdbclient/FDBTypes.h"
#include "fdbclient/MultiVersionTransaction.h"
#include "fdbclient/NativeAPI.actor.h"
#include "fdbclient/ReadYourWrites.h"
#include "fdbclient/RunTransaction.actor.h"
#include "fdbclient/ThreadSafeTransaction.h"
#include "fdbrpc/TenantName.h"
#include "fdbrpc/simulator.h"
#include "fdbserver/workloads/workloads.actor.h"
#include "fdbserver/workloads/BulkSetup.actor.h"
#include "fdbserver/Knobs.h"
#include "flow/Error.h"
#include "flow/IRandom.h"
#include "flow/ThreadHelper.actor.h"
#include "flow/flow.h"

#include "metacluster/Metacluster.h"
#include "metacluster/MetaclusterConsistency.actor.h"
#include "metacluster/MetaclusterData.actor.h"
#include "metacluster/MetaclusterMetadata.h"
#include "metacluster/MetaclusterMove.actor.h"

#include "flow/actorcompiler.h" // This must be the last #include.

struct MetaclusterMoveWorkload : TestWorkload {
	static constexpr auto NAME = "MetaclusterMove";

	struct DataClusterData {
		Database db;
		std::set<int64_t> tenants;
		std::set<TenantGroupName> tenantGroups;

		DataClusterData() {}
		DataClusterData(Database db) : db(db) {}
	};

	struct TestTenantData {

		TenantName name;
		ClusterName cluster;
		Optional<TenantGroupName> tenantGroup;

		TestTenantData() {}
		TestTenantData(TenantName name, ClusterName cluster, Optional<TenantGroupName> tenantGroup)
		  : name(name), cluster(cluster), tenantGroup(tenantGroup) {}
	};

	struct TenantGroupData {
		ClusterName cluster;
		std::set<int64_t> tenants;
	};

	int nodeCount;
	double transactionsPerSecond;
	Key keyPrefix;

	Reference<IDatabase> managementDb;
	std::map<ClusterName, DataClusterData> dataDbs;
	std::vector<ClusterName> dataDbIndex;

	std::map<int64_t, TestTenantData> createdTenants;
	std::map<TenantName, int64_t> tenantNameIndex;
	std::map<TenantGroupName, TenantGroupData> tenantGroups;

	int initialTenants;
	int maxTenants;
	int maxTenantGroups;
	int tenantGroupCapacity;

	metacluster::metadata::management::MovementRecord moveRecord;

	MetaclusterMoveWorkload(WorkloadContext const& wcx) : TestWorkload(wcx) {
		transactionsPerSecond = getOption(options, "transactionsPerSecond"_sr, 5000.0) / clientCount;
		nodeCount = getOption(options, "nodeCount"_sr, transactionsPerSecond * clientCount);
		keyPrefix = unprintable(getOption(options, "keyPrefix"_sr, ""_sr).toString());
		maxTenants =
		    deterministicRandom()->randomInt(1, std::min<int>(1e8 - 1, getOption(options, "maxTenants"_sr, 100)) + 1);
		initialTenants = std::min<int>(maxTenants, getOption(options, "initialTenants"_sr, 40));
		maxTenantGroups = deterministicRandom()->randomInt(
		    1, std::min<int>(2 * maxTenants, getOption(options, "maxTenantGroups"_sr, 20)) + 1);
		tenantGroupCapacity =
		    std::max<int>(1, (initialTenants / 2 + maxTenantGroups - 1) / g_simulator->extraDatabases.size());
	}

	ClusterName chooseClusterName() { return dataDbIndex[deterministicRandom()->randomInt(0, dataDbIndex.size())]; }

	TenantName chooseTenantName() {
		TenantName tenant(format("tenant%08d", deterministicRandom()->randomInt(0, maxTenants)));
		return tenant;
	}

	Optional<TenantGroupName> chooseTenantGroup(Optional<ClusterName> cluster = Optional<ClusterName>()) {
		Optional<TenantGroupName> tenantGroup;
		if (deterministicRandom()->coinflip()) {
			if (!cluster.present()) {
				tenantGroup =
				    TenantGroupNameRef(format("tenantgroup%08d", deterministicRandom()->randomInt(0, maxTenantGroups)));
			} else {
				auto const& existingGroups = dataDbs[cluster.get()].tenantGroups;
				if (deterministicRandom()->coinflip() && !existingGroups.empty()) {
					tenantGroup = deterministicRandom()->randomChoice(
					    std::vector<TenantGroupName>(existingGroups.begin(), existingGroups.end()));
				} else if (tenantGroups.size() < maxTenantGroups) {
					do {
						tenantGroup = TenantGroupNameRef(
						    format("tenantgroup%08d", deterministicRandom()->randomInt(0, maxTenantGroups)));
					} while (tenantGroups.count(tenantGroup.get()) > 0);
				}
			}
		}

		return tenantGroup;
	}

	// Used to gradually increase capacity so that the tenants are somewhat evenly distributed across the clusters
	ACTOR static Future<Void> increaseMetaclusterCapacity(MetaclusterMoveWorkload* self) {
		self->tenantGroupCapacity = ceil(self->tenantGroupCapacity * 1.2);
		state Reference<ITransaction> tr = self->managementDb->createTransaction();
		loop {
			try {
				tr->setOption(FDBTransactionOptions::ACCESS_SYSTEM_KEYS);
				state int dbIndex;
				for (dbIndex = 0; dbIndex < self->dataDbIndex.size(); ++dbIndex) {
					metacluster::DataClusterMetadata clusterMetadata =
					    wait(metacluster::getClusterTransaction(tr, self->dataDbIndex[dbIndex]));
					metacluster::DataClusterEntry updatedEntry = clusterMetadata.entry;
					updatedEntry.capacity.numTenantGroups = self->tenantGroupCapacity;
					metacluster::updateClusterMetadata(
					    tr, self->dataDbIndex[dbIndex], clusterMetadata, {}, updatedEntry);
				}
				wait(safeThreadFutureToFuture(tr->commit()));
				break;
			} catch (Error& e) {
				wait(safeThreadFutureToFuture(tr->onError(e)));
			}
		}

		return Void();
	}

	ACTOR static Future<Void> createTenant(MetaclusterMoveWorkload* self) {
		state TenantName tenantName;
		for (int i = 0; i < 10; ++i) {
			tenantName = self->chooseTenantName();
			if (self->tenantNameIndex.count(tenantName) == 0) {
				break;
			}
		}

		if (self->tenantNameIndex.count(tenantName)) {
			return Void();
		}

		loop {
			try {
				metacluster::MetaclusterTenantMapEntry tenantEntry;
				tenantEntry.tenantName = tenantName;
				tenantEntry.tenantGroup = self->chooseTenantGroup();
				wait(metacluster::createTenant(self->managementDb,
				                               tenantEntry,
				                               metacluster::AssignClusterAutomatically::True,
				                               metacluster::IgnoreCapacityLimit::False));
				metacluster::MetaclusterTenantMapEntry createdEntry =
				    wait(metacluster::getTenant(self->managementDb, tenantName));
				TraceEvent(SevDebug, "MetaclusterMoveWorkloadCreatedTenant")
				    .detail("Tenant", tenantName)
				    .detail("TenantId", createdEntry.id);
				self->createdTenants[createdEntry.id] =
				    TestTenantData(tenantName, createdEntry.assignedCluster, createdEntry.tenantGroup);
				self->tenantNameIndex[tenantName] = createdEntry.id;
				auto& dataDb = self->dataDbs[createdEntry.assignedCluster];
				dataDb.tenants.insert(createdEntry.id);
				if (createdEntry.tenantGroup.present()) {
					auto& tenantGroupData = self->tenantGroups[createdEntry.tenantGroup.get()];
					tenantGroupData.cluster = createdEntry.assignedCluster;
					tenantGroupData.tenants.insert(createdEntry.id);
					dataDb.tenantGroups.insert(createdEntry.tenantGroup.get());
				}
				return Void();
			} catch (Error& e) {
				if (e.code() != error_code_metacluster_no_capacity) {
					throw;
				}

				wait(increaseMetaclusterCapacity(self));
			}
		}
	}

	ACTOR static Future<Void> startMove(MetaclusterMoveWorkload* self,
	                                    TenantGroupName tenantGroup,
	                                    ClusterName srcCluster,
	                                    ClusterName dstCluster) {
		state int tries = 0;
		state int retryLimit = 5;
		try {
			loop {
				Future<Void> startFuture =
				    metacluster::startTenantMovement(self->managementDb, tenantGroup, srcCluster, dstCluster);
				Optional<Void> result = wait(timeout(startFuture, deterministicRandom()->randomInt(1, 30)));
				if (result.present()) {
					TraceEvent(SevDebug, "MetaclusterMoveStartComplete")
					    .detail("TenantGroup", tenantGroup)
					    .detail("SourceCluster", srcCluster)
					    .detail("DestinationCluster", dstCluster);
					// potentially attempt retries even on success
					// to show idempotence
					break;
				}
				if (++tries == retryLimit) {
					throw operation_failed();
				}
				CODE_PROBE(true, "Metacluster move start timed out");
			}
		} catch (Error& e) {
			if (e.code() == error_code_invalid_tenant_move) {
				TraceEvent("MetaclusterMoveWorkloadStartFailed")
				    .detail("TenantGroup", tenantGroup)
				    .detail("SourceCluster", srcCluster)
				    .detail("DestinationCluster", dstCluster);
			}
		}
		return Void();
	}

	ACTOR static Future<Void> switchMove(MetaclusterMoveWorkload* self,
	                                     TenantGroupName tenantGroup,
	                                     ClusterName srcCluster,
	                                     ClusterName dstCluster) {
		try {
			wait(metacluster::switchTenantMovement(self->managementDb, tenantGroup, srcCluster, dstCluster));
		} catch (Error& e) {
			if (e.code() == error_code_invalid_tenant_move) {
				TraceEvent("MetaclusterMoveWorkloadSwitchFailed")
				    .detail("TenantGroup", tenantGroup)
				    .detail("SourceCluster", srcCluster)
				    .detail("DestinationCluster", dstCluster);
			}
			throw e;
		}
		return Void();
	}

	ACTOR static Future<Void> finishMove(MetaclusterMoveWorkload* self,
	                                     TenantGroupName tenantGroup,
	                                     ClusterName srcCluster,
	                                     ClusterName dstCluster) {
		try {
			wait(metacluster::finishTenantMovement(self->managementDb, tenantGroup, srcCluster, dstCluster));
		} catch (Error& e) {
			if (e.code() == error_code_invalid_tenant_move) {
				TraceEvent("MetaclusterMoveWorkloadFinishFailed")
				    .detail("TenantGroup", tenantGroup)
				    .detail("SourceCluster", srcCluster)
				    .detail("DestinationCluster", dstCluster);
			}
			throw e;
		}
		return Void();
	}

	ACTOR static Future<Void> _setup(Database cx, MetaclusterMoveWorkload* self) {
		metacluster::DataClusterEntry clusterEntry;
		clusterEntry.capacity.numTenantGroups = self->tenantGroupCapacity;

		metacluster::util::SimulatedMetacluster simMetacluster = wait(metacluster::util::createSimulatedMetacluster(
		    cx,
		    deterministicRandom()->randomInt(TenantAPI::TENANT_ID_PREFIX_MIN_VALUE,
		                                     TenantAPI::TENANT_ID_PREFIX_MAX_VALUE + 1),
		    clusterEntry));

		self->managementDb = simMetacluster.managementDb;
		ASSERT(!simMetacluster.dataDbs.empty());
		for (auto const& [name, db] : simMetacluster.dataDbs) {
			self->dataDbs[name] = DataClusterData(db);
			self->dataDbIndex.push_back(name);
		}

		TraceEvent(SevDebug, "MetaclusterMoveWorkloadCreateTenants").detail("NumTenants", self->initialTenants);

		while (self->createdTenants.size() < self->initialTenants) {
			wait(createTenant(self));
		}

		TraceEvent(SevDebug, "MetaclusterMoveWorkloadCreateTenantsComplete");

		// container of range-based for with continuation must be a state variable
		state std::map<ClusterName, DataClusterData> dataDbs = self->dataDbs;
		for (auto const& [name, dataDb] : dataDbs) {
			std::vector<Reference<Tenant>> dataTenants;
			// Iterate over each data cluster and attempt to fill some of the tenants with data
			for (auto const& tId : dataDb.tenants) {
				dataTenants.push_back(makeReference<Tenant>(tId));
			}
			wait(bulkSetup(dataDb.db,
			               self,
			               10000,
			               Promise<double>(),
			               false,
			               0.0,
			               1e12,
			               std::vector<uint64_t>(),
			               Promise<std::vector<std::pair<uint64_t, double>>>(),
			               0,
			               0.1,
			               0,
			               0,
			               dataTenants));
		}

		TraceEvent(SevDebug, "MetaclusterMoveWorkloadPopulateTenantDataComplete");

		return Void();
	}

	ACTOR static Future<Void> copyTenantData(Database cx,
	                                         MetaclusterMoveWorkload* self,
	                                         TenantGroupName tenantGroup,
	                                         Database srcDb,
	                                         Database dstDb) {
		ClusterName srcName = self->moveRecord.srcCluster;
		ClusterName dstName = self->moveRecord.dstCluster;
		TenantGroupData groupData = self->tenantGroups[tenantGroup];
		KeyRangeRef normalKeys(""_sr, "\xff"_sr);

		// container of range-based for with continuation must be a state variable
		state std::set<int64_t> tenants = groupData.tenants;
		for (auto const& tId : tenants) {
			state Reference<ReadYourWritesTransaction> srcTr =
			    makeReference<ReadYourWritesTransaction>(srcDb, makeReference<Tenant>(tId));
			state Reference<ReadYourWritesTransaction> dstTr =
			    makeReference<ReadYourWritesTransaction>(dstDb, makeReference<Tenant>(tId));
			srcTr->setOption(FDBTransactionOptions::LOCK_AWARE);
			dstTr->setOption(FDBTransactionOptions::LOCK_AWARE);
			state RangeResult srcRange = wait(srcTr->getRange(normalKeys, 0));
			try {
				for (const auto& [k, v] : srcRange) {
					dstTr->set(k, v);
				}
				wait(dstTr->commit());
			} catch (Error& e) {
				wait(dstTr->onError(e));
			}
		}
		return Void();
	}

	ACTOR static Future<Void> _start(Database cx, MetaclusterMoveWorkload* self) {
		state ClusterName srcCluster = self->chooseClusterName();
		state ClusterName dstCluster = self->chooseClusterName();
		// Expect an error if the same cluster is picked

		Optional<TenantGroupName> optionalTenantGroup = self->chooseTenantGroup(srcCluster);
		while (!optionalTenantGroup.present()) {
			optionalTenantGroup = self->chooseTenantGroup(srcCluster);
		}
		state TenantGroupName tenantGroup = optionalTenantGroup.get();
		state Reference<ITransaction> tr = self->managementDb->createTransaction();
		state Database srcDb = self->dataDbs[srcCluster].db;
		state Database dstDb = self->dataDbs[dstCluster].db;
		loop {
			try {
				wait(startMove(self, tenantGroup, srcCluster, dstCluster));
				// If start completes successfully, the move identifier should be written
				tr->setOption(FDBTransactionOptions::READ_SYSTEM_KEYS);
				Optional<metacluster::metadata::management::MovementRecord> optionalMr = wait(
				    metacluster::metadata::management::emergency_movement::emergencyMovements().get(tr, tenantGroup));
				ASSERT(optionalMr.present());
				self->moveRecord = optionalMr.get();
				wait(copyTenantData(cx, self, tenantGroup, srcDb, dstDb));
				wait(switchMove(self, tenantGroup, srcCluster, dstCluster));
				wait(finishMove(self, tenantGroup, srcCluster, dstCluster));
				break;
			} catch (Error& e) {
				state Error err(e);
				TraceEvent("MetaclusterMoveWorkloadError").error(err);
				if (err.code() == error_code_invalid_tenant_move) {
					if (srcCluster == dstCluster) {
						TraceEvent("MetaclusterMoveWorkloadSameSrcDst")
						    .detail("TenantGroup", tenantGroup)
						    .detail("ClusterName", srcCluster);
						// Change dst cluster since src is linked to the tenant group
						dstCluster = self->chooseClusterName();
					}
					wait(safeThreadFutureToFuture(tr->onError(err)));
					continue;
				}
				throw err;
			}
		}

		return Void();
	}

	ACTOR static Future<bool> _check(MetaclusterMoveWorkload* self) {
		// The metacluster consistency check runs the tenant consistency check for each cluster
		state metacluster::util::MetaclusterConsistencyCheck<IDatabase> metaclusterConsistencyCheck(
		    self->managementDb, metacluster::util::AllowPartialMetaclusterOperations::True);

		wait(metaclusterConsistencyCheck.run());

		return true;
	}

	Future<Void> setup(Database const& cx) override {
		if (clientId == 0) {
			return _setup(cx, this);
		} else {
			return Void();
		}
	}

	Future<Void> start(Database const& cx) override {
		if (clientId == 0) {
			return _start(cx, this);
		} else {
			return Void();
		}
	}

	Future<bool> check(Database const& cx) override {
		if (clientId == 0) {
			return _check(this);
		} else {
			return true;
		}
	}

	void getMetrics(std::vector<PerfMetric>& m) override {}
	Key keyForIndex(int n) { return key(n); }
	Key key(int n) { return doubleToTestKey((double)n / nodeCount, keyPrefix); }
	Value value(int n) { return doubleToTestKey(n, keyPrefix); }

	Standalone<KeyValueRef> operator()(int n) { return KeyValueRef(key(n), value((n + 1) % nodeCount)); }
};

WorkloadFactory<MetaclusterMoveWorkload> MetaclusterMoveWorkloadFactory;