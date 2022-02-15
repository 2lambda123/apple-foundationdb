/*
 * TCInfo.h
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

#include "fdbserver/DDTeamCollection.h"

class TCTeamInfo;
class TCMachineInfo;
class TCMachineTeamInfo;

class TCServerInfo : public ReferenceCounted<TCServerInfo> {
	friend class TCServerInfoImpl;
	UID id;

public:
	Version addedVersion; // Read version when this Server is added
	DDTeamCollection* collection;
	StorageServerInterface lastKnownInterface;
	ProcessClass lastKnownClass;
	std::vector<Reference<TCTeamInfo>> teams;
	Reference<TCMachineInfo> machine;
	Future<Void> tracker;
	int64_t dataInFlightToServer;
	ErrorOr<GetStorageMetricsReply> serverMetrics;
	Promise<std::pair<StorageServerInterface, ProcessClass>> interfaceChanged;
	Future<std::pair<StorageServerInterface, ProcessClass>> onInterfaceChanged;
	Promise<Void> removed;
	Future<Void> onRemoved;
	Future<Void> onTSSPairRemoved;
	Promise<Void> killTss;
	Promise<Void> wakeUpTracker;
	bool inDesiredDC;
	LocalityEntry localityEntry;
	Promise<Void> updated;
	AsyncVar<bool> wrongStoreTypeToRemove;
	AsyncVar<bool> ssVersionTooFarBehind;
	// A storage server's StoreType does not change.
	// To change storeType for an ip:port, we destroy the old one and create a new one.
	KeyValueStoreType storeType; // Storage engine type

	TCServerInfo(StorageServerInterface ssi,
	             DDTeamCollection* collection,
	             ProcessClass processClass,
	             bool inDesiredDC,
	             Reference<LocalitySet> storageServerSet,
	             Version addedVersion = 0);

	UID const& getId() const { return id; }

	bool isCorrectStoreType(KeyValueStoreType configStoreType) const {
		// A new storage server's store type may not be set immediately.
		// If a storage server does not reply its storeType, it will be tracked by failure monitor and removed.
		return (storeType == configStoreType || storeType == KeyValueStoreType::END);
	}

	Future<Void> updateServerMetrics();

	static Future<Void> updateServerMetrics(Reference<TCServerInfo> server);

	Future<Void> serverMetricsPolling();

	~TCServerInfo();
};

class TCMachineInfo : public ReferenceCounted<TCMachineInfo> {
	TCMachineInfo() = default;

public:
	std::vector<Reference<TCServerInfo>> serversOnMachine; // SOMEDAY: change from vector to set
	Standalone<StringRef> machineID;
	std::vector<Reference<TCMachineTeamInfo>> machineTeams; // SOMEDAY: split good and bad machine teams.
	LocalityEntry localityEntry;

	Reference<TCMachineInfo> clone() const;

	explicit TCMachineInfo(Reference<TCServerInfo> server, const LocalityEntry& entry);

	std::string getServersIDStr() const;
};

// TeamCollection's machine team information
class TCMachineTeamInfo : public ReferenceCounted<TCMachineTeamInfo> {
public:
	std::vector<Reference<TCMachineInfo>> machines;
	std::vector<Standalone<StringRef>> machineIDs;
	std::vector<Reference<TCTeamInfo>> serverTeams;
	UID id;

	explicit TCMachineTeamInfo(std::vector<Reference<TCMachineInfo>> const& machines);

	int size() const {
		ASSERT(machines.size() == machineIDs.size());
		return machineIDs.size();
	}

	std::string getMachineIDsStr() const;

	bool operator==(TCMachineTeamInfo& rhs) const { return this->machineIDs == rhs.machineIDs; }
};

// TeamCollection's server team info.
class TCTeamInfo final : public ReferenceCounted<TCTeamInfo>, public IDataDistributionTeam {
	friend class TCTeamInfoImpl;
	std::vector<Reference<TCServerInfo>> servers;
	std::vector<UID> serverIDs;
	bool healthy;
	bool wrongConfiguration; // True if any of the servers in the team have the wrong configuration
	int priority;
	UID id;

public:
	Reference<TCMachineTeamInfo> machineTeam;
	Future<Void> tracker;

	explicit TCTeamInfo(std::vector<Reference<TCServerInfo>> const& servers);

	std::string getTeamID() const override { return id.shortString(); }

	std::vector<StorageServerInterface> getLastKnownServerInterfaces() const override;

	int size() const override {
		ASSERT(servers.size() == serverIDs.size());
		return servers.size();
	}

	std::vector<UID> const& getServerIDs() const override { return serverIDs; }

	const std::vector<Reference<TCServerInfo>>& getServers() const { return servers; }

	std::string getServerIDsStr() const;

	void addDataInFlightToTeam(int64_t delta) override;

	int64_t getDataInFlightToTeam() const override;

	int64_t getLoadBytes(bool includeInFlight = true, double inflightPenalty = 1.0) const override;

	int64_t getMinAvailableSpace(bool includeInFlight = true) const override;

	double getMinAvailableSpaceRatio(bool includeInFlight = true) const override;

	bool hasHealthyAvailableSpace(double minRatio) const override;

	Future<Void> updateStorageMetrics() override;

	bool isOptimal() const override;

	bool isWrongConfiguration() const override { return wrongConfiguration; }
	void setWrongConfiguration(bool wrongConfiguration) override { this->wrongConfiguration = wrongConfiguration; }
	bool isHealthy() const override { return healthy; }
	void setHealthy(bool h) override { healthy = h; }
	int getPriority() const override { return priority; }
	void setPriority(int p) override { priority = p; }
	void addref() override { ReferenceCounted<TCTeamInfo>::addref(); }
	void delref() override { ReferenceCounted<TCTeamInfo>::delref(); }

	bool hasServer(const UID& server) const;

	void addServers(const std::vector<UID>& servers) override;

private:
	// Calculate an "average" of the metrics replies that we received.  Penalize teams from which we did not receive all
	// replies.
	int64_t getLoadAverage() const;
};
