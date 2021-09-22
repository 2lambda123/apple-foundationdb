/*
 * TestTLogServer.actor.cpp
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

#include <algorithm>
#include <memory>
#include <random>
#include <vector>

#include "fdbserver/IKeyValueStore.h"
#include "fdbserver/ptxn/TLogInterface.h"
#include "fdbserver/ptxn/MessageSerializer.h"
#include "fdbserver/ptxn/test/Driver.h"
#include "fdbserver/ptxn/test/FakeLogSystem.h"
#include "fdbserver/ptxn/test/FakePeekCursor.h"
#include "fdbserver/ptxn/test/Utils.h"
#include "flow/Arena.h"

#include "flow/actorcompiler.h" // has to be the last file included

namespace {

ACTOR Future<Void> startTLogServers(std::vector<Future<Void>>* actors,
                                    std::shared_ptr<ptxn::test::TestDriverContext> pContext,
                                    std::string folder) {
	state std::vector<ptxn::InitializePtxnTLogRequest> tLogInitializations;
	state int i = 0;
	for (; i < pContext->numTLogs; i++) {
		PromiseStream<ptxn::InitializePtxnTLogRequest> initializeTLog;
		Promise<Void> recovered;
		tLogInitializations.emplace_back();
		tLogInitializations.back().isPrimary = true;
		tLogInitializations.back().tlogGroups = pContext->tLogGroups;
		UID tlogId = ptxn::test::randomUID();
		UID workerId = ptxn::test::randomUID();
		actors->push_back(ptxn::tLog(std::vector<std::pair<IKeyValueStore*, IDiskQueue*>>(),
		                             makeReference<AsyncVar<ServerDBInfo>>(),
		                             LocalityData(),
		                             initializeTLog,
		                             tlogId,
		                             workerId,
		                             false,
		                             Promise<Void>(),
		                             Promise<Void>(),
		                             folder,
		                             makeReference<AsyncVar<bool>>(false),
		                             makeReference<AsyncVar<UID>>(tlogId)));
		initializeTLog.send(tLogInitializations.back());
		std::cout << "Recruit tlog " << i << " : " << tlogId.shortString() << ", workerID: " << workerId.shortString()
		          << "\n";
	}

	// replace fake TLogInterface with recruited interface
	std::vector<Future<ptxn::TLogInterface_PassivelyPull>> interfaceFutures(pContext->numTLogs);
	for (i = 0; i < pContext->numTLogs; i++) {
		interfaceFutures[i] = tLogInitializations[i].reply.getFuture();
	}
	std::vector<ptxn::TLogInterface_PassivelyPull> interfaces = wait(getAll(interfaceFutures));
	for (i = 0; i < pContext->numTLogs; i++) {
		*(pContext->tLogInterfaces[i]) = interfaces[i];
	}
	// Update the TLogGroupID to interface mapping
	for (auto& [tLogGroupID, tLogGroupLeader] : pContext->tLogGroupLeaders) {
		tLogGroupLeader = ptxn::test::randomlyPick(pContext->tLogInterfaces);
	}
	return Void();
}

void generateMutations(const Version& version,
                       const int numMutations,
                       const std::vector<ptxn::StorageTeamID>& storageTeamIDs,
                       ptxn::test::CommitRecord& commitRecord) {
	Arena arena;
	VectorRef<MutationRef> mutationRefs;
	ptxn::test::generateMutationRefs(numMutations, arena, mutationRefs);
	ptxn::test::distributeMutationRefs(mutationRefs, version, storageTeamIDs, commitRecord);
	commitRecord.messageArena.dependsOn(arena);
}

Standalone<StringRef> serializeMutations(const Version& version,
                                         const ptxn::StorageTeamID storageTeamID,
                                         const ptxn::test::CommitRecord& commitRecord) {
	ptxn::ProxySubsequencedMessageSerializer serializer(version);
	for (const auto& [_, message] : commitRecord.messages.at(version).at(storageTeamID)) {
		serializer.write(std::get<MutationRef>(message), storageTeamID);
	};
	auto serialized = serializer.getSerialized(storageTeamID);
	return serialized;
}

const int COMMIT_PEEK_CHECK_MUTATIONS = 20;

// Randomly commit to a tlog, then peek data, and verify if the data is consistent.
ACTOR Future<Void> commitPeekAndCheck(std::shared_ptr<ptxn::test::TestDriverContext> pContext) {
	state ptxn::test::print::PrintTiming printTiming("tlog/commitPeekAndCheck");

	const ptxn::TLogGroup& group = pContext->tLogGroups[0];
	ASSERT(!group.storageTeams.empty());
	state ptxn::StorageTeamID storageTeamID = group.storageTeams.begin()->first;
	printTiming << "Storage Team ID: " << storageTeamID.toString() << std::endl;

	state std::shared_ptr<ptxn::TLogInterfaceBase> tli = pContext->getTLogInterface(storageTeamID);
	state Version prevVersion = 0; // starts from 0 for first epoch
	state Version beginVersion = 150;
	state Version endVersion(beginVersion + deterministicRandom()->randomInt(5, 20));
	state Optional<UID> debugID(ptxn::test::randomUID());

	generateMutations(beginVersion, COMMIT_PEEK_CHECK_MUTATIONS, { storageTeamID }, pContext->commitRecord);
	printTiming << "Generated " << pContext->commitRecord.getNumTotalMessages() << " messages" << std::endl;
	auto serialized = serializeMutations(beginVersion, storageTeamID, pContext->commitRecord);
	std::unordered_map<ptxn::StorageTeamID, StringRef> messages = { { storageTeamID, serialized } };

	// Commit
	ptxn::TLogCommitRequest commitRequest(ptxn::test::randomUID(),
	                                      pContext->storageTeamIDTLogGroupIDMapper[storageTeamID],
	                                      serialized.arena(),
	                                      messages,
	                                      prevVersion,
	                                      beginVersion,
	                                      0,
	                                      0,
	                                      debugID);
	ptxn::test::print::print(commitRequest);

	// Reply
	ptxn::TLogCommitReply commitReply = wait(tli->commit.getReply(commitRequest));
	ptxn::test::print::print(commitReply);

	// Peek
	ptxn::TLogPeekRequest peekRequest(debugID,
	                                  beginVersion,
	                                  endVersion,
	                                  false,
	                                  false,
	                                  storageTeamID,
	                                  pContext->storageTeamIDTLogGroupIDMapper[storageTeamID]);
	ptxn::test::print::print(peekRequest);

	ptxn::TLogPeekReply peekReply = wait(tli->peek.getReply(peekRequest));
	ptxn::test::print::print(peekReply);

	// Verify
	ptxn::SubsequencedMessageDeserializer deserializer(peekReply.data);
	ASSERT(storageTeamID == deserializer.getStorageTeamID());
	ASSERT_EQ(beginVersion, deserializer.getFirstVersion());
	ASSERT_EQ(beginVersion, deserializer.getLastVersion());
	int i = 0;
	for (auto iter = deserializer.begin(); iter != deserializer.end(); ++iter, ++i) {
		const ptxn::VersionSubsequenceMessage& m = *iter;
		ASSERT_EQ(beginVersion, m.version);
		ASSERT_EQ(i + 1, m.subsequence); // subsequence starts from 1
		ASSERT(pContext->commitRecord.messages[beginVersion][storageTeamID][i].second ==
		       std::get<MutationRef>(m.message));
	}
	printTiming << "Received " << i << " mutations" << std::endl;
	ASSERT_EQ(i, pContext->commitRecord.messages[beginVersion][storageTeamID].size());

	return Void();
}

ACTOR Future<Void> startStorageServers(std::vector<Future<Void>>* actors,
                                       std::shared_ptr<ptxn::test::TestDriverContext> pContext,
                                       std::string folder) {
	ptxn::test::print::PrintTiming printTiming("testTLogServer/startStorageServers");
	// For demo purpose, each storage server only has one storage team
	ASSERT_EQ(pContext->numStorageServers, pContext->numStorageTeamIDs);
	state std::vector<InitializeStorageRequest> storageInitializations;
	state uint8_t locality = 0; // data center locality

	ServerDBInfo dbInfoBuilder;
	dbInfoBuilder.recoveryState = RecoveryState::ACCEPTING_COMMITS;
	dbInfoBuilder.logSystemConfig.logSystemType = LogSystemType::tagPartitioned;
	dbInfoBuilder.logSystemConfig.tLogs.emplace_back();
	auto& tLogSet = dbInfoBuilder.logSystemConfig.tLogs.back();
	tLogSet.locality = locality;

	printTiming << "Assign TLog group leaders" << std::endl;
	for (auto tLogGroup : pContext->tLogGroupLeaders) {
		OptionalInterface<ptxn::TLogInterface_PassivelyPull> optionalInterface =
		    OptionalInterface<ptxn::TLogInterface_PassivelyPull>(
		        *std::dynamic_pointer_cast<ptxn::TLogInterface_PassivelyPull>(tLogGroup.second));
		tLogSet.tLogGroupIDs.push_back(tLogGroup.first);
		tLogSet.ptxnTLogGroups.emplace_back();
		tLogSet.ptxnTLogGroups.back().push_back(optionalInterface);
	}
	state Reference<AsyncVar<ServerDBInfo>> dbInfo = makeReference<AsyncVar<ServerDBInfo>>(dbInfoBuilder);
	state Version tssSeedVersion = 0;
	state int i = 0;
	printTiming << "Recruiting new storage servers" << std::endl;
	for (; i < pContext->numStorageServers; i++) {
		pContext->storageServers.emplace_back();
		auto& recruited = pContext->storageServers.back();
		PromiseStream<InitializeStorageRequest> initializeStorage;
		Promise<Void> recovered;
		storageInitializations.emplace_back();

		actors->push_back(storageServer(openKVStore(KeyValueStoreType::StoreType::SSD_BTREE_V2,
		                                            joinPath(folder, "storage-" + recruited.id().toString() + ".ssd-2"),
		                                            recruited.id(),
		                                            0),
		                                recruited,
		                                Tag(locality, i),
		                                tssSeedVersion,
		                                storageInitializations.back().reply,
		                                dbInfo,
		                                folder,
		                                pContext->storageTeamIDs[i]));
		initializeStorage.send(storageInitializations.back());
		printTiming << "Recruited storage server " << i << " : " << recruited.id().shortString() << "\n";
	}

	// replace fake Storage Servers with recruited interface
	printTiming << "Updating interfaces" << std::endl;
	std::vector<Future<InitializeStorageReply>> interfaceFutures(pContext->numStorageServers);
	for (i = 0; i < pContext->numStorageServers; i++) {
		interfaceFutures[i] = storageInitializations[i].reply.getFuture();
	}
	std::vector<InitializeStorageReply> interfaces = wait(getAll(interfaceFutures));
	for (i = 0; i < pContext->numStorageServers; i++) {
		pContext->storageServers[i] = interfaces[i].interf;
	}
	return Void();
}

} // anonymous namespace

TEST_CASE("/fdbserver/ptxn/test/run_tlog_server") {
	ptxn::test::TestDriverOptions options(params);
	// Commit validation in real TLog is not supported for now
	options.skipCommitValidation = true;
	state std::vector<Future<Void>> actors;
	state std::shared_ptr<ptxn::test::TestDriverContext> pContext = ptxn::test::initTestDriverContext(options);

	state std::string folder = "simdb" + deterministicRandom()->randomAlphaNumeric(10);
	platform::createDirectory(folder);
	// start a real TLog server
	wait(startTLogServers(&actors, pContext, folder));
	// TODO: start fake proxy to talk to real TLog servers.
	startFakeSequencer(actors, pContext);
	startFakeProxy(actors, pContext);
	wait(quorum(actors, 1));
	platform::eraseDirectoryRecursive(folder);
	return Void();
}

TEST_CASE("/fdbserver/ptxn/test/peek_tlog_server") {
	state ptxn::test::TestDriverOptions options(params);
	state std::vector<Future<Void>> actors;
	state std::shared_ptr<ptxn::test::TestDriverContext> pContext = ptxn::test::initTestDriverContext(options);

	for (const auto& group : pContext->tLogGroups) {
		ptxn::test::print::print(group);
	}

	state std::string folder = "simdb/" + deterministicRandom()->randomAlphaNumeric(10);
	platform::createDirectory(folder);
	// start a real TLog server
	wait(startTLogServers(&actors, pContext, folder));
	wait(commitPeekAndCheck(pContext));

	platform::eraseDirectoryRecursive(folder);
	return Void();
}

namespace {

Version& increaseVersion(Version& version) {
	version += deterministicRandom()->randomInt(5, 10);
	return version;
}

ACTOR Future<Void> commitInject(std::shared_ptr<ptxn::test::TestDriverContext> pContext,
                                ptxn::StorageTeamID storageTeamID,
                                int numCommits) {
	state ptxn::test::print::PrintTiming printTiming("tlog/commitInject");

	state std::shared_ptr<ptxn::TLogInterfaceBase> pInterface = pContext->getTLogInterface(storageTeamID);

	state Version currVersion = 0;
	state Version prevVersion = currVersion;
	increaseVersion(currVersion);

	state std::vector<ptxn::TLogCommitRequest> requests;
	for (auto i = 0; i < numCommits; ++i) {
		generateMutations(currVersion, 16, { storageTeamID }, pContext->commitRecord);
		auto serialized = serializeMutations(currVersion, storageTeamID, pContext->commitRecord);
		std::unordered_map<ptxn::StorageTeamID, StringRef> messages = { { storageTeamID, serialized } };
		requests.emplace_back(ptxn::test::randomUID(),
		                      pContext->storageTeamIDTLogGroupIDMapper[storageTeamID],
		                      serialized.arena(),
		                      messages,
		                      prevVersion,
		                      currVersion,
		                      0,
		                      0,
		                      Optional<UID>());

		prevVersion = currVersion;
		increaseVersion(currVersion);
	}
	printTiming << "Generated " << numCommits << " commit requests" << std::endl;

	{
		std::mt19937 g(deterministicRandom()->randomUInt32());
		std::shuffle(std::begin(requests), std::end(requests), g);
	}

	state std::vector<Future<ptxn::TLogCommitReply>> replies;
	state int index = 0;
	for (index = 0; index < numCommits; ++index) {
		printTiming << "Sending version " << requests[index].version << std::endl;
		replies.push_back(pInterface->commit.getReply(requests[index]));
		wait(delay(0.5));
	}
	wait(waitForAll(replies));
	printTiming << "Received all replies" << std::endl;

	return Void();
}

ACTOR Future<Void> verifyPeek(std::shared_ptr<ptxn::test::TestDriverContext> pContext,
                              ptxn::StorageTeamID storageTeamID,
                              int numCommits) {
	state ptxn::test::print::PrintTiming printTiming("tlog/verifyPeek");

	state std::shared_ptr<ptxn::TLogInterfaceBase> pInterface = pContext->getTLogInterface(storageTeamID);

	state Version version = 0;

	state int receivedVersions = 0;
	loop {
		ptxn::TLogPeekRequest request(Optional<UID>(),
		                              version,
		                              0,
		                              false,
		                              false,
		                              storageTeamID,
		                              pContext->storageTeamIDTLogGroupIDMapper[storageTeamID]);
		request.endVersion.reset();
		ptxn::TLogPeekReply reply = wait(pInterface->peek.getReply(request));

		ptxn::SubsequencedMessageDeserializer deserializer(reply.data);
		Version v = deserializer.getFirstVersion();

		if (v == invalidVersion) {
			// The TLog has not received committed data, wait and check again
			wait(delay(0.001));
		} else {
			printTiming << concatToString("Received version range [",
			                              deserializer.getFirstVersion(),
			                              ", ",
			                              deserializer.getLastVersion(),
			                              "]")
			            << std::endl;
			std::vector<MutationRef> mutationRefs;
			auto iter = deserializer.begin();
			Arena deserializeArena = iter.arena();
			for (; iter != deserializer.end(); ++iter) {
				const auto& vsm = *iter;
				if (v != vsm.version) {
					printTiming << "Checking version " << v << std::endl;
					ASSERT(pContext->commitRecord.messages.find(v) != pContext->commitRecord.messages.end());
					const auto& recordedMessages = pContext->commitRecord.messages.at(v).at(storageTeamID);
					ASSERT(mutationRefs.size() == recordedMessages.size());
					for (size_t i = 0; i < mutationRefs.size(); ++i) {
						ASSERT(mutationRefs[i] == std::get<MutationRef>(recordedMessages[i].second));
					}

					mutationRefs.clear();
					v = vsm.version;
					++receivedVersions;
				}
				mutationRefs.emplace_back(std::get<MutationRef>(vsm.message));
			}

			{
				printTiming << "Checking version " << v << std::endl;
				const auto& recordedMessages = pContext->commitRecord.messages.at(v).at(storageTeamID);
				ASSERT(mutationRefs.size() == recordedMessages.size());
				for (size_t i = 0; i < mutationRefs.size(); ++i) {
					ASSERT(mutationRefs[i] == std::get<MutationRef>(recordedMessages[i].second));
				}

				++receivedVersions;
			}

			version = deserializer.getLastVersion() + 1;
		}

		if (receivedVersions == numCommits) {
			printTiming << "Over" << std::endl;
			break;
		}
	}

	return Void();
}

} // anonymous namespace

TEST_CASE("/fdbserver/ptxn/test/commit_peek") {
	state ptxn::test::TestDriverOptions options(params);
	state std::vector<Future<Void>> actors;
	state std::shared_ptr<ptxn::test::TestDriverContext> pContext = ptxn::test::initTestDriverContext(options);

	for (const auto& group : pContext->tLogGroups) {
		ptxn::test::print::print(group);
	}

	const ptxn::TLogGroup& group = pContext->tLogGroups[0];
	state ptxn::StorageTeamID storageTeamID = group.storageTeams.begin()->first;

	state std::string folder = "simdb/" + deterministicRandom()->randomAlphaNumeric(10);
	platform::createDirectory(folder);

	wait(startTLogServers(&actors, pContext, folder));
	const int NUM_COMMITS = 10;
	std::vector<Future<Void>> communicateActors{ commitInject(pContext, storageTeamID, NUM_COMMITS),
		                                         verifyPeek(pContext, storageTeamID, NUM_COMMITS) };
	wait(waitForAll(communicateActors));

	platform::eraseDirectoryRecursive(folder);
	return Void();
}

TEST_CASE("/fdbserver/ptxn/test/run_storage_server") {
	state ptxn::test::TestDriverOptions options(params);
	state std::vector<Future<Void>> actors;
	state std::shared_ptr<ptxn::test::TestDriverContext> pContext = ptxn::test::initTestDriverContext(options);

	for (const auto& group : pContext->tLogGroups) {
		ptxn::test::print::print(group);
	}

	state std::string folder = "simfdb/" + deterministicRandom()->randomAlphaNumeric(10);
	platform::createDirectory(folder);
	// start real TLog servers
	wait(startTLogServers(&actors, pContext, folder));

	// Inject data
	wait(commitInject(pContext, pContext->storageTeamIDs[1], 10));
	wait(verifyPeek(pContext, pContext->storageTeamIDs[1], 10));

	// start real storage servers
	wait(startStorageServers(&actors, pContext, folder));

	wait(delay(60));

	platform::eraseDirectoryRecursive(folder);
	return Void();
}
