/*
 * Metacluster.cpp
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

#include "fdbclient/Metacluster.h"
#include "fdbclient/MetaclusterManagement.actor.h"
#include "libb64/decode.h"
#include "libb64/encode.h"

FDB_DEFINE_BOOLEAN_PARAM(ApplyManagementClusterUpdates);
FDB_DEFINE_BOOLEAN_PARAM(RemoveMissingTenants);
FDB_DEFINE_BOOLEAN_PARAM(AssignClusterAutomatically);
FDB_DEFINE_BOOLEAN_PARAM(GroupAlreadyExists);
FDB_DEFINE_BOOLEAN_PARAM(IsRestoring);
FDB_DEFINE_BOOLEAN_PARAM(RunOnDisconnectedCluster);
FDB_DEFINE_BOOLEAN_PARAM(RunOnMismatchedCluster);
FDB_DEFINE_BOOLEAN_PARAM(RestoreDryRun);
FDB_DEFINE_BOOLEAN_PARAM(ForceJoin);
FDB_DEFINE_BOOLEAN_PARAM(ForceReuseTenantIdPrefix);
FDB_DEFINE_BOOLEAN_PARAM(ForceRemove);
FDB_DEFINE_BOOLEAN_PARAM(IgnoreCapacityLimit);

namespace MetaclusterAPI {

std::string tenantStateToString(TenantState tenantState) {
	switch (tenantState) {
	case TenantState::REGISTERING:
		return "registering";
	case TenantState::READY:
		return "ready";
	case TenantState::REMOVING:
		return "removing";
	case TenantState::UPDATING_CONFIGURATION:
		return "updating configuration";
	case TenantState::RENAMING:
		return "renaming";
	case TenantState::ERROR:
		return "error";
	default:
		UNREACHABLE();
	}
}

TenantState stringToTenantState(std::string stateStr) {
	std::transform(stateStr.begin(), stateStr.end(), stateStr.begin(), [](unsigned char c) { return std::tolower(c); });
	if (stateStr == "registering") {
		return TenantState::REGISTERING;
	} else if (stateStr == "ready") {
		return TenantState::READY;
	} else if (stateStr == "removing") {
		return TenantState::REMOVING;
	} else if (stateStr == "updating configuration") {
		return TenantState::UPDATING_CONFIGURATION;
	} else if (stateStr == "renaming") {
		return TenantState::RENAMING;
	} else if (stateStr == "error") {
		return TenantState::ERROR;
	}

	throw invalid_option();
}
} // namespace MetaclusterAPI

std::string clusterTypeToString(const ClusterType& clusterType) {
	switch (clusterType) {
	case ClusterType::STANDALONE:
		return "standalone";
	case ClusterType::METACLUSTER_MANAGEMENT:
		return "metacluster_management";
	case ClusterType::METACLUSTER_DATA:
		return "metacluster_data";
	default:
		return "unknown";
	}
}

std::string DataClusterEntry::clusterStateToString(DataClusterState clusterState) {
	switch (clusterState) {
	case DataClusterState::REGISTERING:
		return "registering";
	case DataClusterState::READY:
		return "ready";
	case DataClusterState::REMOVING:
		return "removing";
	case DataClusterState::RESTORING:
		return "restoring";
	default:
		UNREACHABLE();
	}
}

DataClusterState DataClusterEntry::stringToClusterState(std::string stateStr) {
	if (stateStr == "registering") {
		return DataClusterState::REGISTERING;
	} else if (stateStr == "ready") {
		return DataClusterState::READY;
	} else if (stateStr == "removing") {
		return DataClusterState::REMOVING;
	} else if (stateStr == "restoring") {
		return DataClusterState::RESTORING;
	}

	UNREACHABLE();
}

json_spirit::mObject DataClusterEntry::toJson() const {
	json_spirit::mObject obj;
	obj["id"] = id.toString();
	obj["capacity"] = capacity.toJson();
	obj["allocated"] = allocated.toJson();
	obj["cluster_state"] = DataClusterEntry::clusterStateToString(clusterState);
	return obj;
}

json_spirit::mObject ClusterUsage::toJson() const {
	json_spirit::mObject obj;
	obj["num_tenant_groups"] = numTenantGroups;
	return obj;
}

MetaclusterTenantMapEntry::MetaclusterTenantMapEntry() {}
MetaclusterTenantMapEntry::MetaclusterTenantMapEntry(int64_t id,
                                                     TenantName tenantName,
                                                     MetaclusterAPI::TenantState tenantState)
  : tenantName(tenantName), tenantState(tenantState) {
	setId(id);
}
MetaclusterTenantMapEntry::MetaclusterTenantMapEntry(int64_t id,
                                                     TenantName tenantName,
                                                     MetaclusterAPI::TenantState tenantState,
                                                     Optional<TenantGroupName> tenantGroup)
  : tenantName(tenantName), tenantState(tenantState), tenantGroup(tenantGroup) {
	setId(id);
}

TenantMapEntry MetaclusterTenantMapEntry::toTenantMapEntry() const {
	TenantMapEntry entry;
	entry.tenantName = tenantName;
	entry.tenantLockState = tenantLockState;
	entry.tenantLockId = tenantLockId;
	entry.tenantGroup = tenantGroup;
	entry.configurationSequenceNum = configurationSequenceNum;
	if (id >= 0) {
		entry.setId(id);
	}

	return entry;
}

MetaclusterTenantMapEntry MetaclusterTenantMapEntry::fromTenantMapEntry(TenantMapEntry const& source) {
	MetaclusterTenantMapEntry entry;
	entry.tenantName = source.tenantName;
	entry.tenantLockState = source.tenantLockState;
	entry.tenantLockId = source.tenantLockId;
	entry.tenantGroup = source.tenantGroup;
	entry.configurationSequenceNum = source.configurationSequenceNum;
	if (source.id >= 0) {
		entry.setId(source.id);
	}

	return entry;
}

void MetaclusterTenantMapEntry::setId(int64_t id) {
	ASSERT(id >= 0);
	this->id = id;
	prefix = TenantAPI::idToPrefix(id);
}

std::string MetaclusterTenantMapEntry::toJson() const {
	json_spirit::mObject tenantEntry;
	tenantEntry["id"] = id;

	tenantEntry["name"] = binaryToJson(tenantName);
	tenantEntry["prefix"] = binaryToJson(prefix);

	tenantEntry["tenant_state"] = MetaclusterAPI::tenantStateToString(tenantState);
	tenantEntry["assigned_cluster"] = binaryToJson(assignedCluster);

	if (tenantGroup.present()) {
		tenantEntry["tenant_group"] = binaryToJson(tenantGroup.get());
	}

	tenantEntry["lock_state"] = TenantAPI::tenantLockStateToString(tenantLockState);
	if (tenantLockId.present()) {
		tenantEntry["lock_id"] = tenantLockId.get().toString();
	}

	if (tenantState == MetaclusterAPI::TenantState::RENAMING) {
		ASSERT(renameDestination.present());
		tenantEntry["rename_destination"] = binaryToJson(renameDestination.get());
	} else if (tenantState == MetaclusterAPI::TenantState::ERROR) {
		tenantEntry["error"] = error;
	}

	return json_spirit::write_string(json_spirit::mValue(tenantEntry));
}

bool MetaclusterTenantMapEntry::matchesConfiguration(MetaclusterTenantMapEntry const& other) const {
	return tenantGroup == other.tenantGroup && tenantLockState == other.tenantLockState &&
	       tenantLockId == other.tenantLockId;
}

bool MetaclusterTenantMapEntry::matchesConfiguration(TenantMapEntry const& other) const {
	return tenantGroup == other.tenantGroup && tenantLockState == other.tenantLockState &&
	       tenantLockId == other.tenantLockId;
}

void MetaclusterTenantMapEntry::configure(Standalone<StringRef> parameter, Optional<Value> value) {
	if (parameter == "tenant_group"_sr) {
		tenantGroup = value;
	} else if (parameter == "assigned_cluster"_sr && value.present()) {
		assignedCluster = value.get();
	} else {
		TraceEvent(SevWarnAlways, "UnknownTenantConfigurationParameter").detail("Parameter", parameter);
		throw invalid_tenant_configuration();
	}
}

bool MetaclusterTenantMapEntry::operator==(MetaclusterTenantMapEntry const& other) const {
	return id == other.id && tenantName == other.tenantName && tenantState == other.tenantState &&
	       tenantLockState == other.tenantLockState && tenantLockId == other.tenantLockId &&
	       tenantGroup == other.tenantGroup && assignedCluster == other.assignedCluster &&
	       configurationSequenceNum == other.configurationSequenceNum && renameDestination == other.renameDestination &&
	       error == other.error;
}

bool MetaclusterTenantMapEntry::operator!=(MetaclusterTenantMapEntry const& other) const {
	return !(*this == other);
}

json_spirit::mObject MetaclusterTenantGroupEntry::toJson() const {
	json_spirit::mObject tenantGroupEntry;
	tenantGroupEntry["assigned_cluster"] = binaryToJson(assignedCluster);
	return tenantGroupEntry;
}

bool MetaclusterTenantGroupEntry::operator==(MetaclusterTenantGroupEntry const& other) const {
	return assignedCluster == other.assignedCluster;
}
bool MetaclusterTenantGroupEntry::operator!=(MetaclusterTenantGroupEntry const& other) const {
	return !(*this == other);
}

KeyBackedObjectProperty<MetaclusterRegistrationEntry, decltype(IncludeVersion())>&
MetaclusterMetadata::metaclusterRegistration() {
	static KeyBackedObjectProperty<MetaclusterRegistrationEntry, decltype(IncludeVersion())> instance(
	    "\xff/metacluster/clusterRegistration"_sr, IncludeVersion());
	return instance;
}

KeyBackedSet<UID>& MetaclusterMetadata::registrationTombstones() {
	static KeyBackedSet<UID> instance("\xff/metacluster/registrationTombstones"_sr);
	return instance;
}

KeyBackedMap<ClusterName, UID>& MetaclusterMetadata::activeRestoreIds() {
	static KeyBackedMap<ClusterName, UID> instance("\xff/metacluster/activeRestoreIds"_sr);
	return instance;
}