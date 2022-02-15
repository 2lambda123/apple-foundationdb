/*
 * TCInfo.actor.cpp
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

#include "fdbclient/ServerKnobs.h"
#include "fdbserver/DDTeamCollection.h"
#include "fdbserver/TCInfo.h"
#include "flow/actorcompiler.h" // This must be the last #include.

class TCServerInfoImpl {
public:
	ACTOR static Future<Void> updateServerMetrics(TCServerInfo* server) {
		state StorageServerInterface ssi = server->lastKnownInterface;
		state Future<ErrorOr<GetStorageMetricsReply>> metricsRequest =
		    ssi.getStorageMetrics.tryGetReply(GetStorageMetricsRequest(), TaskPriority::DataDistributionLaunch);
		state Future<Void> resetRequest = Never();
		state Future<std::pair<StorageServerInterface, ProcessClass>> interfaceChanged(server->onInterfaceChanged);
		state Future<Void> serverRemoved(server->onRemoved);

		loop {
			choose {
				when(ErrorOr<GetStorageMetricsReply> rep = wait(metricsRequest)) {
					if (rep.present()) {
						server->serverMetrics = rep;
						if (server->updated.canBeSet()) {
							server->updated.send(Void());
						}
						break;
					}
					metricsRequest = Never();
					resetRequest = delay(SERVER_KNOBS->METRIC_DELAY, TaskPriority::DataDistributionLaunch);
				}
				when(std::pair<StorageServerInterface, ProcessClass> _ssi = wait(interfaceChanged)) {
					ssi = _ssi.first;
					interfaceChanged = server->onInterfaceChanged;
					resetRequest = Void();
				}
				when(wait(serverRemoved)) { return Void(); }
				when(wait(resetRequest)) { // To prevent a tight spin loop
					if (IFailureMonitor::failureMonitor().getState(ssi.getStorageMetrics.getEndpoint()).isFailed()) {
						resetRequest = IFailureMonitor::failureMonitor().onStateEqual(
						    ssi.getStorageMetrics.getEndpoint(), FailureStatus(false));
					} else {
						resetRequest = Never();
						metricsRequest = ssi.getStorageMetrics.tryGetReply(GetStorageMetricsRequest(),
						                                                   TaskPriority::DataDistributionLaunch);
					}
				}
			}
		}

		if (server->serverMetrics.get().lastUpdate < now() - SERVER_KNOBS->DD_SS_STUCK_TIME_LIMIT) {
			if (server->ssVersionTooFarBehind.get() == false) {
				TraceEvent("StorageServerStuck", server->collection->getDistributorId())
				    .detail("ServerId", server->id.toString())
				    .detail("LastUpdate", server->serverMetrics.get().lastUpdate);
				server->ssVersionTooFarBehind.set(true);
				server->collection->addLaggingStorageServer(server->lastKnownInterface.locality.zoneId().get());
			}
		} else if (server->serverMetrics.get().versionLag > SERVER_KNOBS->DD_SS_FAILURE_VERSIONLAG) {
			if (server->ssVersionTooFarBehind.get() == false) {
				TraceEvent(SevWarn, "SSVersionDiffLarge", server->collection->getDistributorId())
				    .detail("ServerId", server->id.toString())
				    .detail("VersionLag", server->serverMetrics.get().versionLag);
				server->ssVersionTooFarBehind.set(true);
				server->collection->addLaggingStorageServer(server->lastKnownInterface.locality.zoneId().get());
			}
		} else if (server->serverMetrics.get().versionLag < SERVER_KNOBS->DD_SS_ALLOWED_VERSIONLAG) {
			if (server->ssVersionTooFarBehind.get() == true) {
				TraceEvent("SSVersionDiffNormal", server->collection->getDistributorId())
				    .detail("ServerId", server->id.toString())
				    .detail("VersionLag", server->serverMetrics.get().versionLag);
				server->ssVersionTooFarBehind.set(false);
				server->collection->removeLaggingStorageServer(server->lastKnownInterface.locality.zoneId().get());
			}
		}
		return Void();
	}

	ACTOR static Future<Void> updateServerMetrics(Reference<TCServerInfo> server) {
		wait(updateServerMetrics(server.getPtr()));
		return Void();
	}

	ACTOR static Future<Void> serverMetricsPolling(TCServerInfo* server) {
		state double lastUpdate = now();
		loop {
			wait(server->updateServerMetrics());
			wait(delayUntil(lastUpdate + SERVER_KNOBS->STORAGE_METRICS_POLLING_DELAY +
			                    SERVER_KNOBS->STORAGE_METRICS_RANDOM_DELAY * deterministicRandom()->random01(),
			                TaskPriority::DataDistributionLaunch));
			lastUpdate = now();
		}
	}
};

class TCTeamInfoImpl {
public:
	ACTOR static Future<Void> updateStorageMetrics(TCTeamInfo* self) {
		std::vector<Future<Void>> updates;
		updates.reserve(self->servers.size());
		for (int i = 0; i < self->servers.size(); i++)
			updates.push_back(TCServerInfo::updateServerMetrics(self->servers[i]));
		wait(waitForAll(updates));
		return Void();
	}
};

TCServerInfo::TCServerInfo(StorageServerInterface ssi,
                           DDTeamCollection* collection,
                           ProcessClass processClass,
                           bool inDesiredDC,
                           Reference<LocalitySet> storageServerSet,
                           Version addedVersion)
  : id(ssi.id()), addedVersion(addedVersion), collection(collection), lastKnownInterface(ssi),
    lastKnownClass(processClass), dataInFlightToServer(0), onInterfaceChanged(interfaceChanged.getFuture()),
    onRemoved(removed.getFuture()), onTSSPairRemoved(Never()), inDesiredDC(inDesiredDC),
    storeType(KeyValueStoreType::END) {

	if (!ssi.isTss()) {
		localityEntry = ((LocalityMap<UID>*)storageServerSet.getPtr())->add(ssi.locality, &id);
	}
}

Future<Void> TCServerInfo::updateServerMetrics() {
	return TCServerInfoImpl::updateServerMetrics(this);
}

Future<Void> TCServerInfo::updateServerMetrics(Reference<TCServerInfo> server) {
	return TCServerInfoImpl::updateServerMetrics(server);
}

Future<Void> TCServerInfo::serverMetricsPolling() {
	return TCServerInfoImpl::serverMetricsPolling(this);
}

TCServerInfo::~TCServerInfo() {
	if (collection && ssVersionTooFarBehind.get() && !lastKnownInterface.isTss()) {
		collection->removeLaggingStorageServer(lastKnownInterface.locality.zoneId().get());
	}
}

Reference<TCMachineInfo> TCMachineInfo::clone() const {
	auto result = Reference<TCMachineInfo>(new TCMachineInfo);
	result->serversOnMachine = serversOnMachine;
	result->machineID = machineID;
	result->machineTeams = machineTeams;
	result->localityEntry = localityEntry;
	return result;
}

TCMachineInfo::TCMachineInfo(Reference<TCServerInfo> server, const LocalityEntry& entry) : localityEntry(entry) {
	ASSERT(serversOnMachine.empty());
	serversOnMachine.push_back(server);

	LocalityData& locality = server->lastKnownInterface.locality;
	ASSERT(locality.zoneId().present());
	machineID = locality.zoneId().get();
}

std::string TCMachineInfo::getServersIDStr() const {
	std::stringstream ss;
	if (serversOnMachine.empty())
		return "[unset]";

	for (const auto& server : serversOnMachine) {
		ss << server->getId().toString() << " ";
	}

	return std::move(ss).str();
}

TCMachineTeamInfo::TCMachineTeamInfo(std::vector<Reference<TCMachineInfo>> const& machines)
  : machines(machines), id(deterministicRandom()->randomUniqueID()) {
	machineIDs.reserve(machines.size());
	for (int i = 0; i < machines.size(); i++) {
		machineIDs.push_back(machines[i]->machineID);
	}
	sort(machineIDs.begin(), machineIDs.end());
}

std::string TCMachineTeamInfo::getMachineIDsStr() const {
	std::stringstream ss;

	if (machineIDs.empty())
		return "[unset]";

	for (const auto& id : machineIDs) {
		ss << id.contents().toString() << " ";
	}

	return std::move(ss).str();
}

TCTeamInfo::TCTeamInfo(std::vector<Reference<TCServerInfo>> const& servers)
  : servers(servers), healthy(true), wrongConfiguration(false), priority(SERVER_KNOBS->PRIORITY_TEAM_HEALTHY),
    id(deterministicRandom()->randomUniqueID()) {
	if (servers.empty()) {
		TraceEvent(SevInfo, "ConstructTCTeamFromEmptyServers").log();
	}
	serverIDs.reserve(servers.size());
	for (int i = 0; i < servers.size(); i++) {
		serverIDs.push_back(servers[i]->getId());
	}
}

std::vector<StorageServerInterface> TCTeamInfo::getLastKnownServerInterfaces() const {
	std::vector<StorageServerInterface> v;
	v.reserve(servers.size());
	for (const auto& server : servers) {
		v.push_back(server->lastKnownInterface);
	}
	return v;
}

std::string TCTeamInfo::getServerIDsStr() const {
	std::stringstream ss;

	if (serverIDs.empty())
		return "[unset]";

	for (const auto& id : serverIDs) {
		ss << id.toString() << " ";
	}

	return std::move(ss).str();
}

void TCTeamInfo::addDataInFlightToTeam(int64_t delta) {
	for (int i = 0; i < servers.size(); i++)
		servers[i]->dataInFlightToServer += delta;
}

int64_t TCTeamInfo::getDataInFlightToTeam() const {
	int64_t dataInFlight = 0.0;
	for (int i = 0; i < servers.size(); i++)
		dataInFlight += servers[i]->dataInFlightToServer;
	return dataInFlight;
}

int64_t TCTeamInfo::getLoadBytes(bool includeInFlight, double inflightPenalty) const {
	int64_t physicalBytes = getLoadAverage();
	double minAvailableSpaceRatio = getMinAvailableSpaceRatio(includeInFlight);
	int64_t inFlightBytes = includeInFlight ? getDataInFlightToTeam() / servers.size() : 0;
	double availableSpaceMultiplier =
	    SERVER_KNOBS->AVAILABLE_SPACE_RATIO_CUTOFF /
	    (std::max(std::min(SERVER_KNOBS->AVAILABLE_SPACE_RATIO_CUTOFF, minAvailableSpaceRatio), 0.000001));
	if (servers.size() > 2) {
		// make sure in triple replication the penalty is high enough that you will always avoid a team with a
		// member at 20% free space
		availableSpaceMultiplier = availableSpaceMultiplier * availableSpaceMultiplier;
	}

	if (minAvailableSpaceRatio < SERVER_KNOBS->TARGET_AVAILABLE_SPACE_RATIO) {
		TraceEvent(SevWarn, "DiskNearCapacity").suppressFor(1.0).detail("AvailableSpaceRatio", minAvailableSpaceRatio);
	}

	return (physicalBytes + (inflightPenalty * inFlightBytes)) * availableSpaceMultiplier;
}

int64_t TCTeamInfo::getMinAvailableSpace(bool includeInFlight) const {
	int64_t minAvailableSpace = std::numeric_limits<int64_t>::max();
	for (const auto& server : servers) {
		if (server->serverMetrics.present()) {
			auto& replyValue = server->serverMetrics.get();

			ASSERT(replyValue.available.bytes >= 0);
			ASSERT(replyValue.capacity.bytes >= 0);

			int64_t bytesAvailable = replyValue.available.bytes;
			if (includeInFlight) {
				bytesAvailable -= server->dataInFlightToServer;
			}

			minAvailableSpace = std::min(bytesAvailable, minAvailableSpace);
		}
	}

	return minAvailableSpace; // Could be negative
}

double TCTeamInfo::getMinAvailableSpaceRatio(bool includeInFlight) const {
	double minRatio = 1.0;
	for (const auto& server : servers) {
		if (server->serverMetrics.present()) {
			auto& replyValue = server->serverMetrics.get();

			ASSERT(replyValue.available.bytes >= 0);
			ASSERT(replyValue.capacity.bytes >= 0);

			int64_t bytesAvailable = replyValue.available.bytes;
			if (includeInFlight) {
				bytesAvailable = std::max((int64_t)0, bytesAvailable - server->dataInFlightToServer);
			}

			if (replyValue.capacity.bytes == 0)
				minRatio = 0;
			else
				minRatio = std::min(minRatio, ((double)bytesAvailable) / replyValue.capacity.bytes);
		}
	}

	return minRatio;
}

bool TCTeamInfo::hasHealthyAvailableSpace(double minRatio) const {
	return getMinAvailableSpaceRatio() >= minRatio && getMinAvailableSpace() > SERVER_KNOBS->MIN_AVAILABLE_SPACE;
}

bool TCTeamInfo::isOptimal() const {
	for (const auto& server : servers) {
		if (server->lastKnownClass.machineClassFitness(ProcessClass::Storage) > ProcessClass::UnsetFit) {
			return false;
		}
	}
	return true;
}

bool TCTeamInfo::hasServer(const UID& server) const {
	return std::find(serverIDs.begin(), serverIDs.end(), server) != serverIDs.end();
}

void TCTeamInfo::addServers(const std::vector<UID>& servers) {
	serverIDs.reserve(servers.size());
	for (int i = 0; i < servers.size(); i++) {
		serverIDs.push_back(servers[i]);
	}
}

int64_t TCTeamInfo::getLoadAverage() const {
	int64_t bytesSum = 0;
	int added = 0;
	for (int i = 0; i < servers.size(); i++)
		if (servers[i]->serverMetrics.present()) {
			added++;
			bytesSum += servers[i]->serverMetrics.get().load.bytes;
		}

	if (added < servers.size())
		bytesSum *= 2;

	return added == 0 ? 0 : bytesSum / added;
}

Future<Void> TCTeamInfo::updateStorageMetrics() {
	return TCTeamInfoImpl::updateStorageMetrics(this);
}
