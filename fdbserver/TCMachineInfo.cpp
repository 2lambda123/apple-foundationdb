/*
 * TCMachineInfo.h
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

#include "fdbserver/TCMachineInfo.h"
#include "fdbserver/TCMachineTeamInfo.h"
#include "fdbserver/TCServerInfo.h"

TCMachineInfo::TCMachineInfo(Reference<TCServerInfo> server, const LocalityEntry& entry) : localityEntry(entry) {
	ASSERT(serversOnMachine.empty());
	serversOnMachine.insert(server);

	LocalityData& locality = server->lastKnownInterface.locality;
	ASSERT(locality.zoneId().present());
	machineID = locality.zoneId().get();
}

std::string TCMachineInfo::getServersIDStr() const {
	if (serversOnMachine.empty()) return "[unset]";

	std::string result;

	for (const auto& server : serversOnMachine) {
		result += server->getID().toString() + " ";
	}

	return result;
}

Standalone<StringRef> TCMachineInfo::getID() const {
	return machineID;
}

TCServerInfo::ServerSet const& TCMachineInfo::getServersOnMachine() const {
	return serversOnMachine;
}

Reference<TCServerInfo> TCMachineInfo::getRepresentativeServer() const {
	return *serversOnMachine.begin();
}

void TCMachineInfo::addServer(Reference<TCServerInfo> const& server) {
	serversOnMachine.insert(server);
}

void TCMachineInfo::removeServer(Reference<TCServerInfo> const& server) {
	serversOnMachine.erase(server);
}

std::vector<Reference<TCMachineTeamInfo>> const& TCMachineInfo::getMachineTeams() const {
	return machineTeams;
}

bool TCMachineInfo::removeMachineTeam(Reference<TCMachineTeamInfo> const& machineTeam) {
	auto it = std::remove(machineTeams.begin(), machineTeams.end(), machineTeam);
	if (it == machineTeams.end()) {
		return false;
	}
	machineTeams.erase(it, machineTeams.end());
	return true;
}

void TCMachineInfo::addMachineTeam(Reference<TCMachineTeamInfo> const& machineTeam) {
	machineTeams.push_back(machineTeam);
}

void TCMachineInfo::clearMachineTeams() {
	machineTeams.clear();
}
