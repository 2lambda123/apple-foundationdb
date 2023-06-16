/*
 * MetaclusterMove.actor.h
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

#pragma once
#if defined(NO_INTELLISENSE) && !defined(METACLUSTER_METACLUSTERMOVE_ACTOR_G_H)
#define METACLUSTER_METACLUSTERMOVE_ACTOR_G_H
#include "metacluster/MetaclusterMove.actor.g.h"
#elif !defined(METACLUSTER_METACLUSTERMOVE_H)
#define METACLUSTER_METACLUSTERMOVE_H

#include "fdbclient/CommitTransaction.h"
#include "fdbclient/SystemData.h"
#include "metacluster/ConfigureCluster.h"
#include "metacluster/MetaclusterInternal.actor.h"
#include "metacluster/MetaclusterTypes.h"
#include <limits>
#include "fdbclient/Tenant.h"
#include "flow/flow.h"
#include "flow/genericactors.actor.h"

#include "fdbclient/IClientApi.h"
#include "fdbclient/TagThrottle.actor.h"
#include "flow/ThreadHelper.actor.h"

#include "fdbclient/DatabaseContext.h"
#include "fdbclient/FDBTypes.h"
#include "fdbclient/NativeAPI.actor.h"
#include "fdbclient/ReadYourWrites.h"

#include "fdbclient/TenantManagement.actor.h"
#include "fdbrpc/TenantName.h"
#include "flow/FastRef.h"
#include "flow/IRandom.h"
#include "metacluster/ListTenants.actor.h"
#include "metacluster/Metacluster.h"
#include "metacluster/MetaclusterMetadata.h"

#include "flow/actorcompiler.h" // has to be last include
namespace metacluster {
namespace internal {

ACTOR static Future<Void> updateMoveRecordState(Reference<ITransaction> tr,
                                                metadata::management::MovementState mState,
                                                TenantGroupName tenantGroup) {
	Optional<metadata::management::MovementRecord> existingMoveRec =
	    wait(metadata::management::emergency_movement::emergencyMovements().get(tr, tenantGroup));
	auto updatedMoveRec = existingMoveRec.get();
	updatedMoveRec.mState = mState;

	metadata::management::emergency_movement::emergencyMovements().set(tr, tenantGroup, updatedMoveRec);
	return Void();
}

ACTOR static Future<metadata::management::MovementRecord> initMoveParams(
    Reference<ITransaction> tr,
    TenantGroupName tenantGroup,
    std::vector<std::pair<TenantName, int64_t>> tenantsInGroup,
    ClusterName src,
    ClusterName dst) {
	wait(store(tenantsInGroup,
	           listTenantGroupTenantsTransaction(
	               tr, tenantGroup, TenantName(""_sr), TenantName("\xff"_sr), CLIENT_KNOBS->MAX_TENANTS_PER_CLUSTER)));
	Optional<metadata::management::MovementRecord> moveRecord =
	    wait(metadata::management::emergency_movement::emergencyMovements().get(tr, tenantGroup));
	if (!moveRecord.present()) {
		TraceEvent("TenantMoveRecordNotPresent").detail("TenantGroup", tenantGroup);
		throw invalid_tenant_move();
	}
	if (moveRecord->srcCluster != src || moveRecord->dstCluster != dst) {
		TraceEvent("TenantMoveRecordSrcDstMismatch")
		    .detail("TenantGroup", tenantGroup)
		    .detail("ExpectedSrc", src)
		    .detail("ExpectedDst", dst)
		    .detail("RecordSrc", moveRecord->srcCluster)
		    .detail("RecordDst", moveRecord->dstCluster);
		throw invalid_tenant_move();
	}
	return moveRecord.get();
}

ACTOR static Future<std::vector<TenantMapEntry>> getTenantEntries(
    std::vector<std::pair<TenantName, int64_t>> tenantsInGroup,
    Reference<ITransaction> tr) {
	state std::vector<Future<TenantMapEntry>> entryFutures;
	for (const auto& tenantPair : tenantsInGroup) {
		entryFutures.push_back(TenantAPI::getTenantTransaction(tr, tenantPair.first));
	}

	wait(waitForAll(entryFutures));

	state std::vector<TenantMapEntry> results;
	for (auto const& f : entryFutures) {
		results.push_back(f.get());
	}

	return results;
}

template <class DB>
struct StartTenantMovementImpl {
	MetaclusterOperationContext<DB> srcCtx;
	MetaclusterOperationContext<DB> dstCtx;

	// Initialization parameters
	TenantGroupName tenantGroup;
	metadata::management::MovementRecord moveRecord;

	// Parameters filled in during the run
	std::vector<std::pair<TenantName, int64_t>> tenantsInGroup;
	Optional<ThrottleApi::TagQuotaValue> tagQuota;
	Optional<int64_t> storageQuota;

	StartTenantMovementImpl(Reference<DB> managementDb, TenantGroupName tenantGroup, ClusterName src, ClusterName dst)
	  : srcCtx(managementDb, src), dstCtx(managementDb, dst), tenantGroup(tenantGroup) {}

	ACTOR static Future<Void> findTenantsInGroup(StartTenantMovementImpl* self,
	                                             Reference<typename DB::TransactionT> tr) {
		wait(store(self->tenantsInGroup,
		           listTenantGroupTenantsTransaction(tr,
		                                             self->tenantGroup,
		                                             TenantName(""_sr),
		                                             TenantName("\xff"_sr),
		                                             CLIENT_KNOBS->MAX_TENANTS_PER_CLUSTER)));
		return Void();
	}

	ACTOR static Future<Void> storeMoveRecord(StartTenantMovementImpl* self, Reference<typename DB::TransactionT> tr) {
		state ClusterName srcName = self->srcCtx.clusterName.get();
		state ClusterName dstName = self->dstCtx.clusterName.get();
		// Check that tenantGroup exists on src
		state bool exists = wait(
		    metadata::management::clusterTenantGroupIndex().exists(tr, Tuple::makeTuple(srcName, self->tenantGroup)));
		if (!exists) {
			TraceEvent("TenantMoveStartGroupNotOnSource")
			    .detail("TenantGroup", self->tenantGroup)
			    .detail("ClusterName", srcName);
			throw invalid_tenant_move();
		}
		Optional<metadata::management::MovementRecord> existingMoveRecord =
		    wait(metadata::management::emergency_movement::emergencyMovements().get(tr, self->tenantGroup));
		if (!existingMoveRecord.present()) {
			self->moveRecord.runId = deterministicRandom()->randomUniqueID();
			self->moveRecord.srcCluster = srcName;
			self->moveRecord.dstCluster = dstName;
			self->moveRecord.mState = metadata::management::MovementState::START_METADATA;
			self->moveRecord.version = -1;
			metadata::management::emergency_movement::emergencyMovements().set(tr, self->tenantGroup, self->moveRecord);

			// clusterCapacityIndex to accommodate for capacity calculations
			DataClusterMetadata clusterMetadata = self->dstCtx.dataClusterMetadata.get();
			DataClusterEntry updatedEntry = clusterMetadata.entry;
			updatedEntry.allocated.numTenantGroups++;
			metacluster::updateClusterMetadata(
			    tr, dstName, clusterMetadata, Optional<ClusterConnectionString>(), updatedEntry);

			// clusterTenantCount to accommodate for capacity calculations
			int numTenants = self->tenantsInGroup.size();
			metadata::management::clusterTenantCount().atomicOp(tr, dstName, numTenants, MutationRef::AddValue);
		} else {
			metadata::management::MovementRecord mi = existingMoveRecord.get();
			if (mi.srcCluster != srcName || mi.dstCluster != dstName) {
				TraceEvent("TenantMoveStartExistingSrcDstMistmatch")
				    .detail("ExistingSrc", mi.srcCluster)
				    .detail("ExistingDst", mi.dstCluster)
				    .detail("GivenSrc", srcName)
				    .detail("GivenDst", dstName);
				throw invalid_tenant_move();
			}
			self->moveRecord = mi;
		}
		return Void();
	}

	ACTOR static Future<Void> lockSourceTenants(StartTenantMovementImpl* self) {
		wait(self->srcCtx.runManagementTransaction([self = self](Reference<typename DB::TransactionT> tr) {
			return updateMoveRecordState(tr, metadata::management::MovementState::START_LOCK, self->tenantGroup);
		}));
		std::vector<Future<Void>> lockFutures;
		for (auto& tenantPair : self->tenantsInGroup) {
			lockFutures.push_back(metacluster::changeTenantLockState(self->srcCtx.managementDb,
			                                                         tenantPair.first,
			                                                         TenantAPI::TenantLockState::LOCKED,
			                                                         self->moveRecord.runId));
		}
		wait(waitForAll(lockFutures));
		return Void();
	}

	ACTOR static Future<Void> getVersionFromSource(StartTenantMovementImpl* self, Reference<ITransaction> tr) {
		state ThreadFuture<Version> resultFuture = tr->getReadVersion();
		wait(store(self->moveRecord.version, safeThreadFutureToFuture(resultFuture)));
		return Void();
	}

	static Future<Void> storeVersionToManagement(StartTenantMovementImpl* self,
	                                             Reference<typename DB::TransactionT> tr) {
		// Our own record should be updated from the source cluster already
		// Update the management metadata to reflect this
		metadata::management::emergency_movement::emergencyMovements().set(tr, self->tenantGroup, self->moveRecord);
		return Void();
	}

	ACTOR static Future<Standalone<VectorRef<KeyRef>>> getTenantSplitPointsFromSource(StartTenantMovementImpl* self,
	                                                                                  TenantName tenantName) {
		Reference<ITenant> srcTenant = self->srcCtx.dataClusterDb->openTenant(tenantName);
		Reference<ITransaction> srcTr = srcTenant->createTransaction();
		KeyRange allKeys = KeyRangeRef(""_sr, "\xff"_sr);
		// chunkSize = 100MB
		int64_t chunkSize = 100000000;
		ThreadFuture<Standalone<VectorRef<KeyRef>>> resultFuture = srcTr->getRangeSplitPoints(allKeys, chunkSize);
		Standalone<VectorRef<KeyRef>> splitPoints = wait(safeThreadFutureToFuture(resultFuture));
		return splitPoints;
	}

	static Future<Void> storeTenantSplitPoints(StartTenantMovementImpl* self,
	                                           Reference<typename DB::TransactionT> tr,
	                                           TenantName tenantName,
	                                           Standalone<VectorRef<KeyRef>> splitPoints) {
		bool first = true;
		KeyRef lastKey;
		for (auto& key : splitPoints) {
			if (first) {
				lastKey = key;
				first = false;
				continue;
			}
			metadata::management::emergency_movement::splitPointsMap().set(
			    tr, Tuple::makeTuple(self->tenantGroup, self->moveRecord.runId.toString(), tenantName, lastKey), key);
			lastKey = key;
		}
		return Void();
	}

	ACTOR static Future<Void> storeAllTenantsSplitPoints(StartTenantMovementImpl* self,
	                                                     Reference<typename DB::TransactionT> tr) {
		// container of range-based for with continuation must be a state variable
		state std::vector<std::pair<TenantName, int64_t>> tenantsInGroup = self->tenantsInGroup;
		for (auto& tenantPair : tenantsInGroup) {
			state TenantName tenantName = tenantPair.first;
			Standalone<VectorRef<KeyRef>> splitPoints = wait(getTenantSplitPointsFromSource(self, tenantName));
			wait(
			    self->srcCtx.runManagementTransaction([self = self, tenantName = tenantName, splitPoints = splitPoints](
			                                              Reference<typename DB::TransactionT> tr) {
				    return storeTenantSplitPoints(self, tr, tenantName, splitPoints);
			    }));
		}
		ASSERT(self->tenantsInGroup.size());
		TenantName firstTenant = self->tenantsInGroup[0].first;

		// Set the queue head to the first tenant and an empty key
		metadata::management::emergency_movement::movementQueue().set(
		    tr,
		    std::make_pair(self->tenantGroup, self->moveRecord.runId.toString()),
		    std::make_pair(firstTenant, Key()));
		return Void();
	}

	ACTOR static Future<Void> getSourceQuotas(StartTenantMovementImpl* self, Reference<ITransaction> tr) {
		state ThreadFuture<ValueReadResult> resultFuture = tr->get(ThrottleApi::getTagQuotaKey(self->tenantGroup));
		ValueReadResult v = wait(safeThreadFutureToFuture(resultFuture));
		self->tagQuota = v.map([](Value val) { return ThrottleApi::TagQuotaValue::unpack(Tuple::unpack(val)); });
		Optional<int64_t> optionalQuota = wait(TenantMetadata::storageQuota().get(tr, self->tenantGroup));
		self->storageQuota = optionalQuota.get();
		return Void();
	}

	static Future<Void> setDestinationQuota(StartTenantMovementImpl* self, Reference<ITransaction> tr) {
		// If source is unset, leave the destination unset too
		if (self->tagQuota.present()) {
			ThrottleApi::setTagQuota(
			    tr, self->tenantGroup, self->tagQuota.get().reservedQuota, self->tagQuota.get().totalQuota);
		}
		if (self->storageQuota.present()) {
			TenantMetadata::storageQuota().set(tr, self->tenantGroup, self->storageQuota.get());
		}
		return Void();
	}

	ACTOR static Future<Void> createLockedDestinationTenants(StartTenantMovementImpl* self,
	                                                         Reference<ITransaction> tr) {
		wait(self->srcCtx.runManagementTransaction([self = self](Reference<typename DB::TransactionT> tr) {
			return updateMoveRecordState(tr, metadata::management::MovementState::START_CREATE, self->tenantGroup);
		}));
		std::vector<Future<std::pair<Optional<TenantMapEntry>, bool>>> createFutures;
		for (auto& tenantPair : self->tenantsInGroup) {
			TenantMapEntry entry(tenantPair.second, tenantPair.first, self->tenantGroup);
			entry.tenantLockState = TenantAPI::TenantLockState::LOCKED;
			entry.tenantLockId = self->moveRecord.runId;
			createFutures.push_back(TenantAPI::createTenantTransaction(tr, entry, ClusterType::METACLUSTER_DATA));
		}
		wait(waitForAll(createFutures));
		return Void();
	}

	ACTOR static Future<Void> run(StartTenantMovementImpl* self) {
		wait(self->dstCtx.initializeContext());

		wait(self->srcCtx.runManagementTransaction(
		    [self = self](Reference<typename DB::TransactionT> tr) { return findTenantsInGroup(self, tr); }));

		wait(self->srcCtx.runManagementTransaction(
		    [self = self](Reference<typename DB::TransactionT> tr) { return storeMoveRecord(self, tr); }));

		wait(lockSourceTenants(self));

		if (self->moveRecord.version < 0) {
			wait(self->srcCtx.runDataClusterTransaction(
			    [self = self](Reference<ITransaction> tr) { return getVersionFromSource(self, tr); }));
			wait(self->srcCtx.runManagementTransaction(
			    [self = self](Reference<typename DB::TransactionT> tr) { return storeVersionToManagement(self, tr); }));
		}

		wait(self->srcCtx.runManagementTransaction(
		    [self = self](Reference<typename DB::TransactionT> tr) { return storeAllTenantsSplitPoints(self, tr); }));

		wait(self->srcCtx.runDataClusterTransaction(
		    [self = self](Reference<ITransaction> tr) { return getSourceQuotas(self, tr); }));

		wait(self->dstCtx.runDataClusterTransaction(
		    [self = self](Reference<ITransaction> tr) { return setDestinationQuota(self, tr); }));

		wait(self->dstCtx.runDataClusterTransaction(
		    [self = self](Reference<ITransaction> tr) { return createLockedDestinationTenants(self, tr); }));

		return Void();
	}

	Future<Void> run() { return run(this); }
};

template <class DB>
struct SwitchTenantMovementImpl {
	MetaclusterOperationContext<DB> srcCtx;
	MetaclusterOperationContext<DB> dstCtx;

	// Initialization parameters
	TenantGroupName tenantGroup;
	std::vector<std::string>& messages;

	// Parameters filled in during the run
	metadata::management::MovementRecord moveRecord;
	std::vector<std::pair<TenantName, int64_t>> tenantsInGroup;

	SwitchTenantMovementImpl(Reference<DB> managementDb,
	                         TenantGroupName tenantGroup,
	                         ClusterName src,
	                         ClusterName dst,
	                         std::vector<std::string>& messages)
	  : srcCtx(managementDb, src), dstCtx(managementDb, dst), tenantGroup(tenantGroup), messages(messages) {}

	ACTOR static Future<Void> checkMoveRecord(SwitchTenantMovementImpl* self, Reference<typename DB::TransactionT> tr) {
		state ClusterName srcName = self->srcCtx.clusterName.get();
		state ClusterName dstName = self->dstCtx.clusterName.get();
		// Check that tenantGroup exists on src
		// If it doesn't the switch may have already completed
		state bool exists = wait(
		    metadata::management::clusterTenantGroupIndex().exists(tr, Tuple::makeTuple(srcName, self->tenantGroup)));
		if (!exists) {
			TraceEvent("TenantMoveSwitchGroupNotOnSource")
			    .detail("TenantGroup", self->tenantGroup)
			    .detail("ClusterName", srcName);
			throw invalid_tenant_move();
		}
		wait(store(self->moveRecord, initMoveParams(tr, self->tenantGroup, self->tenantsInGroup, srcName, dstName)));
		return Void();
	}

	ACTOR static Future<Void> checkTenantData(SwitchTenantMovementImpl* self, TenantName tName) {
		state Reference<ITenant> srcTenant = self->srcCtx.dataClusterDb->openTenant(tName);
		state Reference<ITenant> dstTenant = self->dstCtx.dataClusterDb->openTenant(tName);
		state Reference<ITransaction> srcTr = srcTenant->createTransaction();
		state Reference<ITransaction> dstTr = dstTenant->createTransaction();
		state KeyRef begin = ""_sr;
		state KeyRef end = "\xff"_sr;
		// what should limit be?
		state int64_t limit = 100000;
		state ThreadFuture<RangeReadResult> srcFuture;
		state ThreadFuture<RangeReadResult> dstFuture;
		state RangeReadResult srcRange;
		state RangeReadResult dstRange;
		loop {
			srcFuture = srcTr->getRange(KeyRangeRef(begin, end), limit);
			dstFuture = dstTr->getRange(KeyRangeRef(begin, end), limit);
			wait(store(srcRange, safeThreadFutureToFuture(srcFuture)) &&
			     store(dstRange, safeThreadFutureToFuture(dstFuture)));
			if (srcRange != dstRange) {
				TraceEvent("TenantMoveSwitchDataMismatch").detail("TenantName", tName);
				throw invalid_tenant_move();
			}

			if (srcRange.more) {
				begin = srcRange.nextBeginKeySelector().getKey();
			} else {
				break;
			}
		}
		return Void();
	}

	ACTOR static Future<Void> checkAllTenantData(SwitchTenantMovementImpl* self) {
		std::vector<Future<Void>> checkFutures;
		for (const auto& tenantPair : self->tenantsInGroup) {
			checkFutures.push_back(checkTenantData(self, tenantPair.first));
		}
		wait(waitForAll(checkFutures));
		return Void();
	}

	ACTOR static Future<Void> applyHybridRanges(SwitchTenantMovementImpl* self,
	                                            Reference<typename DB::TransactionT> tr) {
		wait(self->srcCtx.runManagementTransaction([self = self](Reference<typename DB::TransactionT> tr) {
			return updateMoveRecordState(tr, metadata::management::MovementState::SWITCH_HYBRID, self->tenantGroup);
		}));
		state int rangeLimit = CLIENT_KNOBS->MAX_TENANTS_PER_CLUSTER;
		state KeyRange allKeys = KeyRangeRef(""_sr, "\xff"_sr);

		// container of range-based for with continuation must be a state variable
		state std::vector<std::pair<TenantName, int64_t>> tenantsInGroup = self->tenantsInGroup;
		for (auto& tenantPair : tenantsInGroup) {
			state TenantName tName = tenantPair.first;
			state Reference<ITenant> srcTenant = self->srcCtx.dataClusterDb->openTenant(tName);
			state Reference<ITenant> dstTenant = self->dstCtx.dataClusterDb->openTenant(tName);
			state ThreadFuture<Standalone<VectorRef<KeyRangeRef>>> resultFutureSrc =
			    srcTenant->listBlobbifiedRanges(allKeys, rangeLimit);
			state Standalone<VectorRef<KeyRangeRef>> blobRanges = wait(safeThreadFutureToFuture(resultFutureSrc));
			// Blobbifying ranges is an idempotent operation
			// If retrying, re-blobbify all ranges
			for (auto& blobRange : blobRanges) {
				state KeyRange stateBlobRange = blobRange;
				state ThreadFuture<bool> resultFuture = dstTenant->blobbifyRange(blobRange);
				bool result = wait(safeThreadFutureToFuture(resultFuture));
				if (!result) {
					TraceEvent("TenantMoveSwitchBlobbifyFailed").detail("TenantName", tName);
					throw invalid_tenant_move();
				}
				state ThreadFuture<Version> resultFuture2 = dstTenant->verifyBlobRange(stateBlobRange, latestVersion);
				Version v = wait(safeThreadFutureToFuture(resultFuture2));
				TraceEvent("TenantMoveSwitchBlobVerified").detail("TenantName", tName).detail("VerifyVersion", v);
			}
		}
		return Void();
	}

	ACTOR static Future<Void> switchMetadataToDestination(SwitchTenantMovementImpl* self,
	                                                      Reference<typename DB::TransactionT> tr) {
		wait(self->srcCtx.runManagementTransaction([self = self](Reference<typename DB::TransactionT> tr) {
			return updateMoveRecordState(tr, metadata::management::MovementState::SWITCH_METADATA, self->tenantGroup);
		}));
		state ClusterName srcName = self->srcCtx.clusterName.get();
		state ClusterName dstName = self->dstCtx.clusterName.get();

		state std::vector<std::pair<TenantName, MetaclusterTenantMapEntry>> tenantMetadataList;
		wait(store(tenantMetadataList, listTenantMetadataTransaction(tr, self->tenantsInGroup)));
		for (auto& tenantPair : tenantMetadataList) {
			state TenantName tName = tenantPair.first;
			state MetaclusterTenantMapEntry tenantEntry = tenantPair.second;
			state int64_t tId = tenantEntry.id;

			// tenantMetadata().tenantMap update assigned cluster
			if (tenantEntry.assignedCluster != srcName) {
				TraceEvent(SevError, "TenantMoveSwitchTenantEntryWrongCluster")
				    .detail("TenantName", tName)
				    .detail("ExpectedCluster", srcName)
				    .detail("EntryCluster", tenantEntry.assignedCluster);
				self->messages.push_back(fmt::format("Tenant move switch wrong assigned cluster\n"
				                                     "		expected:	{}\n"
				                                     "		actual:		{}",
				                                     srcName,
				                                     tenantEntry.assignedCluster));
				throw invalid_tenant_move();
			}
			tenantEntry.assignedCluster = dstName;
			metadata::management::tenantMetadata().tenantMap.set(tr, tId, tenantEntry);

			// clusterTenantIndex erase tenant index on src, create tenant index on dst
			metadata::management::clusterTenantIndex().erase(tr, Tuple::makeTuple(srcName, tName, tId));
			metadata::management::clusterTenantIndex().insert(tr, Tuple::makeTuple(dstName, tName, tId));
		}
		// clusterTenantGroupIndex erase group index on src, create group index on dst
		metadata::management::clusterTenantGroupIndex().erase(tr, Tuple::makeTuple(srcName, self->tenantGroup));
		metadata::management::clusterTenantGroupIndex().insert(tr, Tuple::makeTuple(dstName, self->tenantGroup));

		// tenantMetadata().tenantGroupMap update assigned cluster
		state Optional<MetaclusterTenantGroupEntry> groupEntry;
		wait(store(groupEntry, metadata::management::tenantMetadata().tenantGroupMap.get(tr, self->tenantGroup)));
		if (!groupEntry.present()) {
			TraceEvent(SevError, "TenantMoveSwitchGroupEntryMissing").detail("TenantGroup", self->tenantGroup);
			throw invalid_tenant_move();
		}
		if (groupEntry.get().assignedCluster != srcName) {
			TraceEvent(SevError, "TenantMoveSwitchGroupEntryIncorrectCluster")
			    .detail("TenantGroup", self->tenantGroup)
			    .detail("ExpectedCluster", srcName)
			    .detail("GroupEntryAssignedCluster", groupEntry.get().assignedCluster);
			throw invalid_tenant_move();
		}
		groupEntry.get().assignedCluster = dstName;
		metadata::management::tenantMetadata().tenantGroupMap.set(tr, self->tenantGroup, groupEntry.get());

		return Void();
	}

	ACTOR static Future<Void> run(SwitchTenantMovementImpl* self) {
		wait(self->dstCtx.initializeContext());
		wait(self->srcCtx.runManagementTransaction(
		    [self = self](Reference<typename DB::TransactionT> tr) { return checkMoveRecord(self, tr); }));

		wait(checkAllTenantData(self));

		wait(self->srcCtx.runManagementTransaction(
		    [self = self](Reference<typename DB::TransactionT> tr) { return applyHybridRanges(self, tr); }));

		wait(self->srcCtx.runManagementTransaction(
		    [self = self](Reference<typename DB::TransactionT> tr) { return switchMetadataToDestination(self, tr); }));

		return Void();
	}

	Future<Void> run() { return run(this); }
};

template <class DB>
struct FinishTenantMovementImpl {
	MetaclusterOperationContext<DB> srcCtx;
	MetaclusterOperationContext<DB> dstCtx;

	// Initialization parameters
	TenantGroupName tenantGroup;
	metadata::management::MovementRecord moveRecord;

	// Parameters filled in during the run
	std::vector<std::pair<TenantName, int64_t>> tenantsInGroup;

	FinishTenantMovementImpl(Reference<DB> managementDb, TenantGroupName tenantGroup, ClusterName src, ClusterName dst)
	  : srcCtx(managementDb, src), dstCtx(managementDb, dst), tenantGroup(tenantGroup) {}

	ACTOR static Future<Void> checkMoveRecord(FinishTenantMovementImpl* self, Reference<typename DB::TransactionT> tr) {
		state ClusterName srcName = self->srcCtx.clusterName.get();
		state ClusterName dstName = self->dstCtx.clusterName.get();
		// Check that tenantGroup exists on dst
		state bool exists = wait(
		    metadata::management::clusterTenantGroupIndex().exists(tr, Tuple::makeTuple(dstName, self->tenantGroup)));
		if (!exists) {
			TraceEvent("TenantMoveFinishGroupNotOnDestination")
			    .detail("TenantGroup", self->tenantGroup)
			    .detail("ClusterName", dstName);
			throw invalid_tenant_move();
		}
		wait(store(self->moveRecord, initMoveParams(tr, self->tenantGroup, self->tenantsInGroup, srcName, dstName)));
		return Void();
	}

	ACTOR static Future<Void> checkValidUnlock(FinishTenantMovementImpl* self,
	                                           Reference<typename DB::TransactionT> tr,
	                                           std::vector<TenantMapEntry> srcEntries,
	                                           std::vector<TenantMapEntry> dstEntries) {
		state size_t iterLen = self->tenantsInGroup.size();
		ASSERT_EQ(iterLen, srcEntries.size());
		ASSERT_EQ(iterLen, dstEntries.size());
		state int index = 0;
		for (; index < iterLen; index++) {
			state TenantName tName = self->tenantsInGroup[index].first;
			state int64_t tId = self->tenantsInGroup[index].second;
			state TenantMapEntry srcEntry = srcEntries[index];
			state TenantMapEntry dstEntry = dstEntries[index];

			// Assert the tenant we are unlocking is on the right cluster
			Tuple indexTuple = Tuple::makeTuple(self->dstCtx.clusterName.get(), tName, tId);
			bool result = wait(metadata::management::clusterTenantIndex().exists(tr, indexTuple));
			if (!result) {
				TraceEvent(SevError, "TenantMoveFinishUnlockTenantClusterMismatch")
				    .detail("TenantName", tName)
				    .detail("TenantID", tId)
				    .detail("ExpectedCluster", self->dstCtx.clusterName.get());
				throw invalid_tenant_move();
			}
			// Assert src tenant has the correct tenant group
			if (!srcEntry.tenantGroup.present()) {
				TraceEvent(SevError, "TenantMoveFinishUnlockTenantGroupMissing")
				    .detail("TenantName", tName)
				    .detail("TenantID", tId);
				throw invalid_tenant_move();
			}
			if (srcEntry.tenantGroup.get() != self->tenantGroup ||
			    srcEntry.tenantGroup.get() != dstEntry.tenantGroup.get()) {
				TraceEvent(SevError, "TenantMoveFinishUnlockTenantGroupMismatch")
				    .detail("TenantName", tName)
				    .detail("TenantID", tId)
				    .detail("ExpectedGroup", self->tenantGroup)
				    .detail("SourceEntryTenantGroup", srcEntry.tenantGroup.get())
				    .detail("DestinationEntryTenantGroup", dstEntry.tenantGroup.get());
				throw invalid_tenant_move();
			}
			// Assert src tenant is locked
			if (srcEntry.tenantLockState != TenantAPI::TenantLockState::LOCKED) {
				TraceEvent(SevError, "TenantMoveFinishUnlockMatchingTenantNotLocked")
				    .detail("TenantName", tName)
				    .detail("TenantID", tId);
				throw invalid_tenant_move();
			}
		}
		return Void();
	}

	ACTOR static Future<Void> unlockDestinationTenants(FinishTenantMovementImpl* self) {
		wait(self->srcCtx.runManagementTransaction([self = self](Reference<typename DB::TransactionT> tr) {
			return updateMoveRecordState(tr, metadata::management::MovementState::FINISH_UNLOCK, self->tenantGroup);
		}));
		std::vector<Future<Void>> unlockFutures;
		for (auto& tenantPair : self->tenantsInGroup) {
			unlockFutures.push_back(metacluster::changeTenantLockState(self->srcCtx.managementDb,
			                                                           tenantPair.first,
			                                                           TenantAPI::TenantLockState::UNLOCKED,
			                                                           self->moveRecord.runId));
		}
		wait(waitForAll(unlockFutures));
		return Void();
	}

	ACTOR static Future<Void> purgeSourceBlobRanges(FinishTenantMovementImpl* self,
	                                                Reference<typename DB::TransactionT> tr) {
		state KeyRange allKeys = KeyRangeRef(""_sr, "\xff"_sr);

		// container of range-based for with continuation must be a state variable
		state std::vector<std::pair<TenantName, int64_t>> tenantsInGroup = self->tenantsInGroup;
		for (auto& tenantPair : tenantsInGroup) {
			state TenantName tName = tenantPair.first;
			state Reference<ITenant> srcTenant = self->srcCtx.dataClusterDb->openTenant(tName);
			state ThreadFuture<Key> resultFuture = srcTenant->purgeBlobGranules(allKeys, latestVersion, false);
			Key purgeKey = wait(safeThreadFutureToFuture(resultFuture));
			state ThreadFuture<Void> resultFuture2 = srcTenant->waitPurgeGranulesComplete(purgeKey);
			wait(safeThreadFutureToFuture(resultFuture2));
		}
		return Void();
	}

	ACTOR static Future<Void> checkValidDelete(FinishTenantMovementImpl* self,
	                                           Reference<typename DB::TransactionT> tr,
	                                           std::vector<TenantMapEntry> srcEntries,
	                                           std::vector<TenantMapEntry> dstEntries) {
		state size_t iterLen = self->tenantsInGroup.size();
		ASSERT_EQ(iterLen, srcEntries.size());
		ASSERT_EQ(iterLen, dstEntries.size());
		state int index = 0;
		for (; index < iterLen; index++) {
			state TenantName tName = self->tenantsInGroup[index].first;
			state int64_t tId = self->tenantsInGroup[index].second;
			state TenantMapEntry srcEntry = srcEntries[index];
			state TenantMapEntry dstEntry = dstEntries[index];

			// Assert src tenant is locked
			if (srcEntry.tenantLockState != TenantAPI::TenantLockState::LOCKED) {
				TraceEvent(SevError, "TenantMoveFinishTenantNotLocked")
				    .detail("TenantName", tName)
				    .detail("TenantID", tId);
				throw invalid_tenant_move();
			}

			// Assert dst tenant exists in metacluster metadata
			Tuple indexTuple = Tuple::makeTuple(self->dstCtx.clusterName.get(), tName, tId);
			bool result = wait(metadata::management::clusterTenantIndex().exists(tr, indexTuple));
			if (!result) {
				TraceEvent(SevError, "TenantMoveFinishDeleteDataMismatch")
				    .detail("TenantName", tName)
				    .detail("TenantID", tId)
				    .detail("ExpectedCluster", self->dstCtx.clusterName.get());
				throw invalid_tenant_move();
			}

			// Assert matching tenant groups
			if (dstEntry.tenantGroup != srcEntry.tenantGroup) {
				TraceEvent(SevError, "TenantMoveFinishTenantGroupMismatch")
				    .detail("DestinationTenantGroup", dstEntry.tenantGroup)
				    .detail("SourceTenantGroup", srcEntry.tenantGroup);
				throw invalid_tenant_move();
			}
		}
		return Void();
	}

	ACTOR static Future<Void> checkDestinationVersion(FinishTenantMovementImpl* self, Reference<ITransaction> tr) {
		loop {
			state ThreadFuture<Version> resultFuture = tr->getReadVersion();
			state Version destVersion;
			wait(store(destVersion, safeThreadFutureToFuture(resultFuture)));
			if (destVersion > self->moveRecord.version) {
				break;
			}
			wait(delay(1.0));
		}
		return Void();
	}

	static Future<Void> deleteSourceData(FinishTenantMovementImpl* self, TenantName tName) {
		Reference<ITenant> srcTenant = self->srcCtx.dataClusterDb->openTenant(tName);
		Reference<ITransaction> srcTr = srcTenant->createTransaction();
		KeyRangeRef normalKeys(""_sr, "\xff"_sr);
		srcTr->clear(normalKeys);
		return Void();
	}

	ACTOR static Future<Void> deleteSourceTenants(FinishTenantMovementImpl* self, Reference<ITransaction> tr) {
		std::vector<Future<Void>> deleteFutures;
		for (auto& tenantPair : self->tenantsInGroup) {
			TenantName tName = tenantPair.first;
			int64_t tId = tenantPair.second;
			deleteSourceData(self, tName);
			deleteFutures.push_back(TenantAPI::deleteTenantTransaction(tr, tId, ClusterType::METACLUSTER_DATA));
		}
		wait(waitForAll(deleteFutures));
		return Void();
	}

	static Future<Void> updateCapacityMetadata(FinishTenantMovementImpl* self,
	                                           Reference<typename DB::TransactionT> tr) {
		ClusterName srcName = self->srcCtx.clusterName.get();

		// clusterCapacityIndex() reduce allocated capacity of source
		DataClusterMetadata clusterMetadata = self->srcCtx.dataClusterMetadata.get();
		DataClusterEntry updatedEntry = clusterMetadata.entry;
		updatedEntry.allocated.numTenantGroups--;
		metacluster::updateClusterMetadata(
		    tr, srcName, clusterMetadata, Optional<ClusterConnectionString>(), updatedEntry);

		// clusterTenantCount() reduce tenant count of source
		int numTenants = self->tenantsInGroup.size();
		metadata::management::clusterTenantCount().atomicOp(tr, srcName, -numTenants, MutationRef::AddValue);
		return Void();
	}

	ACTOR static Future<Void> run(FinishTenantMovementImpl* self) {
		wait(self->dstCtx.initializeContext());
		wait(self->srcCtx.runManagementTransaction(
		    [self = self](Reference<typename DB::TransactionT> tr) { return checkMoveRecord(self, tr); }));

		state std::vector<TenantMapEntry> srcEntries = wait(self->srcCtx.runDataClusterTransaction(
		    [self = self](Reference<ITransaction> tr) { return getTenantEntries(self->tenantsInGroup, tr); }));
		state std::vector<TenantMapEntry> dstEntries = wait(self->dstCtx.runDataClusterTransaction(
		    [self = self](Reference<ITransaction> tr) { return getTenantEntries(self->tenantsInGroup, tr); }));
		wait(self->dstCtx.runDataClusterTransaction(
		    [self = self](Reference<ITransaction> tr) { return checkDestinationVersion(self, tr); }));
		wait(self->srcCtx.runManagementTransaction(
		    [self = self, srcEntries = srcEntries, dstEntries = dstEntries](Reference<typename DB::TransactionT> tr) {
			    return checkValidUnlock(self, tr, srcEntries, dstEntries);
		    }));
		wait(self->srcCtx.runManagementTransaction(
		    [self = self, srcEntries = srcEntries, dstEntries = dstEntries](Reference<typename DB::TransactionT> tr) {
			    return checkValidDelete(self, tr, srcEntries, dstEntries);
		    }));

		wait(self->srcCtx.runManagementTransaction(
		    [self = self](Reference<typename DB::TransactionT> tr) { return updateCapacityMetadata(self, tr); }));

		wait(unlockDestinationTenants(self));

		wait(self->srcCtx.runManagementTransaction(
		    [self = self](Reference<typename DB::TransactionT> tr) { return purgeSourceBlobRanges(self, tr); }));

		wait(self->dstCtx.runDataClusterTransaction(
		    [self = self](Reference<ITransaction> tr) { return deleteSourceTenants(self, tr); }));

		return Void();
	}

	Future<Void> run() { return run(this); }
};

template <class DB>
struct AbortTenantMovementImpl {
	MetaclusterOperationContext<DB> srcCtx;
	MetaclusterOperationContext<DB> dstCtx;

	// Initialization parameters
	TenantGroupName tenantGroup;
	metadata::management::MovementRecord moveRecord;

	// Parameters filled in during the run
	std::vector<std::pair<TenantName, int64_t>> tenantsInGroup;

	AbortTenantMovementImpl(Reference<DB> managementDb, TenantGroupName tenantGroup, ClusterName src, ClusterName dst)
	  : srcCtx(managementDb, src), dstCtx(managementDb, dst), tenantGroup(tenantGroup) {}

	ACTOR static Future<Void> checkMoveRecord(AbortTenantMovementImpl* self, Reference<typename DB::TransactionT> tr) {
		state ClusterName srcName = self->srcCtx.clusterName.get();
		state ClusterName dstName = self->dstCtx.clusterName.get();
		wait(store(self->moveRecord, initMoveParams(tr, self->tenantGroup, self->tenantsInGroup, srcName, dstName)));
		return Void();
	}

	static Future<Void> clearMovementMetadata(AbortTenantMovementImpl* self, Reference<typename DB::TransactionT> tr) {
		UID runId = self->moveRecord.runId;
		metadata::management::emergency_movement::emergencyMovements().erase(tr, self->tenantGroup);
		metadata::management::emergency_movement::movementQueue().erase(
		    tr, std::make_pair(self->tenantGroup, runId.toString()));
		Tuple beginTuple = Tuple::makeTuple(self->tenantGroup, runId.toString(), TenantName(""_sr), KeyRef(""_sr));
		Tuple endTuple =
		    Tuple::makeTuple(self->tenantGroup, runId.toString(), TenantName("\xff"_sr), KeyRef("\xff"_sr));
		metadata::management::emergency_movement::splitPointsMap().erase(tr, beginTuple, endTuple);
		return Void();
	}

	// Don't check for matching destination entries because they will have been deleted or not yet created
	ACTOR static Future<Void> checkValidUnlock(AbortTenantMovementImpl* self,
	                                           Reference<typename DB::TransactionT> tr,
	                                           std::vector<TenantMapEntry> srcEntries) {
		state size_t iterLen = self->tenantsInGroup.size();
		ASSERT_EQ(iterLen, srcEntries.size());
		state int index = 0;
		for (; index < iterLen; index++) {
			state TenantName tName = self->tenantsInGroup[index].first;
			state int64_t tId = self->tenantsInGroup[index].second;
			state TenantMapEntry srcEntry = srcEntries[index];

			// Assert the tenant we are unlocking is on the right cluster
			Tuple indexTuple = Tuple::makeTuple(self->srcCtx.clusterName.get(), tName, tId);
			bool result = wait(metadata::management::clusterTenantIndex().exists(tr, indexTuple));
			if (!result) {
				TraceEvent(SevError, "TenantMoveAbortUnlockTenantClusterMismatch")
				    .detail("TenantName", tName)
				    .detail("TenantID", tId)
				    .detail("ExpectedCluster", self->srcCtx.clusterName.get());
				throw invalid_tenant_move();
			}
		}
		return Void();
	}

	ACTOR static Future<Void> unlockSourceTenants(AbortTenantMovementImpl* self) {
		std::vector<Future<Void>> unlockFutures;
		for (auto& tenantPair : self->tenantsInGroup) {
			unlockFutures.push_back(metacluster::changeTenantLockState(self->srcCtx.managementDb,
			                                                           tenantPair.first,
			                                                           TenantAPI::TenantLockState::UNLOCKED,
			                                                           self->moveRecord.runId));
		}
		wait(waitForAll(unlockFutures));
		return Void();
	}

	ACTOR static Future<Void> checkValidDelete(AbortTenantMovementImpl* self,
	                                           Reference<typename DB::TransactionT> tr,
	                                           std::vector<TenantMapEntry> srcEntries,
	                                           std::vector<TenantMapEntry> dstEntries) {
		state size_t iterLen = self->tenantsInGroup.size();
		ASSERT_EQ(iterLen, srcEntries.size());
		ASSERT_EQ(iterLen, dstEntries.size());
		state int index = 0;
		for (; index < iterLen; index++) {
			state TenantName tName = self->tenantsInGroup[index].first;
			state int64_t tId = self->tenantsInGroup[index].second;
			state TenantMapEntry srcEntry = srcEntries[index];
			state TenantMapEntry dstEntry = dstEntries[index];

			// Assert dst tenant is locked
			if (dstEntry.tenantLockState != TenantAPI::TenantLockState::LOCKED) {
				TraceEvent(SevError, "TenantMoveAbortTenantNotLocked")
				    .detail("TenantName", tName)
				    .detail("TenantID", tId);
				throw invalid_tenant_move();
			}

			// Assert src tenant exists in metacluster metadata
			// Abort will have switched metadata to source by this point
			Tuple indexTuple = Tuple::makeTuple(self->srcCtx.clusterName.get(), tName, tId);
			bool result = wait(metadata::management::clusterTenantIndex().exists(tr, indexTuple));
			if (!result) {
				TraceEvent(SevError, "TenantMoveFinishDeleteNoMatchingTenant")
				    .detail("TenantName", tName)
				    .detail("TenantID", tId)
				    .detail("ExpectedCluster", self->srcCtx.clusterName.get());
				throw invalid_tenant_move();
			}

			// Assert matching tenant groups
			if (dstEntry.tenantGroup != srcEntry.tenantGroup) {
				TraceEvent(SevError, "TenantMoveFinishTenantGroupMismatch")
				    .detail("DestinationTenantGroup", dstEntry.tenantGroup)
				    .detail("SourceTenantGroup", srcEntry.tenantGroup);
				throw invalid_tenant_move();
			}
		}
		return Void();
	}

	static Future<Void> deleteDestinationData(AbortTenantMovementImpl* self, TenantName tName) {
		Reference<ITenant> dstTenant = self->dstCtx.dataClusterDb->openTenant(tName);
		Reference<ITransaction> dstTr = dstTenant->createTransaction();
		KeyRangeRef normalKeys(""_sr, "\xff"_sr);
		dstTr->clear(normalKeys);
		return Void();
	}

	ACTOR static Future<Void> deleteDestinationTenants(AbortTenantMovementImpl* self, Reference<ITransaction> tr) {
		std::vector<Future<Void>> deleteFutures;
		// container of range-based for with continuation must be a state variable
		state std::vector<std::pair<TenantName, int64_t>> tenantsInGroup = self->tenantsInGroup;
		for (auto& tenantPair : tenantsInGroup) {
			TenantName tName = tenantPair.first;
			int64_t tId = tenantPair.second;
			deleteDestinationData(self, tName);
			deleteFutures.push_back(TenantAPI::deleteTenantTransaction(tr, tId, ClusterType::METACLUSTER_DATA));
		}
		wait(waitForAll(deleteFutures));
		return Void();
	}

	ACTOR static Future<Void> purgeDestinationBlobRanges(AbortTenantMovementImpl* self) {
		state KeyRange allKeys = KeyRangeRef(""_sr, "\xff"_sr);

		// container of range-based for with continuation must be a state variable
		state std::vector<std::pair<TenantName, int64_t>> tenantsInGroup = self->tenantsInGroup;
		for (auto& tenantPair : tenantsInGroup) {
			state TenantName tName = tenantPair.first;
			state Reference<ITenant> dstTenant = self->dstCtx.dataClusterDb->openTenant(tName);
			state ThreadFuture<Key> resultFuture = dstTenant->purgeBlobGranules(allKeys, latestVersion, false);
			state Key purgeKey = wait(safeThreadFutureToFuture(resultFuture));
			state ThreadFuture<Void> resultFuture2 = dstTenant->waitPurgeGranulesComplete(purgeKey);
			wait(safeThreadFutureToFuture(resultFuture2));
		}
		return Void();
	}

	ACTOR static Future<Void> switchMetadataToSource(AbortTenantMovementImpl* self,
	                                                 Reference<typename DB::TransactionT> tr) {
		state ClusterName srcName = self->srcCtx.clusterName.get();
		state ClusterName dstName = self->dstCtx.clusterName.get();
		// clusterCapacityIndex() increase allocated capacity of source
		DataClusterMetadata srcClusterMetadata = self->srcCtx.dataClusterMetadata.get();
		DataClusterEntry srcUpdatedEntry = srcClusterMetadata.entry;
		srcUpdatedEntry.allocated.numTenantGroups++;

		metacluster::updateClusterMetadata(
		    tr, srcName, srcClusterMetadata, Optional<ClusterConnectionString>(), srcUpdatedEntry);

		// clusterCapacityIndex() decrease allocated capacity of destination
		DataClusterMetadata dstClusterMetadata = self->dstCtx.dataClusterMetadata.get();
		DataClusterEntry dstUpdatedEntry = dstClusterMetadata.entry;
		dstUpdatedEntry.allocated.numTenantGroups--;
		metacluster::updateClusterMetadata(
		    tr, dstName, dstClusterMetadata, Optional<ClusterConnectionString>(), dstUpdatedEntry);

		// clusterTenantCount() increase tenant count of source, decrease tenant count of destination
		int numTenants = self->tenantsInGroup.size();
		metadata::management::clusterTenantCount().atomicOp(tr, srcName, numTenants, MutationRef::AddValue);
		metadata::management::clusterTenantCount().atomicOp(tr, dstName, -numTenants, MutationRef::AddValue);

		state std::vector<std::pair<TenantName, MetaclusterTenantMapEntry>> tenantMetadataList;
		wait(store(tenantMetadataList, listTenantMetadataTransaction(tr, self->tenantsInGroup)));
		for (auto& tenantPair : tenantMetadataList) {
			state TenantName tName = tenantPair.first;
			state MetaclusterTenantMapEntry tenantEntry = tenantPair.second;
			state int64_t tId = tenantEntry.id;

			// tenantMetadata().tenantMap update assigned cluster
			if (tenantEntry.assignedCluster != dstName) {
				TraceEvent(SevError, "TenantMoveAbortSwitchTenantEntryWrongCluster")
				    .detail("TenantName", tName)
				    .detail("TenantId", tId)
				    .detail("ExpectedCluster", srcName)
				    .detail("EntryCluster", tenantEntry.assignedCluster);
				throw invalid_tenant_move();
			}
			tenantEntry.assignedCluster = srcName;
			metadata::management::tenantMetadata().tenantMap.set(tr, tId, tenantEntry);

			// clusterTenantIndex erase tenant index on dst, create tenant index on src
			metadata::management::clusterTenantIndex().erase(tr, Tuple::makeTuple(dstName, tName, tId));
			metadata::management::clusterTenantIndex().insert(tr, Tuple::makeTuple(srcName, tName, tId));
		}
		// clusterTenantGroupIndex erase group index on dst, create group index on src
		metadata::management::clusterTenantGroupIndex().erase(tr, Tuple::makeTuple(dstName, self->tenantGroup));
		metadata::management::clusterTenantGroupIndex().insert(tr, Tuple::makeTuple(srcName, self->tenantGroup));

		// tenantMetadata().tenantGroupMap update assigned cluster
		state Optional<MetaclusterTenantGroupEntry> groupEntry;
		wait(store(groupEntry, metadata::management::tenantMetadata().tenantGroupMap.get(tr, self->tenantGroup)));
		if (!groupEntry.present()) {
			TraceEvent(SevError, "TenantMoveAbortSwitchGroupEntryMissing").detail("TenantGroup", self->tenantGroup);
			throw invalid_tenant_move();
		}
		if (groupEntry.get().assignedCluster != dstName) {
			TraceEvent(SevError, "TenantMoveAbortSwitchGroupEntryIncorrectCluster")
			    .detail("TenantGroup", self->tenantGroup)
			    .detail("ExpectedCluster", dstName)
			    .detail("GroupEntryAssignedCluster", groupEntry.get().assignedCluster);
			throw invalid_tenant_move();
		}
		groupEntry.get().assignedCluster = srcName;
		metadata::management::tenantMetadata().tenantGroupMap.set(tr, self->tenantGroup, groupEntry.get());

		return Void();
	}

	ACTOR static Future<Void> abortStartMetadata(AbortTenantMovementImpl* self) {
		wait(self->srcCtx.runManagementTransaction(
		    [self = self](Reference<typename DB::TransactionT> tr) { return clearMovementMetadata(self, tr); }));
		return Void();
	}

	ACTOR static Future<Void> abortStartLock(AbortTenantMovementImpl* self) {
		state std::vector<TenantMapEntry> srcEntries = wait(self->srcCtx.runDataClusterTransaction(
		    [self = self](Reference<ITransaction> tr) { return getTenantEntries(self->tenantsInGroup, tr); }));
		wait(self->srcCtx.runManagementTransaction(
		    [self = self, srcEntries = srcEntries](Reference<typename DB::TransactionT> tr) {
			    return checkValidUnlock(self, tr, srcEntries);
		    }));
		wait(unlockSourceTenants(self));

		// Update state and unwind with other steps
		wait(self->srcCtx.runManagementTransaction([self = self](Reference<typename DB::TransactionT> tr) {
			return updateMoveRecordState(tr, metadata::management::MovementState::START_METADATA, self->tenantGroup);
		}));
		wait(abortStartMetadata(self));
		return Void();
	}

	ACTOR static Future<Void> abortStartCreate(AbortTenantMovementImpl* self) {
		// If no tenant entries exist on dst, they are already deleted or were never created
		state std::vector<TenantMapEntry> dstEntries;
		state bool runDelete = true;
		try {
			wait(store(dstEntries, self->dstCtx.runDataClusterTransaction([self = self](Reference<ITransaction> tr) {
				return getTenantEntries(self->tenantsInGroup, tr);
			})));
		} catch (Error& e) {
			if (e.code() != error_code_tenant_not_found) {
				throw;
			}
			runDelete = false;
		}
		if (runDelete) {
			state std::vector<TenantMapEntry> srcEntries = wait(self->srcCtx.runDataClusterTransaction(
			    [self = self](Reference<ITransaction> tr) { return getTenantEntries(self->tenantsInGroup, tr); }));
			wait(self->srcCtx.runManagementTransaction([self = self, srcEntries = srcEntries, dstEntries = dstEntries](
			                                               Reference<typename DB::TransactionT> tr) {
				return checkValidDelete(self, tr, srcEntries, dstEntries);
			}));
			wait(self->dstCtx.runDataClusterTransaction(
			    [self = self](Reference<ITransaction> tr) { return deleteDestinationTenants(self, tr); }));
		}

		// Update state and unwind with other steps
		wait(self->srcCtx.runManagementTransaction([self = self](Reference<typename DB::TransactionT> tr) {
			return updateMoveRecordState(tr, metadata::management::MovementState::START_LOCK, self->tenantGroup);
		}));
		wait(abortStartLock(self));
		return Void();
	}

	ACTOR static Future<Void> abortSwitchHybrid(AbortTenantMovementImpl* self) {
		// Okay to run even if step is uncompleted or partially completed
		wait(purgeDestinationBlobRanges(self));

		// Update state and unwind with other steps
		wait(self->srcCtx.runManagementTransaction([self = self](Reference<typename DB::TransactionT> tr) {
			return updateMoveRecordState(tr, metadata::management::MovementState::START_CREATE, self->tenantGroup);
		}));
		wait(abortStartCreate(self));
		return Void();
	}

	ACTOR static Future<Void> abortSwitchMetadata(AbortTenantMovementImpl* self) {
		// Check for full completion and only reverse if fully completed
		Optional<MetaclusterTenantGroupEntry> optionalGroupEntry =
		    wait(tryGetTenantGroup(self->dstCtx.managementDb, self->tenantGroup));
		ASSERT(optionalGroupEntry.present());
		if (optionalGroupEntry.get().assignedCluster == self->dstCtx.clusterName.get()) {
			wait(self->srcCtx.runManagementTransaction(
			    [self = self](Reference<typename DB::TransactionT> tr) { return switchMetadataToSource(self, tr); }));
		}

		// Update state and unwind with other steps
		wait(self->srcCtx.runManagementTransaction([self = self](Reference<typename DB::TransactionT> tr) {
			return updateMoveRecordState(tr, metadata::management::MovementState::SWITCH_HYBRID, self->tenantGroup);
		}));
		wait(abortSwitchHybrid(self));
		return Void();
	}

	ACTOR static Future<Void> run(AbortTenantMovementImpl* self) {
		wait(self->dstCtx.initializeContext());
		wait(self->srcCtx.runManagementTransaction(
		    [self = self](Reference<typename DB::TransactionT> tr) { return checkMoveRecord(self, tr); }));

		// Determine how far in the move process we've progressed and begin unwinding
		Future<Void> abortFuture;
		switch (self->moveRecord.mState) {
		case metadata::management::MovementState::START_METADATA:
			abortFuture = abortStartMetadata(self);
			break;
		case metadata::management::MovementState::START_LOCK:
			abortFuture = abortStartLock(self);
			break;
		case metadata::management::MovementState::START_CREATE:
			abortFuture = abortStartCreate(self);
			break;
		case metadata::management::MovementState::SWITCH_HYBRID:
			abortFuture = abortSwitchHybrid(self);
			break;
		case metadata::management::MovementState::SWITCH_METADATA:
			abortFuture = abortSwitchMetadata(self);
			break;
		case metadata::management::MovementState::FINISH_UNLOCK:
			TraceEvent("TenantMoveAbortNotAllowedAfterDestUnlocked");
			throw invalid_tenant_move();
		}
		wait(abortFuture);
		return Void();
	}

	Future<Void> run() { return run(this); }
};

} // namespace internal

ACTOR template <class DB>
Future<Void> startTenantMovement(Reference<DB> db, TenantGroupName tenantGroup, ClusterName src, ClusterName dst) {
	state internal::StartTenantMovementImpl<DB> impl(db, tenantGroup, src, dst);
	if (src == dst) {
		TraceEvent("TenantMoveStartSameSrcDst").detail("TenantGroup", tenantGroup).detail("ClusterName", src);
		throw invalid_tenant_move();
	}
	wait(impl.run());
	return Void();
}

ACTOR template <class DB>
Future<Void> switchTenantMovement(Reference<DB> db,
                                  TenantGroupName tenantGroup,
                                  ClusterName src,
                                  ClusterName dst,
                                  std::vector<std::string>* messages) {
	state internal::SwitchTenantMovementImpl<DB> impl(db, tenantGroup, src, dst, *messages);
	if (src == dst) {
		TraceEvent("TenantMoveSwitchSameSrcDst").detail("TenantGroup", tenantGroup).detail("ClusterName", src);
		throw invalid_tenant_move();
	}
	wait(impl.run());
	return Void();
}

ACTOR template <class DB>
Future<Void> finishTenantMovement(Reference<DB> db, TenantGroupName tenantGroup, ClusterName src, ClusterName dst) {
	state internal::FinishTenantMovementImpl<DB> impl(db, tenantGroup, src, dst);
	if (src == dst) {
		TraceEvent("TenantMoveFinishSameSrcDst").detail("TenantGroup", tenantGroup).detail("ClusterName", src);
		throw invalid_tenant_move();
	}
	wait(impl.run());
	return Void();
}

ACTOR template <class DB>
Future<Void> abortTenantMovement(Reference<DB> db, TenantGroupName tenantGroup, ClusterName src, ClusterName dst) {
	state internal::AbortTenantMovementImpl<DB> impl(db, tenantGroup, src, dst);
	if (src == dst) {
		TraceEvent("TenantMoveAbortSameSrcDst").detail("TenantGroup", tenantGroup).detail("ClusterName", src);
		throw invalid_tenant_move();
	}
	wait(impl.run());
	return Void();
}

} // namespace metacluster

#include "flow/unactorcompiler.h"
#endif