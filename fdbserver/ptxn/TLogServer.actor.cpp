/*
 * TLogServer.actor.cpp
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

#include <tuple>

#include "fdbclient/CommitTransaction.h"
#include "fdbclient/FDBTypes.h"
#include "fdbclient/KeyRangeMap.h"
#include "fdbclient/NativeAPI.actor.h"
#include "fdbclient/Notified.h"
#include "fdbclient/RunTransaction.actor.h"
#include "fdbclient/SystemData.h"
#include "fdbrpc/FailureMonitor.h"
#include "fdbrpc/Stats.h"
#include "fdbrpc/sim_validation.h"
#include "fdbrpc/simulator.h"
#include "fdbserver/FDBExecHelper.actor.h"
#include "fdbserver/IDiskQueue.h"
#include "fdbserver/IKeyValueStore.h"
#include "fdbserver/Knobs.h"
#include "fdbserver/LogProtocolMessage.h"
#include "fdbserver/LogSystem.h"
#include "fdbserver/MutationTracking.h"
#include "fdbserver/RecoveryState.h"
#include "fdbserver/ServerDBInfo.h"
#include "fdbserver/SpanContextMessage.h"
#include "fdbserver/WaitFailure.h"
#include "fdbserver/WorkerInterface.actor.h"
#include "fdbserver/ptxn/TLogInterface.h"
#include "fdbserver/ptxn/MessageSerializer.h"
#include "fdbserver/ptxn/test/Driver.h"
#include "fdbserver/ptxn/test/Utils.h"
#include "flow/ActorCollection.h"
#include "flow/Hash3.h"
#include "flow/Histogram.h"
#include "flow/IRandom.h"
#include "flow/Trace.h"
#include "flow/UnitTest.h"
#include "flow/actorcompiler.h" // This must be the last #include.

namespace ptxn {

struct LogGenerationData;
struct TLogGroupData;
struct TLogServerData;
ACTOR Future<Void> updateStorage(Reference<TLogGroupData> self);

struct TLogQueue final : public IClosable {
public:
	TLogQueue(IDiskQueue* queue, UID dbgid) : queue(queue), dbgid(dbgid) {}

	// Each packet in the queue is
	//    uint32_t payloadSize
	//    uint8_t payload[payloadSize]  (begins with uint64_t protocolVersion via IncludeVersion)
	//    uint8_t validFlag

	// TLogQueue is a durable queue of TLogQueueEntry objects with an interface similar to IDiskQueue

	// TLogQueue pushes (but not commits) are atomic - after commit fails to return, a prefix of entire calls to push
	// are durable.  This is implemented on top of the weaker guarantee of IDiskQueue::commit (that a prefix of bytes is
	// durable) using validFlag and by padding any incomplete packet with zeros after recovery.

	// Before calling push, pop, or commit, the user must call readNext() until it throws end_of_stream(). It may not be
	// called again thereafter.
	Future<TLogQueueEntry> readNext(TLogGroupData* tLog) { return readNext(this, tLog); }

	Future<bool> initializeRecovery(IDiskQueue::location recoverAt) { return queue->initializeRecovery(recoverAt); }

	void push(TLogQueueEntry const& qe, Reference<LogGenerationData> logData);
	void forgetBefore(Version upToVersion, Reference<LogGenerationData> logData);
	void pop(IDiskQueue::location upToLocation);
	Future<Void> commit() { return queue->commit(); }

	// Implements IClosable
	Future<Void> getError() const override { return queue->getError(); }
	Future<Void> onClosed() const override { return queue->onClosed(); }
	void dispose() override {
		queue->dispose();
		delete this;
	}
	void close() override {
		queue->close();
		delete this;
	}

private:
	IDiskQueue* queue;
	UID dbgid;

	void updateVersionSizes(const TLogQueueEntry& result,
	                        TLogGroupData* logGroup,
	                        IDiskQueue::location start,
	                        IDiskQueue::location end);

	ACTOR static Future<TLogQueueEntry> readNext(TLogQueue* self, TLogGroupData* tLogGroup) {
		state TLogQueueEntry result;
		state int zeroFillSize = 0;

		loop {
			state IDiskQueue::location startloc = self->queue->getNextReadLocation();
			Standalone<StringRef> h = wait(self->queue->readNext(sizeof(uint32_t)));
			if (h.size() != sizeof(uint32_t)) {
				if (h.size()) {
					TEST(true); // Zero fill within size field
					int payloadSize = 0;
					memcpy(&payloadSize, h.begin(), h.size());
					zeroFillSize = sizeof(uint32_t) - h.size(); // zero fill the size itself
					zeroFillSize += payloadSize + 1; // and then the contents and valid flag
				}
				break;
			}

			state uint32_t payloadSize = *(uint32_t*)h.begin();
			ASSERT(payloadSize < (100 << 20));

			Standalone<StringRef> e = wait(self->queue->readNext(payloadSize + 1));
			if (e.size() != payloadSize + 1) {
				TEST(true); // Zero fill within payload
				zeroFillSize = payloadSize + 1 - e.size();
				break;
			}

			if (e[payloadSize]) {
				ASSERT(e[payloadSize] == 1);
				Arena a = e.arena();
				ArenaReader ar(a, e.substr(0, payloadSize), IncludeVersion());
				ar >> result;
				const IDiskQueue::location endloc = self->queue->getNextReadLocation();
				self->updateVersionSizes(result, tLogGroup, startloc, endloc);
				return result;
			}
		}
		if (zeroFillSize) {
			TEST(true); // Fixing a partial commit at the end of the tlog queue
			for (int i = 0; i < zeroFillSize; i++)
				self->queue->push(StringRef((const uint8_t*)"", 1));
		}
		throw end_of_stream();
	}
};

////// Persistence format (for self->persistentData)

// Immutable keys
// persistFormat has been mostly invalidated by TLogVersion, and can probably be removed when
// 4.6's TLog code is removed.
static const KeyValueRef persistFormat(LiteralStringRef("Format"), LiteralStringRef("FoundationDB/LogServer/3/0"));
static const KeyRangeRef persistFormatReadableRange(LiteralStringRef("FoundationDB/LogServer/3/0"),
                                                    LiteralStringRef("FoundationDB/LogServer/4/0"));
static const KeyRangeRef persistProtocolVersionKeys(LiteralStringRef("ProtocolVersion/"),
                                                    LiteralStringRef("ProtocolVersion0"));
static const KeyRangeRef persistTLogSpillTypeKeys(LiteralStringRef("TLogSpillType/"),
                                                  LiteralStringRef("TLogSpillType0"));
static const KeyRangeRef persistRecoveryCountKeys =
    KeyRangeRef(LiteralStringRef("DbRecoveryCount/"), LiteralStringRef("DbRecoveryCount0"));

// Updated on updatePersistentData()
// persistCurrentVersionKeys stores verion of the interface of a certain recruitment.
static const KeyRangeRef persistCurrentVersionKeys =
    KeyRangeRef(LiteralStringRef("version/"), LiteralStringRef("version0"));
static const KeyRangeRef persistKnownCommittedVersionKeys =
    KeyRangeRef(LiteralStringRef("knownCommitted/"), LiteralStringRef("knownCommitted0"));
static const KeyRef persistRecoveryLocationKey = KeyRef(LiteralStringRef("recoveryLocation"));
static const KeyRangeRef persistLocalityKeys =
    KeyRangeRef(LiteralStringRef("Locality/"), LiteralStringRef("Locality0"));
static const KeyRangeRef persistLogRouterTagsKeys =
    KeyRangeRef(LiteralStringRef("LogRouterTags/"), LiteralStringRef("LogRouterTags0"));
static const KeyRangeRef persistTxsTagsKeys = KeyRangeRef(LiteralStringRef("TxsTags/"), LiteralStringRef("TxsTags0"));
static const KeyRange persistTagMessagesKeys = prefixRange(LiteralStringRef("TagMsg/"));
static const KeyRange persistTagMessageRefsKeys = prefixRange(LiteralStringRef("TagMsgRef/"));
static const KeyRange persistTagPoppedKeys = prefixRange(LiteralStringRef("TagPop/"));
static const KeyRange persistStorageTeamKeys = prefixRange(LiteralStringRef("StorageTeam/"));
static const KeyRange persistStorageTeamPoppedKeys = prefixRange(LiteralStringRef("StorageTeamPop/"));
static const KeyRange persistStorageTeamMessagesKeys = prefixRange(LiteralStringRef("StorageTeamMsg/"));
static const KeyRange persistStorageTeamMessageRefsKeys = prefixRange(LiteralStringRef("StorageTeamMsgRef/"));

static Key persistStorageTeamMessagesKey(UID id, StorageTeamID storageTeamId, Version version) {
	BinaryWriter wr(Unversioned());
	wr.serializeBytes(persistStorageTeamMessagesKeys.begin);
	wr << id;
	wr << storageTeamId;
	wr << bigEndian64(version);
	return wr.toValue();
}

Key persistStorageTeamMessageRefsKey(UID id, StorageTeamID storageTeamId, Version version) {
	BinaryWriter wr(Unversioned());
	wr.serializeBytes(persistStorageTeamMessageRefsKeys.begin);
	wr << id;
	wr << storageTeamId;
	wr << bigEndian64(version);
	return wr.toValue();
}

static Key persistStorageTeamPoppedKey(UID id, StorageTeamID storageTeamId) {
	BinaryWriter wr(Unversioned());
	wr.serializeBytes(persistStorageTeamPoppedKeys.begin);
	wr << id;
	wr << storageTeamId;
	return wr.toValue();
}

static Value persistStorageTeamPoppedValue(Version popped) {
	return BinaryWriter::toValue(popped, Unversioned());
}

static StorageTeamID decodeStorageTeamIDPoppedKey(KeyRef key) {
	return BinaryReader::fromStringRef<StorageTeamID>(key, Unversioned());
}

static std::pair<std::vector<Tag>, Version> decodePairValue(ValueRef value) {
	return BinaryReader::fromStringRef<std::pair<std::vector<Tag>, Version>>(value, Unversioned());
}

struct SpilledData {
	SpilledData() = default;
	SpilledData(Version version, IDiskQueue::location start, uint32_t length, uint32_t mutationBytes)
	  : version(version), start(start), length(length), mutationBytes(mutationBytes) {}

	template <class Ar>
	void serialize(Ar& ar) {
		serializer(ar, version, start, length, mutationBytes);
	}

	Version version = 0;
	IDiskQueue::location start = 0;
	uint32_t length = 0;
	uint32_t mutationBytes = 0;
};

// Data for a TLog group across multiple generations
struct TLogGroupData : NonCopyable, public ReferenceCounted<TLogGroupData> {
	AsyncTrigger newLogData; // trigger for new generation data creation.
	// A process has only 1 SharedTLog, which holds data for multiple logs, so that it obeys its assigned memory limit.
	// A process has only 1 active log and multiple non-active log from old generations.
	// In the figure below, TLog [1-4] are logs from old generations.
	// Because SS may need to pull data from old generation log, we keep Tlog [1-4].
	//
	//  We always pop the disk queue from the oldest TLog, spill from the oldest TLog that still has
	//  data in memory, and commits to the disk queue come from the most recent TLog.
	//
	//                    tlog group
	//  +--------+--------+--------+--------+--------+
	//  | xxxxxx |  xxxx  | xxxxxx |  xxx   |  xx    |
	//  +--------+--------+--------+--------+--------+
	//    ^popOrder          ^spillOrder         ^committing
	//
	// x means a commit in the history which corresponds to location in log queue.
	// ^ points to a log queue location
	// ^popOrder is the location where SS reads the to-be-read data from tlog.
	// ^committing is the location where the active TLog accepts the pushed data.
	Deque<UID> popOrder;
	Deque<UID> spillOrder;
	std::map<UID, Reference<LogGenerationData>> id_data;

	UID dbgid;
	UID workerID;
	UID tlogGroupID;

	IKeyValueStore* persistentData; // Durable data on disk that were spilled.
	IDiskQueue* rawPersistentQueue; // The physical queue the persistentQueue below stores its data. Ideally, log
	// interface should work without directly accessing rawPersistentQueue
	TLogQueue* persistentQueue; // Logical queue the log operates on and persist its data.

	int64_t diskQueueCommitBytes = 0;
	// becomes true when diskQueueCommitBytes is greater than MAX_QUEUE_COMMIT_BYTES
	AsyncVar<bool> largeDiskQueueCommitBytes{ false };

	Reference<AsyncVar<ServerDBInfo>> dbInfo;
	Database cx;

	NotifiedVersion queueCommitEnd{ 0 };
	Version queueCommitBegin = 0;

	int64_t instanceID;
	int64_t bytesInput = 0;
	int64_t bytesDurable = 0;
	// The number of bytes of mutations this TLog should hold in memory before spilling.
	int64_t targetVolatileBytes = SERVER_KNOBS->TLOG_SPILL_THRESHOLD;
	int64_t overheadBytesInput = 0;
	int64_t overheadBytesDurable = 0;

	FlowLock peekMemoryLimiter{ SERVER_KNOBS->TLOG_SPILL_REFERENCE_MAX_PEEK_MEMORY_BYTES };

	PromiseStream<Future<Void>> sharedActors;
	Promise<Void> terminated;
	FlowLock concurrentLogRouterReads{ SERVER_KNOBS->CONCURRENT_LOG_ROUTER_READS };
	FlowLock persistentDataCommitLock;

	// Beginning of fields used by snapshot based backup and restore
	bool ignorePopRequest = false; // ignore pop request from storage servers
	double ignorePopDeadline; // time until which the ignorePopRequest will be
	// honored
	std::string ignorePopUid; // callers that set ignorePopRequest will set this
	// extra state, used to validate the ownership of
	// the set and for callers that unset will
	// be able to match it up
	std::string dataFolder; // folder where data is stored
	std::map<Tag, Version> toBePopped; // map of Tag->Version for all the pops
	// that came when ignorePopRequest was set
	Reference<AsyncVar<bool>> degraded;
	// End of fields used by snapshot based backup and restore

	std::vector<TagsAndMessage> tempTagMessages;

	Reference<Histogram> commitLatencyDist;

	// shared server data
	Reference<TLogServerData> tLogServerData;

	TLogGroupData(UID dbgid,
	              UID groupID,
	              UID workerID,
	              IKeyValueStore* persistentData,
	              IDiskQueue* persistentQueue,
	              Reference<AsyncVar<ServerDBInfo>> dbInfo,
	              Reference<AsyncVar<bool>> degraded,
	              std::string folder,
	              Reference<TLogServerData> tLogServer)
	  : dbgid(dbgid), workerID(workerID), tlogGroupID(groupID), persistentData(persistentData),
	    rawPersistentQueue(persistentQueue), persistentQueue(new TLogQueue(persistentQueue, dbgid)), dbInfo(dbInfo),
	    instanceID(deterministicRandom()->randomUniqueID().first()), dataFolder(folder), degraded(degraded),
	    commitLatencyDist(Histogram::getHistogram(LiteralStringRef("tLog"),
	                                              LiteralStringRef("commit"),
	                                              Histogram::Unit::microseconds)),
	    tLogServerData(tLogServer) {
		cx = openDBOnServer(dbInfo, TaskPriority::DefaultEndpoint, LockAware::True);
	}
};

struct TLogServerData : NonCopyable, public ReferenceCounted<TLogServerData> {
	std::unordered_map<TLogGroupID, Reference<TLogGroupData>> tlogGroups;
	std::unordered_map<TLogGroupID, Reference<TLogGroupData>> oldTLogGroups;

	// There is one interface for each recruitment, during recovery previous recruitments are fetched and interfaces are
	// started
	std::map<UID, ptxn::TLogInterface_PassivelyPull> id_interf;

	// Promise streams to hold the actors of the interfaces
	std::map<UID, PromiseStream<Future<Void>>> actorsPerRecruitment;

	// Once its value is set, TLogRejoinRequest will be sent to master for each interface of each recruitment.
	std::map<UID, Promise<Void>> registerWithMasters;

	// what's this for?
	std::unordered_map<UID, std::vector<Reference<struct LogGenerationData>>> logGenerations;

	// A process has only 1 SharedTLog, which holds data for multiple log groups. Each group obeys its own assigned
	// memory limit to ensure fairness.
	// A group has at most 1 active log and multiple non-active log from old generations.
	// In the figure below:
	//   epoch [1-4] are old generations;
	//   group2 is not recruited in the current generation doesn't have an active log
	//   each group has its own commit history (indicated by the number of x)
	// Because SS may need to pull data from old generation log, we keep Tlog [1-4].
	//
	// TLogGroupData holds data for a log group's multiple generations.
	// LogGenerationData holds data for a generation for a tlog group.
	//
	//                    SharedTLog
	//                                        current
	//    epoch 1  epoch 2  epoch 3  epoch 4  epoch 5
	//  +--------+--------+--------+--------+--------+
	//  |        |        |  xxxxx | xxxxxx |   xx   |  group1
	//  +--------+--------+--------+--------+--------+
	//  |        |        |   xxx  | xxxxxx |        |  group2
	//  +--------+--------+--------+--------+--------+
	//  |   xx   | xxxxx  |  xxxx  |   xx   |   x    |  group3
	//  +--------+--------+--------+--------+--------+
	//  |        |        |        |   xxx  |  xxxxx |  group4
	//  +--------+--------+--------+--------+--------+

	AsyncTrigger newLogData;

	UID dbgid;
	UID workerID;

	// not sure if we need this.
	IKeyValueStore* persistentData; // Durable data on disk that were spilled

	int64_t diskQueueCommitBytes = 0;
	// becomes true when diskQueueCommitBytes is greater than MAX_QUEUE_COMMIT_BYTES
	AsyncVar<bool> largeDiskQueueCommitBytes{ false };

	Reference<AsyncVar<ServerDBInfo>> dbInfo;
	Database cx;

	NotifiedVersion queueCommitEnd{ 0 };
	Version queueCommitBegin = 0;

	int64_t instanceID;
	int64_t bytesInput = 0;
	int64_t bytesDurable = 0;
	int64_t targetVolatileBytes; // The number of bytes of mutations this TLog should hold in memory before spilling.
	int64_t overheadBytesInput = 0;
	int64_t overheadBytesDurable = 0;

	WorkerCache<TLogInterface_PassivelyPull> tlogCache;
	FlowLock peekMemoryLimiter;

	PromiseStream<Future<Void>> sharedActors;
	PromiseStream<Future<Void>> addActors;
	Promise<Void> terminated;
	FlowLock concurrentLogRouterReads;
	FlowLock persistentDataCommitLock;

	// Beginning of fields used by snapshot based backup and restore
	bool ignorePopRequest = false; // ignore pop request from storage servers
	double ignorePopDeadline; // time until which the ignorePopRequest will be
	// honored
	std::string ignorePopUid; // callers that set ignorePopRequest will set this
	// extra state, used to validate the ownership of
	// the set and for callers that unset will
	// be able to match it up
	std::string dataFolder; // folder where data is stored

	// that came when ignorePopRequest was set
	Reference<AsyncVar<bool>> degraded;
	// End of fields used by snapshot based backup and restore

	std::vector<TagsAndMessage> tempTagMessages;

	Reference<Histogram> commitLatencyDist;

	Future<Void> removed;

	TLogServerData(UID dbgid,
	               UID workerID,
	               Reference<AsyncVar<ServerDBInfo>> dbInfo,
	               Reference<AsyncVar<bool>> degraded,
	               std::string folder)
	  : dbgid(dbgid), workerID(workerID), dbInfo(dbInfo), instanceID(deterministicRandom()->randomUniqueID().first()),
	    targetVolatileBytes(SERVER_KNOBS->TLOG_SPILL_THRESHOLD),
	    peekMemoryLimiter(SERVER_KNOBS->TLOG_SPILL_REFERENCE_MAX_PEEK_MEMORY_BYTES),
	    concurrentLogRouterReads(SERVER_KNOBS->CONCURRENT_LOG_ROUTER_READS), ignorePopDeadline(), dataFolder(folder),
	    degraded(degraded), commitLatencyDist(Histogram::getHistogram(LiteralStringRef("tLog"),
	                                                                  LiteralStringRef("commit"),
	                                                                  Histogram::Unit::microseconds)) {
		cx = openDBOnServer(dbInfo, TaskPriority::DefaultEndpoint, LockAware::True);
	}
};

// LogGenerationData holds data for a TLogGroup in a generation.
struct LogGenerationData : NonCopyable, public ReferenceCounted<LogGenerationData> {

	// StorageTeamData holds data for a storage team and tracks each Tag in the team. Tag represents a storage server,
	// and a storage team is guaranteed to not place two copies of data on the same storage server.
	struct StorageTeamData : NonCopyable, public ReferenceCounted<StorageTeamData> {

		StorageTeamID storageTeamId;
		std::vector<Tag> tags;
		std::map<Version, std::pair<StringRef, Arena>> versionMessages;
		Version popped = 0; // see popped version tracking contract below
		IDiskQueue::location poppedLocation = 0; // The location of the earliest commit with data for this tag.
		Version persistentPopped = 0; // The popped version recorded in the btree.
		Version versionForPoppedLocation = 0; // `poppedLocation` was calculated at this popped version
		bool poppedRecently = false; // `popped` has changed since last updatePersistentData
		bool unpoppedRecovered = false;
		bool nothingPersistent =
		    false; // true means tag is *known* to have no messages in persistentData.  false means nothing.

		StorageTeamData(StorageTeamID storageTeam, std::vector<Tag> tags) : storageTeamId(storageTeam), tags(tags) {}

		StorageTeamData(StorageTeamID storageTeam, std::vector<Tag> tags, Version popped)
		  : storageTeamId(storageTeam), tags(tags), popped(popped) {}

		StorageTeamData(StorageTeamData&& r) noexcept
		  : storageTeamId(r.storageTeamId), tags(r.tags), versionMessages(std::move(r.versionMessages)),
		    popped(r.popped), poppedLocation(r.poppedLocation), persistentPopped(r.persistentPopped),
		    versionForPoppedLocation(r.versionForPoppedLocation), poppedRecently(r.poppedRecently),
		    unpoppedRecovered(r.unpoppedRecovered), nothingPersistent(r.nothingPersistent) {}
		void operator=(StorageTeamData&& r) noexcept {
			storageTeamId = r.storageTeamId;
			nothingPersistent = r.nothingPersistent;
			poppedRecently = r.poppedRecently;
			popped = r.popped;
			persistentPopped = r.persistentPopped;
			versionForPoppedLocation = r.versionForPoppedLocation;
			poppedLocation = r.poppedLocation;
			unpoppedRecovered = r.unpoppedRecovered;
			versionMessages = std::move(r.versionMessages);
		}
	};

	// For the version of each entry that was push()ed, the [start, end) location of the serialized bytes
	Map<Version, std::pair<IDiskQueue::location, IDiskQueue::location>> versionLocation;

	/*
	Popped version tracking contract needed by log system to implement ILogCursor::popped():

	    - Log server tracks for each (possible) tag a popped_version
	    Impl: TagData::popped (in memory) and persistTagPoppedKeys (in persistentData)
	    - popped_version(tag) is <= the maximum version for which log server (or a predecessor) is ever asked to pop the
	tag Impl: Only increased by tLogPop() in response to either a pop request or recovery from a predecessor
	    - popped_version(tag) is > the maximum version for which log server is unable to peek messages due to previous
	pops (on this server or a predecessor) Impl: Increased by tLogPop() atomically with erasing messages from memory;
	persisted by updatePersistentData() atomically with erasing messages from store; messages are not erased from queue
	where popped_version is not persisted
	    - LockTLogReply returns all tags which either have messages, or which have nonzero popped_versions
	    Impl: tag_data is present for all such tags
	    - peek(tag, v) returns the popped_version for tag if that is greater than v
	    Impl: Check tag_data->popped (after all waits)
	*/

	// If persistentDataVersion != persistentDurableDataVersion,
	// then spilling is happening from persistentDurableDataVersion to persistentDataVersion.
	// Data less than persistentDataDurableVersion is spilled on disk (or fully popped from the TLog);
	VersionMetricHandle persistentDataVersion,
	    persistentDataDurableVersion; // The last version number in the portion of the log (written|durable) to
	                                  // persistentData
	Version queuePoppedVersion; // The disk queue has been popped up until the location which represents this version.
	Version minPoppedTagVersion;
	Tag minPoppedTag; // The tag that makes tLog hold its data and cause tLog's disk queue increasing.

	// In-memory index: messages data at each version
	Deque<std::pair<Version, Standalone<VectorRef<uint8_t>>>> messageBlocks;
	// Mutations byte size for each version
	//     pair.first: normal mutations size
	//     pair.second: txs mutations size
	Map<Version, std::pair<int, int>> version_sizes;

	// Tlog group that this LogGeneration belongs to.
	Reference<TLogGroupData> tlogGroupData;

	// The maximum version that a proxy has told us that is committed (all TLogs have ack'd a commit for this version).
	Version knownCommittedVersion = 0;

	// Log interface id for this generation.
	// Different TLogGroups in the same generation in the same tlog server share the same log ID.
	UID logId;

	CounterCollection cc;
	Counter bytesInput;
	Counter bytesDurable;

	ProtocolVersion protocolVersion;

	// Storage teams tracker
	std::unordered_map<StorageTeamID, Reference<StorageTeamData>> storageTeamData;
	std::map<ptxn::StorageTeamID, std::vector<Tag>> storageTeams;

	Future<Void> terminated;
	AsyncTrigger stopCommit; // Trigger to stop the commit
	bool stopped = false; // Whether this generation has been stopped.
	bool initialized = false; // Whether this generation has been initialized.
	DBRecoveryCount recoveryCount; // How many recoveries happened in the past, served as generation id.

	// Versions related to Commit.
	NotifiedVersion version{ 0 }; // next version to commit

	// The disk queue has committed up until the queueCommittedVersion version.
	NotifiedVersion queueCommittedVersion;

	Version queueCommittingVersion = 0;

	Reference<AsyncVar<Reference<ILogSystem>>> logSystem;

	Version durableKnownCommittedVersion = 0;
	Version minKnownCommittedVersion = 0;

	Version newPersistentDataVersion;

	// Whether this tlog interface is removed, this can happen when a new master is elected and tlog interface recruited
	// by the old master gets removed.
	Future<Void> removed;
	PromiseStream<Future<Void>> addActor;
	Promise<Void> recoveryComplete, committingQueue;

	Version unrecoveredBefore = 1;
	Version recoveredAt = 1;

	// why do we need it, what does it do?
	int8_t locality; // data center id?
	UID recruitmentID;
	TLogSpillType logSpillType;
	PromiseStream<Void> warningCollectorInput;

	Reference<StorageTeamData> getStorageTeamData(const StorageTeamID& storageTeamID) {
		for (const auto& [id, data] : storageTeamData) {
			ASSERT_WE_THINK(data->storageTeamId.isValid());
		}
		return storageTeamData[storageTeamID];
	}

	// For a given version, get the serialized messages
	Optional<std::pair<Version, StringRef>> getSerializedTLogData(const Version& version,
	                                                              const StorageTeamID& strorageTeamID);

	// only callable after getStorageTeamData returns a null reference
	Reference<StorageTeamData> createStorageTeamData(StorageTeamID team, std::vector<Tag>& tags, Version popped = 0) {
		return storageTeamData[team] = makeReference<StorageTeamData>(team, tags, popped);
	}

	// only callable after getStorageTeamData returns a null reference
	void removeStorageTeam(StorageTeamID team) {
		storageTeamData.erase(team);
		storageTeams.erase(team);
	}

	explicit LogGenerationData(Reference<TLogGroupData> tlogGroupData,
	                           TLogInterface_PassivelyPull interf,
	                           UID recruitmentID,
	                           ProtocolVersion protocolVersion,
	                           TLogSpillType logSpillType,
	                           std::map<ptxn::StorageTeamID, std::vector<Tag>>& storageTeams,
	                           int8_t locality,
	                           const std::string& context)
	  : tlogGroupData(tlogGroupData), logId(interf.id()), cc("TLog", interf.id().toString()),
	    bytesInput("BytesInput", cc), bytesDurable("BytesDurable", cc), protocolVersion(protocolVersion),
	    storageTeams(storageTeams), terminated(tlogGroupData->terminated.getFuture()),
	    logSystem(new AsyncVar<Reference<ILogSystem>>()),
	    // These are initialized differently on init() or recovery
	    locality(locality), recruitmentID(recruitmentID), logSpillType(logSpillType) {
		specialCounter(cc, "Version", [this]() { return this->version.get(); });
		specialCounter(cc, "QueueCommittedVersion", [this]() { return this->queueCommittedVersion.get(); });
		specialCounter(cc, "KnownCommittedVersion", [this]() { return this->knownCommittedVersion; });
		// The locality and id of the tag that is responsible for making the TLog hold onto its oldest piece of data.
		// If disk queues are growing and no one is sure why, then you shall look at this to find the tag responsible
		// for why the TLog thinks it can't throw away data.
		specialCounter(cc, "SharedBytesInput", [tlogGroupData]() { return tlogGroupData->bytesInput; });
		specialCounter(cc, "SharedBytesDurable", [tlogGroupData]() { return tlogGroupData->bytesDurable; });
		specialCounter(cc, "SharedOverheadBytesInput", [tlogGroupData]() { return tlogGroupData->overheadBytesInput; });
		specialCounter(
		    cc, "SharedOverheadBytesDurable", [tlogGroupData]() { return tlogGroupData->overheadBytesDurable; });
		specialCounter(
		    cc, "PeekMemoryReserved", [tlogGroupData]() { return tlogGroupData->peekMemoryLimiter.activePermits(); });
		specialCounter(
		    cc, "PeekMemoryRequestsStalled", [tlogGroupData]() { return tlogGroupData->peekMemoryLimiter.waiters(); });
		specialCounter(cc, "Generation", [this]() { return this->recoveryCount; });
	}

	~LogGenerationData() {
		endRole(Role::TRANSACTION_LOG, logId, "Error", true);

		if (!terminated.isReady()) {
			tlogGroupData->bytesDurable += bytesInput.getValue() - bytesDurable.getValue();
			TraceEvent("TLogBytesWhenRemoved", logId)
			    .detail("SharedBytesInput", tlogGroupData->bytesInput)
			    .detail("SharedBytesDurable", tlogGroupData->bytesDurable)
			    .detail("LocalBytesInput", bytesInput.getValue())
			    .detail("LocalBytesDurable", bytesDurable.getValue());

			ASSERT_ABORT(tlogGroupData->bytesDurable <= tlogGroupData->bytesInput);

			Key logIdKey = BinaryWriter::toValue(logId, Unversioned());
			tlogGroupData->persistentData->clear(singleKeyRange(logIdKey.withPrefix(persistCurrentVersionKeys.begin)));
			tlogGroupData->persistentData->clear(
			    singleKeyRange(logIdKey.withPrefix(persistKnownCommittedVersionKeys.begin)));
			tlogGroupData->persistentData->clear(singleKeyRange(logIdKey.withPrefix(persistLocalityKeys.begin)));
			tlogGroupData->persistentData->clear(singleKeyRange(logIdKey.withPrefix(persistLogRouterTagsKeys.begin)));
			tlogGroupData->persistentData->clear(singleKeyRange(logIdKey.withPrefix(persistTxsTagsKeys.begin)));
			tlogGroupData->persistentData->clear(singleKeyRange(logIdKey.withPrefix(persistRecoveryCountKeys.begin)));
			tlogGroupData->persistentData->clear(singleKeyRange(logIdKey.withPrefix(persistProtocolVersionKeys.begin)));
			tlogGroupData->persistentData->clear(singleKeyRange(logIdKey.withPrefix(persistTLogSpillTypeKeys.begin)));
			tlogGroupData->persistentData->clear(singleKeyRange(logIdKey.withPrefix(persistRecoveryLocationKey)));
			tlogGroupData->persistentData->clear(singleKeyRange(logIdKey.withPrefix(persistStorageTeamKeys.begin)));
			Key msgKey = logIdKey.withPrefix(persistTagMessagesKeys.begin);
			tlogGroupData->persistentData->clear(KeyRangeRef(msgKey, strinc(msgKey)));
			Key msgRefKey = logIdKey.withPrefix(persistTagMessageRefsKeys.begin);
			tlogGroupData->persistentData->clear(KeyRangeRef(msgRefKey, strinc(msgRefKey)));
			Key poppedKey = logIdKey.withPrefix(persistTagPoppedKeys.begin);
			tlogGroupData->persistentData->clear(KeyRangeRef(poppedKey, strinc(poppedKey)));
		}
	}

	LogEpoch epoch() const { return recoveryCount; }

	bool shouldSpillByValue(StorageTeamID t) const {
		switch (logSpillType) {
		case TLogSpillType::VALUE:
			return true;
		case TLogSpillType::REFERENCE:
			return t == txsTeam;
		default:
			ASSERT(false);
			return false;
		}
	}

	bool shouldSpillByReference(StorageTeamID t) const { return !shouldSpillByValue(t); }
};

void TLogQueue::push(TLogQueueEntry const& qe, Reference<LogGenerationData> logData) {
	BinaryWriter wr(Unversioned()); // outer framing is not versioned
	wr << uint32_t(0);
	IncludeVersion(ProtocolVersion::withTLogQueueEntryRef()).write(wr); // payload is versioned
	wr << qe;
	wr << uint8_t(1);
	*(uint32_t*)wr.getData() = wr.getLength() - sizeof(uint32_t) - sizeof(uint8_t);
	const IDiskQueue::location startloc = queue->getNextPushLocation();
	// FIXME: push shouldn't return anything.  We should call getNextPushLocation() again.
	const IDiskQueue::location endloc = queue->push(wr.toValue());
	//TraceEvent("TLogQueueVersionWritten", dbgid).detail("Size", wr.getLength() - sizeof(uint32_t) - sizeof(uint8_t)).detail("Loc", loc);
	logData->versionLocation[qe.version] = std::make_pair(startloc, endloc);
}

void TLogQueue::forgetBefore(Version upToVersion, Reference<LogGenerationData> logData) {
	// Keep only the given and all subsequent version numbers
	// Find the first version >= upTo
	auto v = logData->versionLocation.lower_bound(upToVersion);
	if (v == logData->versionLocation.begin())
		return;

	if (v == logData->versionLocation.end()) {
		v = logData->versionLocation.lastItem();
	} else {
		v.decrementNonEnd();
	}

	logData->versionLocation.erase(logData->versionLocation.begin(),
	                               v); // ... and then we erase that previous version and all prior versions
}

void TLogQueue::pop(IDiskQueue::location upToLocation) {
	queue->pop(upToLocation);
}

void TLogQueue::updateVersionSizes(const TLogQueueEntry& result,
                                   TLogGroupData* logGroup,
                                   IDiskQueue::location start,
                                   IDiskQueue::location end) {
	auto it = logGroup->id_data.find(result.id);
	if (it != logGroup->id_data.end()) {
		it->second->versionLocation[result.version] = std::make_pair(start, end);
	}
}

// TODO: should deserialize messages to pairs of storage team -> message
void commitMessages(Reference<TLogGroupData> self,
                    Reference<LogGenerationData> logData,
                    Version version,
                    StringRef messages,
                    StorageTeamID storageTeamId) {
	// SOMEDAY: This method of copying messages is reasonably memory efficient, but it's still a lot of bytes copied.
	// Find a way to do the memory allocation right as we receive the messages in the network layer.

	// The structure of a message is:
	//   | Protocol Version | Main Header | Message Header | Message |
	// and we are only persisting Message Header + Message.
	static const size_t MESSAGE_OVERHEAD_BYTES =
	    ptxn::SerializerVersionOptionBytes + ptxn::getSerializedBytes<ptxn::details::MessageHeader>();
	StringRef decapitatedMessage = messages.substr(MESSAGE_OVERHEAD_BYTES);

	int64_t addedBytes = 0;
	int64_t overheadBytes = 0;
	int expectedBytes = 0;
	int txsBytes = 0;
	int msgSize = decapitatedMessage.size();
	if (!msgSize)
		return;

	// Grab the last block in the blocks list so we can share its arena
	// We pop all of the elements of it to create a "fresh" vector that starts at the end of the previous vector
	Standalone<VectorRef<uint8_t>> block;
	if (logData->messageBlocks.empty()) {
		block = Standalone<VectorRef<uint8_t>>();
		block.reserve(block.arena(), std::max<int64_t>(SERVER_KNOBS->TLOG_MESSAGE_BLOCK_BYTES, msgSize));
	} else {
		block = logData->messageBlocks.back().second;
	}

	block.pop_front(block.size());

	if (decapitatedMessage.size() > block.capacity() - block.size()) {
		logData->messageBlocks.emplace_back(version, block);
		addedBytes += int64_t(block.size()) * SERVER_KNOBS->TLOG_MESSAGE_BLOCK_OVERHEAD_FACTOR;
		block = Standalone<VectorRef<uint8_t>>();
		block.reserve(block.arena(), std::max<int64_t>(SERVER_KNOBS->TLOG_MESSAGE_BLOCK_BYTES, msgSize));
	}

	TraceEvent(SevDebug, "TLogCommitMessages")
	    .detail("Version", version)
	    .detail("StorageTeamID", storageTeamId)
	    .detail("RawMessage", messages)
	    .detail("SharedTLogID", self->dbgid)
	    .detail("TLogGroupID", self->tlogGroupID)
	    .detail("LogId", logData->logId);
	block.append(block.arena(), decapitatedMessage.begin(), msgSize);

	Reference<LogGenerationData::StorageTeamData> storageTeamData = logData->getStorageTeamData(storageTeamId);
	if (!storageTeamData) {
		storageTeamData = logData->createStorageTeamData(storageTeamId, logData->storageTeams[storageTeamId]);
	}

	ASSERT(storageTeamData->versionMessages.find(version) == storageTeamData->versionMessages.end());
	StringRef storedMessage(block.end() - msgSize, msgSize);
	const auto expectedStoredMessageSize = storedMessage.expectedSize();

	storageTeamData->versionMessages[version] = std::make_pair(storedMessage, block.arena());

	if (expectedStoredMessageSize > SERVER_KNOBS->MAX_MESSAGE_SIZE) {
		TraceEvent(SevWarnAlways, "LargeMessage").detail("Size", expectedStoredMessageSize);
	}
	if (storageTeamId != txsTeam) {
		expectedBytes += expectedStoredMessageSize;
	} else {
		txsBytes += expectedStoredMessageSize;
	}

	// The factor of VERSION_MESSAGES_OVERHEAD is intended to be an overestimate of the actual memory used
	// to store this data in a std::deque. In practice, this number is probably something like 528/512
	// ~= 1.03, but this could vary based on the implementation. There will also be a fixed overhead per
	// std::deque, but its size should be trivial relative to the size of the TLog queue and can be thought
	// of as increasing the capacity of the queue slightly.
	overheadBytes += SERVER_KNOBS->VERSION_MESSAGES_ENTRY_BYTES_WITH_OVERHEAD;

	msgSize -= messages.size();

	logData->messageBlocks.emplace_back(version, block);
	addedBytes += int64_t(block.size()) * SERVER_KNOBS->TLOG_MESSAGE_BLOCK_OVERHEAD_FACTOR;
	addedBytes += overheadBytes;

	logData->version_sizes[version] = std::make_pair(expectedBytes, txsBytes);
	logData->bytesInput += addedBytes;
	self->bytesInput += addedBytes;
	self->overheadBytesInput += overheadBytes;

	//TraceEvent("TLogPushed", self->dbgid).detail("Bytes", addedBytes).detail("MessageBytes", messages.size()).detail("Tags", tags.size()).detail("ExpectedBytes", expectedBytes).detail("MCount", mCount).detail("TCount", tCount);
}

ACTOR Future<Void> doQueueCommit(Reference<TLogGroupData> self,
                                 Reference<LogGenerationData> logData,
                                 std::vector<Reference<LogGenerationData>> missingFinalCommit) {
	state Version ver = logData->version.get();
	state Version commitNumber = self->queueCommitBegin + 1;
	state Version knownCommittedVersion = logData->knownCommittedVersion;
	self->queueCommitBegin = commitNumber;
	logData->queueCommittingVersion = ver;

	g_network->setCurrentTask(TaskPriority::TLogCommitReply);
	Future<Void> c = self->persistentQueue->commit();
	self->diskQueueCommitBytes = 0;
	self->largeDiskQueueCommitBytes.set(false);

	wait(ioDegradedOrTimeoutError(
	    c, SERVER_KNOBS->MAX_STORAGE_COMMIT_TIME, self->degraded, SERVER_KNOBS->TLOG_DEGRADED_DURATION));
	if (g_network->isSimulated() && !g_simulator.speedUpSimulation && BUGGIFY_WITH_PROB(0.0001)) {
		wait(delay(6.0));
	}
	wait(self->queueCommitEnd.whenAtLeast(commitNumber - 1));

	// Calling check_yield instead of yield to avoid a destruction ordering problem in simulation
	if (g_network->check_yield(g_network->getCurrentTask())) {
		wait(delay(0, g_network->getCurrentTask()));
	}

	ASSERT(ver > logData->queueCommittedVersion.get());

	logData->durableKnownCommittedVersion = knownCommittedVersion;

	//TraceEvent("TLogCommitDurable", self->dbgid).detail("Version", ver);

	logData->queueCommittedVersion.set(ver);
	self->queueCommitEnd.set(commitNumber);

	for (auto& it : missingFinalCommit) {
		TraceEvent("TLogCommitMissingFinalCommit", self->dbgid)
		    .detail("LogId", logData->logId)
		    .detail("Version", it->version.get())
		    .detail("QueueVer", it->queueCommittedVersion.get());
		TEST(true); // A TLog was replaced before having a chance to commit its queue
		it->queueCommittedVersion.set(it->version.get());
	}
	return Void();
}

ACTOR Future<Void> commitQueue(Reference<TLogGroupData> self) {
	state Reference<LogGenerationData> logData;
	state std::vector<Reference<LogGenerationData>> missingFinalCommit;

	loop {
		int foundCount = 0;
		for (auto it : self->id_data) {
			if (!it.second->stopped) {
				logData = it.second;
				foundCount++;
			} else if (it.second->version.get() >
			           std::max(it.second->queueCommittingVersion, it.second->queueCommittedVersion.get())) {
				missingFinalCommit.push_back(it.second);
			}
		}

		ASSERT(foundCount < 2);
		if (!foundCount) {
			wait(self->newLogData.onTrigger());
			continue;
		}
		ASSERT(logData->tlogGroupData->tlogGroupID == self->tlogGroupID);
		TraceEvent("CommitQueueNewLog", self->dbgid)
		    .detail("LogId", logData->logId)
		    .detail("Version", logData->version.get())
		    .detail("Committing", logData->queueCommittingVersion)
		    .detail("Commmitted", logData->queueCommittedVersion.get());
		if (logData->committingQueue.canBeSet()) {
			logData->committingQueue.send(Void());
		}

		loop {
			if (logData->stopped && logData->version.get() == std::max(logData->queueCommittingVersion,
			                                                           logData->queueCommittedVersion.get())) {
				wait(logData->queueCommittedVersion.whenAtLeast(logData->version.get()));
				break;
			}

			choose {
				when(wait(logData->version.whenAtLeast(
				    std::max(logData->queueCommittingVersion, logData->queueCommittedVersion.get()) + 1))) {
					while (self->queueCommitBegin != self->queueCommitEnd.get() &&
					       !self->largeDiskQueueCommitBytes.get()) {
						wait(self->queueCommitEnd.whenAtLeast(self->queueCommitBegin) ||
						     self->largeDiskQueueCommitBytes.onChange());
					}
					if (logData->version.get() > logData->queueCommittedVersion.get()) {
						self->sharedActors.send(doQueueCommit(self, logData, missingFinalCommit));
					}
					missingFinalCommit.clear();
				}
				when(wait(self->newLogData.onTrigger())) {}
			}
		}
	}
}

ACTOR Future<Void> tLogCommit(Reference<TLogGroupData> self,
                              TLogCommitRequest req,
                              Reference<LogGenerationData> logData) {
	state Span span("TLog:tLogCommit"_loc, req.spanID);
	state Optional<UID> tlogDebugID;
	if (req.debugID.present()) {
		tlogDebugID = nondeterministicRandom()->randomUniqueID();
		g_traceBatch.addAttach("CommitAttachID", req.debugID.get().first(), tlogDebugID.get().first());
		g_traceBatch.addEvent("CommitDebug", tlogDebugID.get().first(), "TLog.tLogCommit.BeforeWaitForVersion");
	}

	logData->minKnownCommittedVersion = std::max(logData->minKnownCommittedVersion, req.minKnownCommittedVersion);
	wait(logData->version.whenAtLeast(req.prevVersion));

	// Calling check_yield instead of yield to avoid a destruction ordering problem in simulation
	if (g_network->check_yield(g_network->getCurrentTask())) {
		wait(delay(0, g_network->getCurrentTask()));
	}

	state double waitStartT = 0;
	while (self->bytesInput - self->bytesDurable >= SERVER_KNOBS->TLOG_HARD_LIMIT_BYTES && !logData->stopped) {
		if (now() - waitStartT >= 1) {
			TraceEvent(SevWarn, "TLogUpdateLag", logData->logId).detail("Version", logData->version.get());
			waitStartT = now();
		}
		wait(delayJittered(.005, TaskPriority::TLogCommit));
	}

	if (logData->stopped) {
		req.reply.sendError(tlog_stopped());
		return Void();
	}

	state double beforeCommitT = now();

	// Not a duplicate (check relies on critical section between here self->version.set() below!)
	state bool isNotDuplicate = (logData->version.get() == req.prevVersion);
	if (isNotDuplicate) {
		if (req.debugID.present())
			g_traceBatch.addEvent("CommitDebug", tlogDebugID.get().first(), "TLog.tLogCommit.Before");

		//TraceEvent("TLogCommit", logData->logId).detail("Version", req.version);
		for (auto& message : req.messages) {
			commitMessages(self, logData, req.version, message.second, message.first);
		}

		logData->knownCommittedVersion = std::max(logData->knownCommittedVersion, req.knownCommittedVersion);

		TLogQueueEntryRef qe;
		// Log the changes to the persistent queue, to be committed by commitQueue()
		qe.version = req.version;
		qe.knownCommittedVersion = logData->knownCommittedVersion;
		qe.id = logData->logId;
		qe.storageTeams.reserve(req.messages.size());
		qe.messages.reserve(req.messages.size());
		for (auto& message : req.messages) {
			qe.storageTeams.push_back(message.first);
			qe.messages.push_back(message.second);
		}
		self->persistentQueue->push(qe, logData);

		self->diskQueueCommitBytes += qe.expectedSize();
		if (self->diskQueueCommitBytes > SERVER_KNOBS->MAX_QUEUE_COMMIT_BYTES) {
			self->largeDiskQueueCommitBytes.set(true);
		}

		// Notifies the commitQueue actor to commit persistentQueue, and also unblocks tLogPeekMessages actors
		logData->version.set(req.version);

		if (req.debugID.present())
			g_traceBatch.addEvent("CommitDebug", tlogDebugID.get().first(), "TLog.tLogCommit.AfterTLogCommit");
	}
	// Send replies only once all prior messages have been received and committed.
	state Future<Void> stopped = logData->stopCommit.onTrigger();
	wait(timeoutWarning(
	    logData->queueCommittedVersion.whenAtLeast(req.version) || stopped, 0.1, logData->warningCollectorInput));

	if (stopped.isReady()) {
		ASSERT(logData->stopped);
		req.reply.sendError(tlog_stopped());
		return Void();
	}

	if (isNotDuplicate) {
		self->commitLatencyDist->sampleSeconds(now() - beforeCommitT);
	}

	if (req.debugID.present())
		g_traceBatch.addEvent("CommitDebug", tlogDebugID.get().first(), "TLog.tLogCommit.After");

	req.reply.send(logData->durableKnownCommittedVersion);
	return Void();
}

Optional<std::pair<Version, StringRef>> LogGenerationData::getSerializedTLogData(const Version& version,
                                                                                 const StorageTeamID& storageTeamID) {

	auto pStorageTeamData = getStorageTeamData(storageTeamID);
	// by lower_bound, if we pass in 10, we might get 12, and return 12
	auto iter = pStorageTeamData->versionMessages.lower_bound(version);
	if (iter == pStorageTeamData->versionMessages.end()) {
		return Optional<std::pair<Version, StringRef>>();
	}

	return std::make_pair(iter->first, iter->second.first);
}

static const size_t TLOG_PEEK_REQUEST_REPLY_SIZE_CRITERIA = 1024 * 1024;

ACTOR Future<Void> tLogPeekMessages(TLogPeekRequest req, Reference<LogGenerationData> logData) {
	ASSERT(logData.isValid());

	wait(logData->version.whenAtLeast(req.beginVersion));

	if (!logData->getStorageTeamData(req.storageTeamID).isValid()) {
		req.reply.sendError(storage_team_id_not_found());
		return Void();
	}

	TLogPeekReply reply;
	TLogSubsequencedMessageSerializer serializer(req.storageTeamID);
	int versionCount = 0;
	Version version = req.beginVersion;
	Optional<std::pair<Version, StringRef>> serializedData;
	while ((serializedData = logData->getSerializedTLogData(version, req.storageTeamID)).present()) {
		auto result = serializedData.get();
		version = result.first;

		if (req.endVersion.present() && version > req.endVersion.get()) {
			// [will remove afterPR] previously has a bug, if first run version is bigger than req, it will be returned
			// anyways.
			break;
		}
		auto& data = result.second;

		if (!reply.beginVersion.present()) {
			reply.beginVersion = version;
		}

		serializer.writeSerializedVersionSection(data);
		++version;
		versionCount++;

		if (serializer.getTotalBytes() > TLOG_PEEK_REQUEST_REPLY_SIZE_CRITERIA) {
			break;
		}
	}

	Standalone<StringRef> serialized = serializer.getSerialized();

	reply.arena = serialized.arena();
	reply.data = serialized;
	reply.endVersion = version;
	if (versionCount == 0) {
		// Up to this version is empty. This is because within a group,
		// all version data must be continuously received.
		reply.endVersion = logData->version.get() + 1;
	}
	reply.maxKnownVersion = logData->version.get();
	reply.minKnownCommittedVersion = logData->minKnownCommittedVersion;

	req.reply.send(reply);

	return Void();
}

ACTOR Future<Void> initPersistentState(Reference<TLogGroupData> self, Reference<LogGenerationData> logData) {
	wait(self->persistentDataCommitLock.take());
	state FlowLock::Releaser commitLockReleaser(self->persistentDataCommitLock);

	// PERSIST: Initial setup of persistentData for a brand new tLog for a new database
	state IKeyValueStore* storage = self->persistentData;
	wait(ioTimeoutError(storage->init(), SERVER_KNOBS->TLOG_MAX_CREATE_DURATION));
	storage->set(persistFormat);
	storage->set(
	    KeyValueRef(BinaryWriter::toValue(logData->logId, Unversioned()).withPrefix(persistCurrentVersionKeys.begin),
	                BinaryWriter::toValue(logData->version.get(), Unversioned())));
	storage->set(KeyValueRef(
	    BinaryWriter::toValue(logData->logId, Unversioned()).withPrefix(persistKnownCommittedVersionKeys.begin),
	    BinaryWriter::toValue(logData->knownCommittedVersion, Unversioned())));
	storage->set(KeyValueRef(BinaryWriter::toValue(logData->logId, Unversioned()).withPrefix(persistLocalityKeys.begin),
	                         BinaryWriter::toValue(logData->locality, Unversioned())));
	storage->set(
	    KeyValueRef(BinaryWriter::toValue(logData->logId, Unversioned()).withPrefix(persistRecoveryCountKeys.begin),
	                BinaryWriter::toValue(logData->recoveryCount, Unversioned())));
	storage->set(
	    KeyValueRef(BinaryWriter::toValue(logData->logId, Unversioned()).withPrefix(persistProtocolVersionKeys.begin),
	                BinaryWriter::toValue(logData->protocolVersion, Unversioned())));
	storage->set(
	    KeyValueRef(BinaryWriter::toValue(logData->logId, Unversioned()).withPrefix(persistTLogSpillTypeKeys.begin),
	                BinaryWriter::toValue(logData->logSpillType, AssumeVersion(logData->protocolVersion))));
	storage->set(KeyValueRef(
	    BinaryWriter::toValue(logData->logId, IncludeVersion(ProtocolVersion::withPartitionTransaction()))
	        .withPrefix(persistStorageTeamKeys.begin),
	    BinaryWriter::toValue(logData->storageTeams, IncludeVersion(ProtocolVersion::withPartitionTransaction()))));

	for (auto team : logData->storageTeams) {
		ASSERT(!logData->getStorageTeamData(team.first));
		logData->createStorageTeamData(team.first, team.second);
	}

	TraceEvent("TLogInitCommit", logData->logId);
	wait(ioTimeoutError(self->persistentData->commit(), SERVER_KNOBS->TLOG_MAX_CREATE_DURATION));
	return Void();
}

ACTOR Future<Void> rejoinMasters(Reference<TLogServerData> self,
                                 TLogInterface_PassivelyPull tli,
                                 DBRecoveryCount recoveryCount,
                                 Future<Void> registerWithMaster,
                                 bool isPrimary) {
	state UID lastMasterID(0, 0);
	loop {
		auto const& inf = self->dbInfo->get();
		bool isDisplaced =
		    !std::count(inf.priorCommittedLogServers.begin(), inf.priorCommittedLogServers.end(), tli.id());
		if (isPrimary) {
			isDisplaced =
			    isDisplaced && inf.recoveryCount >= recoveryCount && inf.recoveryState != RecoveryState::UNINITIALIZED;
		} else {
			isDisplaced = isDisplaced &&
			              ((inf.recoveryCount > recoveryCount && inf.recoveryState != RecoveryState::UNINITIALIZED) ||
			               (inf.recoveryCount == recoveryCount && inf.recoveryState == RecoveryState::FULLY_RECOVERED));
		}
		isDisplaced = isDisplaced && !inf.logSystemConfig.hasTLog(tli.id());
		if (isDisplaced) {
			TraceEvent("TLogDisplaced", tli.id())
			    .detail("Reason", "DBInfoDoesNotContain")
			    .detail("RecoveryCount", recoveryCount)
			    .detail("InfRecoveryCount", inf.recoveryCount)
			    .detail("RecoveryState", (int)inf.recoveryState)
			    .detail("LogSysConf", describe(inf.logSystemConfig.tLogs))
			    .detail("PriorLogs", describe(inf.priorCommittedLogServers))
			    .detail("OldLogGens", inf.logSystemConfig.oldTLogs.size());
			if (BUGGIFY)
				wait(delay(SERVER_KNOBS->BUGGIFY_WORKER_REMOVED_MAX_LAG * deterministicRandom()->random01()));
			throw worker_removed();
		}

		if (registerWithMaster.isReady()) {
			if (self->dbInfo->get().master.id() != lastMasterID) {
				// The TLogRejoinRequest is needed to establish communications with a new master, which doesn't have our
				// TLogInterface
				TLogRejoinRequest req(tli);
				TraceEvent("TLogRejoining", tli.id()).detail("Master", self->dbInfo->get().master.id());
				choose {
					when(TLogRejoinReply rep =
					         wait(brokenPromiseToNever(self->dbInfo->get().master.tlogRejoin.getReply(req)))) {
						if (rep.masterIsRecovered)
							lastMasterID = self->dbInfo->get().master.id();
					}
					when(wait(self->dbInfo->onChange())) {}
				}
			} else {
				wait(self->dbInfo->onChange());
			}
		} else {
			wait(registerWithMaster || self->dbInfo->onChange());
		}
	}
}

ACTOR Future<TLogGroupLockResult> lockTLogGroup(Reference<TLogGroupData> groupData,
                                                Reference<LogGenerationData> logData) {
	state Version stopVersion = logData->version.get();
	TEST(true); // TLog stopped by recovering master
	TEST(logData->stopped); // logData already stopped
	TEST(!logData->stopped); // logData not yet stopped
	TraceEvent("TLogGroupLock", groupData->dbgid)
	    .detail("LogId", logData->logId)
	    .detail("Ver", stopVersion)
	    .detail("TLogGroupID", groupData->tlogGroupID)
	    .detail("IsStopped", logData->stopped)
	    .detail("QueueCommitted", logData->queueCommittedVersion.get());

	logData->stopped = true;
	if (!logData->recoveryComplete.isSet()) {
		logData->recoveryComplete.sendError(end_of_stream());
	}

	wait(logData->queueCommittedVersion.whenAtLeast(stopVersion));

	ASSERT(stopVersion == logData->version.get());

	Version kcv = logData->knownCommittedVersion;
	TraceEvent("TLogGroupLock2", groupData->dbgid)
	    .detail("LogId", logData->logId)
	    .detail("Ver", stopVersion)
	    .detail("TLogGroupID", groupData->tlogGroupID)
	    .detail("IsStopped", logData->stopped)
	    .detail("QueueCommitted", logData->queueCommittedVersion.get())
	    .detail("KnownCommitted", kcv);

	TLogGroupLockResult groupResult;
	groupResult.id = groupData->tlogGroupID;
	groupResult.end = stopVersion;
	groupResult.knownCommittedVersion = kcv;

	return groupResult;
}

ACTOR Future<Void> lockTLogServer(
    Reference<TLogServerData> self,
    ReplyPromise<TLogLockResult> reply,
    std::shared_ptr<std::unordered_map<TLogGroupID, Reference<LogGenerationData>>> activeGeneration) {
	state std::unordered_map<TLogGroupID, Reference<TLogGroupData>>::iterator team;
	TraceEvent("TLogLock", self->dbgid).detail("WrokerID", self->workerID);
	state TLogLockResult result;
	state std::vector<Future<TLogGroupLockResult>> futures;
	for (team = self->tlogGroups.begin(); team != self->tlogGroups.end(); team++) {
		TLogGroupID id = team->first;
		auto tlogGroup = activeGeneration->find(id);
		Reference<LogGenerationData> logDataActiveGeneration = tlogGroup->second;
		futures.push_back(lockTLogGroup(team->second, logDataActiveGeneration));
	}
	state std::vector<TLogGroupLockResult> groupResults = wait(getAll(futures));
	result.groupResults.swap(groupResults);
	TraceEvent("TLogLock2", self->dbgid).detail("WrokerID", self->workerID);
	reply.send(result);
	return Void();
}

// Services a peek request.
ACTOR Future<Void> servicePeekRequest(
    Reference<TLogServerData> self,
    TLogPeekRequest req,
    std::shared_ptr<std::unordered_map<TLogGroupID, Reference<LogGenerationData>>> activeGeneration) {
	// block until dbInfo is ready, otherwise we won't find the correct TLog group
	while (self->dbInfo->get().recoveryState < RecoveryState::ACCEPTING_COMMITS) {
		wait(self->dbInfo->onChange());
	}

	auto tLogGroupID =
	    tLogGroupByStorageTeamID(self->dbInfo->get().logSystemConfig.tLogs[0].tLogGroupIDs, req.storageTeamID);
	auto tlogGroup = activeGeneration->find(tLogGroupID);
	TEST(tlogGroup == activeGeneration->end()); // TLog peek: group not found
	if (tlogGroup == activeGeneration->end()) {
		TraceEvent("TLogPeekGroupNotFound", self->dbgid).detail("Group", tLogGroupID).detail("Team", req.storageTeamID);
		req.reply.sendError(tlog_group_not_found());
		return Void();
	}
	Reference<LogGenerationData> logData = tlogGroup->second;
	logData->addActor.send(tLogPeekMessages(req, logData));

	return Void();
}

ACTOR Future<Void> serveTLogInterface_PassivelyPull(
    Reference<TLogServerData> self,
    TLogInterface_PassivelyPull tli,
    std::shared_ptr<std::unordered_map<TLogGroupID, Reference<LogGenerationData>>> activeGeneration) {
	ASSERT(activeGeneration->size());

	state UID recruitmentID = activeGeneration->begin()->second->recruitmentID;
	state Future<Void> dbInfoChange = Void();
	loop choose {
		when(wait(dbInfoChange)) {
			dbInfoChange = self->dbInfo->onChange();
			bool found = false;
			if (self->dbInfo->get().recoveryState >= RecoveryState::ACCEPTING_COMMITS) {
				for (auto& logs : self->dbInfo->get().logSystemConfig.tLogs) {
					if (std::count(logs.tLogs.begin(), logs.tLogs.end(), tli.id())) {
						found = true;
						break;
					}
				}
			}
			if (found && self->dbInfo->get().logSystemConfig.recruitmentID == recruitmentID) {
				for (auto& logData : *activeGeneration) {
					logData.second->logSystem->set(ILogSystem::fromServerDBInfo(self->dbgid, self->dbInfo->get()));
				}
			} else {
				for (auto& logData : *activeGeneration) {
					logData.second->logSystem->set(Reference<ILogSystem>());
				}
			}
		}
		when(TLogCommitRequest req = waitNext(tli.commit.getFuture())) {
			auto tlogGroup = activeGeneration->find(req.tLogGroupID);
			TEST(tlogGroup == activeGeneration->end()); // TLog group not found
			if (tlogGroup == activeGeneration->end()) {
				TraceEvent(SevWarn, "TLogCommitUnknownGroup", self->dbgid).detail("Group", req.tLogGroupID);
				req.reply.sendError(tlog_group_not_found());
				continue;
			}

			Reference<LogGenerationData> logData = tlogGroup->second;
			TEST(logData->stopped); // TLogCommitRequest while stopped
			if (logData->stopped) {
				req.reply.sendError(tlog_stopped());
				continue;
			}

			// Update storage teams.
			for (auto t : req.addedTeams) {
				logData->storageTeams.emplace(t, req.teamToTags.find(t)->second);
				logData->createStorageTeamData(t, logData->storageTeams[t]);
			}

			for (auto& t : req.removedTeams) {
				logData->removeStorageTeam(t);
			}

			logData->addActor.send(tLogCommit(logData->tlogGroupData, req, logData));
		}
		when(TLogPeekRequest req = waitNext(tli.peek.getFuture())) {
			self->addActors.send(servicePeekRequest(self, req, activeGeneration));
		}
		when(ReplyPromise<TLogLockResult> reply = waitNext(tli.lock.getFuture())) {
			wait(lockTLogServer(self, reply, activeGeneration));
		}
	}
}

void removeLog(Reference<LogGenerationData> logData) {
	Reference<TLogGroupData> self = logData->tlogGroupData;
	Reference<TLogServerData> tlogServerData = self->tLogServerData;
	TraceEvent("TLogRemoved", self->dbgid)
	    .detail("LogId", logData->logId)
	    .detail("Input", logData->bytesInput.getValue())
	    .detail("Durable", logData->bytesDurable.getValue());
	logData->stopped = true;
	if (!logData->recoveryComplete.isSet()) {
		logData->recoveryComplete.sendError(end_of_stream());
	}

	self->id_data.erase(logData->logId);
	logData->addActor = PromiseStream<Future<Void>>(); // there could be items still in the promise stream if one of the
	// actors threw an error immediately
	tlogServerData->logGenerations.erase(logData->logId);

	if (tlogServerData->logGenerations.size() == 0) {
		throw worker_removed();
	}
}

ACTOR Future<Void> tLogCore(
    Reference<TLogServerData> self,
    std::shared_ptr<std::unordered_map<TLogGroupID, Reference<LogGenerationData>>> activeGeneration,
    TLogInterface_PassivelyPull tli,
    UID recruitmentId) {
	if (self->removed.isReady()) {
		wait(delay(0)); // to avoid iterator invalidation in restorePersistentState when removed is already ready
		ASSERT(self->removed.isError());

		if (self->removed.getError().code() != error_code_worker_removed) {
			throw self->removed.getError();
		}

		for (auto& logGroup : *activeGeneration) {
			removeLog(logGroup.second);
		}
		return Void();
	}

	TraceEvent("TLogCore", self->dbgid).detail("WorkerID", self->workerID);
	self->actorsPerRecruitment[recruitmentId].send(self->removed);

	// FIXME: update tlogMetrics to include new information, or possibly only have one copy for the shared instance
	for (auto& logGroup : *activeGeneration) {
		self->sharedActors.send(traceCounters("TLogMetrics",
		                                      logGroup.second->logId,
		                                      SERVER_KNOBS->STORAGE_LOGGING_DELAY,
		                                      &logGroup.second->cc,
		                                      logGroup.second->logId.toString() + "/TLogMetrics"));
	}
	startRole(Role::TRANSACTION_LOG, tli.id(), self->workerID, { { "SharedTLog", self->dbgid.shortString() } });

	// TODO: remove this so that a log generation is only tracked once
	self->actorsPerRecruitment[recruitmentId].send(traceRole(Role::TRANSACTION_LOG, tli.id()));
	self->actorsPerRecruitment[recruitmentId].send(serveTLogInterface_PassivelyPull(self, tli, activeGeneration));
	self->actorsPerRecruitment[recruitmentId].send(waitFailureServer(tli.waitFailure.getFuture()));
	state Future<Void> error = actorCollection(self->actorsPerRecruitment[recruitmentId].getFuture());

	try {
		wait(error);
		throw internal_error();
	} catch (Error& e) {
		if (e.code() != error_code_worker_removed)
			throw;
		for (auto& logGroup : *activeGeneration) {
			removeLog(logGroup.second);
		}
		return Void();
	}
}

ACTOR Future<Void> checkEmptyQueue(Reference<TLogGroupData> self) {
	TraceEvent("TLogCheckEmptyQueueBegin", self->dbgid);
	try {
		bool recoveryFinished = wait(self->persistentQueue->initializeRecovery(0));
		if (recoveryFinished)
			return Void();
		TLogQueueEntry r = wait(self->persistentQueue->readNext(self.getPtr())); // readNext might return endofstream
		throw internal_error();
	} catch (Error& e) {
		if (e.code() != error_code_end_of_stream)
			throw;
		TraceEvent("TLogCheckEmptyQueueEnd", self->dbgid);
		return Void();
	}
}

ACTOR Future<Void> checkRecovered(Reference<TLogGroupData> self) {
	TraceEvent("TLogCheckRecoveredBegin", self->dbgid);
	Optional<Value> v = wait(self->persistentData->readValue(StringRef()));
	TraceEvent("TLogCheckRecoveredEnd", self->dbgid);
	return Void();
}

bool tlogTerminated(Reference<TLogGroupData> self,
                    IKeyValueStore* persistentData,
                    TLogQueue* persistentQueue,
                    Error const& e) {
	// Dispose the IKVS (destroying its data permanently) only if this shutdown is definitely permanent.  Otherwise just
	// close it.
	// assign an empty PromiseSteam to self->sharedActors would delete the referenfce of the internal queue in
	// PromiseSteam thus the actors can be cancelled in the case there is no more references of the old queue
	self->sharedActors = PromiseStream<Future<Void>>();
	if (e.code() == error_code_worker_removed || e.code() == error_code_recruitment_failed) {
		persistentData->dispose();
		persistentQueue->dispose();
	} else {
		persistentData->close();
		persistentQueue->close();
	}

	if (e.code() == error_code_worker_removed || e.code() == error_code_recruitment_failed ||
	    e.code() == error_code_file_not_found || e.code() == error_code_operation_cancelled) {
		TraceEvent("TLogTerminated", self->dbgid).error(e, true);
		return true;
	} else
		return false;
}

void stopAllTLogs(Reference<TLogServerData> self, UID newLogId) {
	for (auto& team : self->tlogGroups) {
		for (auto it : team.second->id_data) {
			if (!it.second->stopped) {
				TraceEvent("TLogStoppedByNewRecruitment", self->dbgid)
				    .detail("LogId", it.second->logId)
				    .detail("StoppedId", it.first)
				    .detail("RecruitedId", newLogId)
				    .detail("EndEpoch", it.second->logSystem->get().getPtr() != 0);
				if (it.second->committingQueue.canBeSet()) {
					it.second->committingQueue.sendError(worker_removed());
				}
			}
			it.second->stopped = true;
			if (!it.second->recoveryComplete.isSet()) {
				it.second->recoveryComplete.sendError(end_of_stream());
			}
			it.second->stopCommit.trigger();
		}
	}
}

ACTOR Future<Void> restorePersistentState(Reference<TLogGroupData> self,
                                          LocalityData locality,
                                          Reference<TLogServerData> serverData) {
	state double startt = now();
	state Reference<LogGenerationData> logData;
	state KeyRange tagKeys;
	// PERSIST: Read basic state from persistentData; replay persistentQueue but don't erase it

	state IKeyValueStore* storage = self->persistentData;
	wait(storage->init());
	state Future<Optional<Value>> fFormat = storage->readValue(persistFormat.key);
	state Future<Optional<Value>> fRecoveryLocation = storage->readValue(persistRecoveryLocationKey);
	state Future<RangeResult> fVers =
	    storage->readRange(persistCurrentVersionKeys); // these kv must be persisted so that we can restore
	state Future<RangeResult> fKnownCommitted = storage->readRange(persistKnownCommittedVersionKeys);
	state Future<RangeResult> fLocality = storage->readRange(persistLocalityKeys);
	state Future<RangeResult> fLogRouterTags = storage->readRange(persistLogRouterTagsKeys);
	state Future<RangeResult> fTxsTags = storage->readRange(persistTxsTagsKeys);
	state Future<RangeResult> fRecoverCounts = storage->readRange(persistRecoveryCountKeys);
	state Future<RangeResult> fProtocolVersions = storage->readRange(persistProtocolVersionKeys);
	state Future<RangeResult> fTLogSpillTypes = storage->readRange(persistTLogSpillTypeKeys);
	state Future<RangeResult> fStorageTeams = storage->readRange(persistStorageTeamKeys);

	// FIXME: metadata in queue?

	wait(waitForAll(std::vector{ fFormat, fRecoveryLocation }));
	wait(waitForAll(std::vector{ fVers,
	                             fKnownCommitted,
	                             fLocality,
	                             fLogRouterTags,
	                             fTxsTags,
	                             fRecoverCounts,
	                             fProtocolVersions,
	                             fTLogSpillTypes }));

	if (fFormat.get().present() && !persistFormatReadableRange.contains(fFormat.get().get())) {
		// FIXME: remove when we no longer need to test upgrades from 4.X releases
		if (g_network->isSimulated()) {
			TraceEvent("ElapsedTime").detail("SimTime", now()).detail("RealTime", 0).detail("RandomUnseed", 0);
			flushAndExit(0);
		}

		TraceEvent(SevError, "UnsupportedDBFormat", self->dbgid)
		    .detail("Format", fFormat.get().get())
		    .detail("Expected", persistFormat.value);
		throw worker_recovery_failed();
	}

	if (!fFormat.get().present()) {
		RangeResult v = wait(self->persistentData->readRange(KeyRangeRef(StringRef(), LiteralStringRef("\xff")), 1));
		if (!v.size()) {
			TEST(true); // The DB is completely empty, so it was never initialized.  Delete it.
			throw worker_removed();
		} else {
			// This should never happen
			TraceEvent(SevError, "NoDBFormatKey", self->dbgid).detail("FirstKey", v[0].key);
			ASSERT(false);
			throw worker_recovery_failed();
		}
	}

	state std::vector<Future<ErrorOr<Void>>> removed;

	ASSERT(fFormat.get().get() == LiteralStringRef("FoundationDB/LogServer/3/0"));

	ASSERT(fVers.get().size() == fRecoverCounts.get().size());

	state std::map<UID, int8_t> id_locality;
	for (auto it : fLocality.get()) {
		id_locality[BinaryReader::fromStringRef<UID>(it.key.removePrefix(persistLocalityKeys.begin), Unversioned())] =
		    BinaryReader::fromStringRef<int8_t>(it.value, Unversioned());
	}

	state std::map<UID, int> id_logRouterTags;
	for (auto it : fLogRouterTags.get()) {
		id_logRouterTags[BinaryReader::fromStringRef<UID>(it.key.removePrefix(persistLogRouterTagsKeys.begin),
		                                                  Unversioned())] =
		    BinaryReader::fromStringRef<int>(it.value, Unversioned());
	}

	state std::map<UID, int> id_txsTags;
	for (auto it : fTxsTags.get()) {
		id_txsTags[BinaryReader::fromStringRef<UID>(it.key.removePrefix(persistTxsTagsKeys.begin), Unversioned())] =
		    BinaryReader::fromStringRef<int>(it.value, Unversioned());
	}

	state std::map<UID, Version> id_knownCommitted;
	for (auto it : fKnownCommitted.get()) {
		id_knownCommitted[BinaryReader::fromStringRef<UID>(it.key.removePrefix(persistKnownCommittedVersionKeys.begin),
		                                                   Unversioned())] =
		    BinaryReader::fromStringRef<Version>(it.value, Unversioned());
	}

	state IDiskQueue::location minimumRecoveryLocation = 0;
	if (fRecoveryLocation.get().present()) {
		minimumRecoveryLocation =
		    BinaryReader::fromStringRef<IDiskQueue::location>(fRecoveryLocation.get().get(), Unversioned());
	}

	state std::map<UID, std::map<StorageTeamID, std::vector<Tag>>> storageTeams;
	for (const auto& it : fStorageTeams.get()) {
		storageTeams[BinaryReader::fromStringRef<UID>(it.key.removePrefix(persistStorageTeamKeys.begin),
		                                              IncludeVersion(ProtocolVersion::withPartitionTransaction()))] =
		    BinaryReader::fromStringRef<std::map<StorageTeamID, std::vector<Tag>>>(
		        it.value, IncludeVersion(ProtocolVersion::withPartitionTransaction()));
	}

	state int idx = 0;
	state std::vector<std::pair<Version, UID>> logsByVersion;
	serverData->removed = Never();

	for (idx = 0; idx < fVers.get().size(); idx++) {
		// persistCurrentVersionKeys is a prefix of recruitment id, and each recruitment can have only one version
		// thus we need to create a new TLogInterface for each round, it is for each recruitment.
		state KeyRef rawId =
		    fVers.get()[idx].key.removePrefix(persistCurrentVersionKeys.begin); // get interface.id for each generation
		UID id1 = BinaryReader::fromStringRef<UID>(rawId, Unversioned());
		UID id2 = BinaryReader::fromStringRef<UID>(
		    fRecoverCounts.get()[idx].key.removePrefix(persistRecoveryCountKeys.begin), Unversioned());
		ASSERT(id1 == id2);

		ptxn::TLogInterface_PassivelyPull recruited;
		if (serverData->id_interf.find(id1) == serverData->id_interf.end()) {
			recruited = ptxn::TLogInterface_PassivelyPull(id1, serverData->dbgid, locality);
			recruited.initEndpoints();
			serverData->id_interf[id1] = recruited;
			// DUMPTOKEN(recruited.peekMessages);
			// DUMPTOKEN(recruited.popMessages);
			DUMPTOKEN(recruited.commit);
			DUMPTOKEN(recruited.lock);
			DUMPTOKEN(recruited.getQueuingMetrics);
			DUMPTOKEN(recruited.confirmRunning);
			DUMPTOKEN(recruited.waitFailure);
			DUMPTOKEN(recruited.recoveryFinished);
			DUMPTOKEN(recruited.disablePopRequest);
			DUMPTOKEN(recruited.enablePopRequest);
			DUMPTOKEN(recruited.snapRequest);
		} else {
			recruited = serverData->id_interf[id1];
		}

		ProtocolVersion protocolVersion =
		    BinaryReader::fromStringRef<ProtocolVersion>(fProtocolVersions.get()[idx].value, Unversioned());
		TLogSpillType logSpillType = BinaryReader::fromStringRef<TLogSpillType>(fTLogSpillTypes.get()[idx].value,
		                                                                        AssumeVersion(protocolVersion));

		logData = makeReference<LogGenerationData>(self,
		                                           recruited,
		                                           UID(),
		                                           protocolVersion,
		                                           logSpillType,
		                                           storageTeams[id1],
		                                           0, // TODO: find whether/why we need this parameter
		                                           "Restored");
		logData->locality = id_locality[id1];
		logData->stopped = true;
		self->id_data[id1] = logData;

		logData->knownCommittedVersion = id_knownCommitted[id1];
		Version ver = BinaryReader::fromStringRef<Version>(fVers.get()[idx].value, Unversioned());
		logData->persistentDataVersion = ver;
		logData->persistentDataDurableVersion = ver;
		logData->version.set(ver);
		logData->recoveryCount =
		    BinaryReader::fromStringRef<DBRecoveryCount>(fRecoverCounts.get()[idx].value, Unversioned());

		// for multiple groups with same recruitment id, here it sends the same request to master multiple times.
		// it works fine now, will change if necessary.
		logData->removed = rejoinMasters(
		    serverData, recruited, logData->recoveryCount, serverData->registerWithMasters[id1].getFuture(), false);
		removed.push_back(errorOr(logData->removed));
		logsByVersion.emplace_back(ver, id1);

		TraceEvent("TLogPersistentStateRestore", self->dbgid)
		    .detail("LogId", logData->logId)
		    .detail("Ver", ver)
		    .detail("RecoveryCount", logData->recoveryCount);
		// Restore popped keys.  Pop operations that took place after the last (committed) updatePersistentDataVersion
		// might be lost, but that is fine because we will get the corresponding data back, too.
		tagKeys = prefixRange(rawId.withPrefix(persistTagPoppedKeys.begin));
		loop {
			if (logData->removed.isReady())
				break;
			RangeResult data = wait(self->persistentData->readRange(tagKeys, BUGGIFY ? 3 : 1 << 30, 1 << 20));
			if (!data.size())
				break;
			((KeyRangeRef&)tagKeys) = KeyRangeRef(keyAfter(data.back().key, tagKeys.arena()), tagKeys.end);

			for (auto& kv : data) {
				StorageTeamID id = decodeStorageTeamIDPoppedKey(kv.key);
				std::pair<std::vector<Tag>, Version> v = decodePairValue(kv.value);
				std::vector<Tag> tags = v.first;
				Version popped = v.second;
				TraceEvent("TLogRestorePopped", logData->logId).detail("StorageTeamID", id).detail("To", popped);

				auto storageTeamData = logData->getStorageTeamData(id);
				ASSERT(!storageTeamData);
				logData->createStorageTeamData(id, tags, popped);
				logData->getStorageTeamData(id)->persistentPopped = popped;
			}
		}
	}

	std::sort(logsByVersion.begin(), logsByVersion.end());
	for (const auto& pair : logsByVersion) {
		// TLogs that have been fully spilled won't have queue entries read in the loop below.
		self->popOrder.push_back(pair.second);
	}
	logsByVersion.clear();

	state Future<Void> allRemoved = waitForAll(removed);
	state UID lastId = UID(1, 1); // initialized so it will not compare equal to a default UID
	state double recoverMemoryLimit = SERVER_KNOBS->TLOG_RECOVER_MEMORY_LIMIT;
	if (BUGGIFY)
		recoverMemoryLimit =
		    std::max<double>(SERVER_KNOBS->BUGGIFY_RECOVER_MEMORY_LIMIT, (double)SERVER_KNOBS->TLOG_SPILL_THRESHOLD);

	try {
		bool recoveryFinished = wait(self->persistentQueue->initializeRecovery(minimumRecoveryLocation));
		if (recoveryFinished) {
			throw end_of_stream();
		}
		loop {
			if (allRemoved.isReady()) {
				TEST(true); // all tlogs removed during queue recovery
				throw worker_removed();
			}
			choose {
				when(TLogQueueEntry qe = wait(self->persistentQueue->readNext(self.getPtr()))) {
					if (qe.id != lastId) {
						lastId = qe.id;
						auto it = self->id_data.find(qe.id);
						if (it != self->id_data.end()) {
							logData = it->second;
						} else {
							logData = Reference<LogGenerationData>();
						}
					}

					//TraceEvent("TLogRecoveredQE", self->dbgid).detail("LogId", qe.id).detail("Ver", qe.version).detail("MessageBytes", qe.messages.size()).detail("Tags", qe.tags.size())
					//	.detail("Tag0", qe.tags.size() ? qe.tags[0].tag : invalidTag).detail("Version",
					// logData->version.get());

					if (logData) {
						if (!self->spillOrder.size() || self->spillOrder.back() != qe.id) {
							self->spillOrder.push_back(qe.id);
						}
						logData->knownCommittedVersion =
						    std::max(logData->knownCommittedVersion, qe.knownCommittedVersion);
						if (qe.version > logData->version.get()) {
							for (int i = 0; i < qe.messages.size(); i++) {
								commitMessages(self, logData, qe.version, qe.messages[i], qe.storageTeams[i]);
							}
							logData->version.set(qe.version);
							logData->queueCommittedVersion.set(qe.version);

							while (self->bytesInput - self->bytesDurable >= recoverMemoryLimit) {
								TEST(true); // Flush excess data during TLog queue recovery
								TraceEvent("FlushLargeQueueDuringRecovery", self->dbgid)
								    .detail("LogId", logData->logId)
								    .detail("BytesInput", self->bytesInput)
								    .detail("BytesDurable", self->bytesDurable)
								    .detail("Version", logData->version.get())
								    .detail("PVer", logData->persistentDataVersion);

								choose {
									when(wait(updateStorage(self))) {}
									when(wait(allRemoved)) { throw worker_removed(); }
								}
							}
						} else {
							// Updating persistRecoveryLocation and persistCurrentVersion at the same time,
							// transactionally, should mean that we never read any TLogQueueEntry that has already
							// been spilled.
							ASSERT_WE_THINK(qe.version == logData->version.get());
						}
					}
				}
				when(wait(allRemoved)) { throw worker_removed(); }
			}
		}
	} catch (Error& e) {
		if (e.code() != error_code_end_of_stream)
			throw;
	}

	TraceEvent("TLogRestorePersistentStateDone", self->dbgid).detail("Took", now() - startt);
	TEST(now() - startt >= 1.0); // TLog recovery took more than 1 second

	return Void();
}

ACTOR Future<Void> tlogGroupStart(Reference<TLogGroupData> self, Reference<LogGenerationData> logData) {
	try {
		if (logData->removed.isReady()) {
			throw logData->removed.getError();
		}

		// Brand new tlog, initialization has already been done by caller
		wait(initPersistentState(self, logData) || logData->removed);

		if (logData->recoveryComplete.isSet()) {
			throw worker_removed();
		}

		logData->initialized = true;
		self->newLogData.trigger();

		logData->recoveryComplete.send(Void());

		wait(logData->committingQueue.getFuture() || logData->removed);

		TraceEvent("TLogGroupReady", logData->logId)
		    .detail("GroupId", self->tlogGroupID)
		    .detail("Locality", logData->locality);
	} catch (Error& e) {

		if (e.code() != error_code_worker_removed) {
			throw;
		}

		wait(delay(0.0)); // if multiple recruitment requests were already in the promise stream make sure they are all
		// started before any are removed
		removeLog(logData);
	}
	return Void();
}

// Start the tLog role for a worker
ACTOR Future<Void> tLogStart(Reference<TLogServerData> self, InitializePtxnTLogRequest req, LocalityData locality) {
	ASSERT(req.isPrimary);
	// we start the new tlog server
	state TLogInterface_PassivelyPull recruited(self->dbgid, locality);
	recruited.initEndpoints();

	DUMPTOKEN(recruited.commit);
	DUMPTOKEN(recruited.lock);
	DUMPTOKEN(recruited.getQueuingMetrics);
	DUMPTOKEN(recruited.confirmRunning);
	DUMPTOKEN(recruited.waitFailure);
	DUMPTOKEN(recruited.recoveryFinished);
	DUMPTOKEN(recruited.snapRequest);

	DUMPTOKEN(recruited.disablePopRequest);
	DUMPTOKEN(recruited.enablePopRequest);

	stopAllTLogs(self, recruited.id());
	self->removed = rejoinMasters(self, recruited, req.epoch, Future<Void>(Void()), req.isPrimary);

	state std::vector<Future<Void>> tlogGroupStarts;
	state std::shared_ptr<std::unordered_map<TLogGroupID, Reference<LogGenerationData>>> activeGeneration =
	    std::make_shared<std::unordered_map<TLogGroupID, Reference<LogGenerationData>>>();
	for (auto& group : req.tlogGroups) {
		ASSERT(self->tlogGroups.count(group.logGroupId));
		Reference<TLogGroupData> tlogGroupData = self->tlogGroups[group.logGroupId];
		ASSERT(group.logGroupId == tlogGroupData->tlogGroupID);
		Reference<LogGenerationData> newGenerationData = makeReference<LogGenerationData>(tlogGroupData,
		                                                                                  recruited,
		                                                                                  req.recruitmentID,
		                                                                                  g_network->protocolVersion(),
		                                                                                  req.spillType,
		                                                                                  group.storageTeams,
		                                                                                  req.locality,
		                                                                                  "Recruited");
		// groups belong to the same interface(implying they have the same generation) share the same key(i.e.
		// interface.id) it will be persisted in each group, during recovery we will aggregate it by interface.id and
		// re-build the interface who serves many groups.
		tlogGroupData->id_data[recruited.id()] = newGenerationData;
		newGenerationData->removed = self->removed;
		activeGeneration->emplace(group.logGroupId, newGenerationData);
		tlogGroupStarts.push_back(tlogGroupStart(tlogGroupData, newGenerationData));
		tlogGroupData->spillOrder.push_back(recruited.id());
		tlogGroupData->popOrder.push_back(recruited.id());
	}

	wait(waitForAll(tlogGroupStarts));

	req.reply.send(recruited);

	TraceEvent("TLogStart", recruited.id());
	wait(tLogCore(self, activeGeneration, recruited, recruited.id()));
	return Void();
}

ACTOR Future<Void> tLogPop(Reference<TLogGroupData> self, TLogPopRequest req, Reference<LogGenerationData> logData) {
	if (self->ignorePopRequest) {
		TraceEvent(SevDebug, "IgnoringPopRequest").detail("IgnorePopDeadline", self->ignorePopDeadline);

		auto& v = self->toBePopped[req.tag];
		v = std::max(v, req.version);

		TraceEvent(SevDebug, "IgnoringPopRequest")
		    .detail("IgnorePopDeadline", self->ignorePopDeadline)
		    .detail("Tag", req.tag)
		    .detail("Version", req.version);
	} else {
		// TODO: pop from tlog
		wait(Future<Void>(Void()));
		// wait(tLogPopCore(self, req.tag, req.version, logData));
	}
	req.reply.send(Void());
	return Void();
}

void updatePersistentPopped(Reference<TLogGroupData> self,
                            Reference<LogGenerationData> logData,
                            Reference<LogGenerationData::StorageTeamData> data) {
	if (!data->poppedRecently)
		return;
	self->persistentData->set(KeyValueRef(persistStorageTeamPoppedKey(logData->logId, data->storageTeamId),
	                                      persistStorageTeamPoppedValue(data->popped)));
	data->poppedRecently = false;
	data->persistentPopped = data->popped;

	if (data->nothingPersistent)
		return;

	if (logData->shouldSpillByValue(data->storageTeamId)) {
		self->persistentData->clear(
		    KeyRangeRef(persistStorageTeamMessagesKey(logData->logId, data->storageTeamId, Version(0)),
		                persistStorageTeamMessagesKey(logData->logId, data->storageTeamId, data->popped)));
	} else {
		self->persistentData->clear(
		    KeyRangeRef(persistStorageTeamMessageRefsKey(logData->logId, data->storageTeamId, Version(0)),
		                persistStorageTeamMessageRefsKey(logData->logId, data->storageTeamId, data->popped)));
	}

	if (data->popped > logData->persistentDataVersion) {
		data->nothingPersistent = true;
	}
}

ACTOR Future<Void> updatePersistentData(Reference<TLogGroupData> self,
                                        Reference<LogGenerationData> logData,
                                        Version newPersistentDataVersion) {
	state BinaryWriter wr(Unversioned());

	// PERSIST: Changes self->persistentDataVersion and writes and commits the relevant changes
	ASSERT(newPersistentDataVersion <= logData->version.get());
	ASSERT(newPersistentDataVersion <= logData->queueCommittedVersion.get());
	ASSERT(newPersistentDataVersion > logData->persistentDataVersion);
	ASSERT(logData->persistentDataVersion == logData->persistentDataDurableVersion);
	logData->newPersistentDataVersion = newPersistentDataVersion;

	//TraceEvent("UpdatePersistentData", self->dbgid).detail("Seq", newPersistentDataSeq);

	state bool anyData = false;

	// For all existing tags
	state int tagLocality = 0;
	state std::unordered_map<StorageTeamID, Reference<LogGenerationData::StorageTeamData>>::iterator it;
	for (it = logData->storageTeamData.begin(); it != logData->storageTeamData.end(); it++) {
		// iterate through all storage teams and try to update persistent data
		state Reference<LogGenerationData::StorageTeamData> teamData = it->second;
		if (teamData) {
			// TODO: implement eraseMessagesBefore for StorageTeamData
			// wait(teamData->eraseMessagesBefore(teamData->popped, self, logData, TaskPriority::UpdateStorage));
			state Version currentVersion = 0;
			// Clear recently popped versions from persistentData if necessary
			updatePersistentPopped(self, logData, teamData);
			state Version lastVersion = std::numeric_limits<Version>::min();
			state IDiskQueue::location firstLocation = std::numeric_limits<IDiskQueue::location>::max();
			// Transfer unpopped messages with version numbers less than newPersistentDataVersion to persistentData
			// TOFIX: versions in logData->versionLocation is erased through persistentQueue->forgetBefore,
			// however we do not erase it in teamData yet, that alone needs a PR.
			state std::map<Version, std::pair<StringRef, Arena>>::iterator msg =
			    teamData->versionMessages.lower_bound(logData->versionLocation.begin()->key);
			state int refSpilledTagCount = 0;
			wr = BinaryWriter(AssumeVersion(logData->protocolVersion));
			// We prefix our spilled locations with a count, so that we can read this back out as a VectorRef.
			wr << uint32_t(0);
			while (msg != teamData->versionMessages.end() && msg->first <= newPersistentDataVersion) {
				currentVersion = msg->first;
				anyData = true;
				teamData->nothingPersistent = false;

				if (logData->shouldSpillByValue(teamData->storageTeamId)) {
					wr = BinaryWriter(Unversioned());
					// write real data here as the value to be persisted.
					for (; msg != teamData->versionMessages.end() && msg->first == currentVersion; ++msg) {
						wr << msg->second.first; // question: do we need arena here?
					}

					self->persistentData->set(KeyValueRef(
					    persistStorageTeamMessagesKey(logData->logId, teamData->storageTeamId, currentVersion),
					    wr.toValue()));
				} else {
					// spill everything else by reference
					const IDiskQueue::location begin = logData->versionLocation[currentVersion].first;
					const IDiskQueue::location end = logData->versionLocation[currentVersion].second;
					ASSERT(end > begin && end.lo - begin.lo < std::numeric_limits<uint32_t>::max());
					uint32_t length = static_cast<uint32_t>(end.lo - begin.lo);
					refSpilledTagCount++;

					uint32_t size = 0;
					for (; msg != teamData->versionMessages.end() && msg->first == currentVersion; ++msg) {
						// Fast forward until we find a new version.
						// TOFIX: how to calculate the size of stringref?
						// size += msg->second->first.expectedSize();
						size += 0;
					}

					SpilledData spilledData(currentVersion, begin, length, size);
					wr << spilledData;

					lastVersion = std::max(currentVersion, lastVersion);
					firstLocation = std::min(begin, firstLocation);

					if ((wr.getLength() + sizeof(SpilledData) >
					     SERVER_KNOBS->TLOG_SPILL_REFERENCE_MAX_BYTES_PER_BATCH)) {
						*(uint32_t*)wr.getData() = refSpilledTagCount;
						self->persistentData->set(KeyValueRef(
						    persistStorageTeamMessageRefsKey(logData->logId, teamData->storageTeamId, lastVersion),
						    wr.toValue()));
						teamData->poppedLocation = std::min(teamData->poppedLocation, firstLocation);
						refSpilledTagCount = 0;
						wr = BinaryWriter(AssumeVersion(logData->protocolVersion));
						wr << uint32_t(0);
					}

					Future<Void> f = yield(TaskPriority::UpdateStorage);
					if (!f.isReady()) {
						wait(f);
						msg = teamData->versionMessages.upper_bound(currentVersion);
					}
				}
			}
			if (refSpilledTagCount > 0) {
				*(uint32_t*)wr.getData() = refSpilledTagCount;
				self->persistentData->set(
				    KeyValueRef(persistStorageTeamMessageRefsKey(logData->logId, teamData->storageTeamId, lastVersion),
				                wr.toValue()));
				teamData->poppedLocation = std::min(teamData->poppedLocation, firstLocation);
			}

			wait(yield(TaskPriority::UpdateStorage));
		}
	}

	auto locationIter = logData->versionLocation.lower_bound(newPersistentDataVersion);
	if (locationIter != logData->versionLocation.end()) {
		self->persistentData->set(
		    KeyValueRef(persistRecoveryLocationKey, BinaryWriter::toValue(locationIter->value.first, Unversioned())));
	}
	// key : persistCurrentVersionKeys + interface.id
	// value : persistentDataVersion
	// for groups served by the same interface(implying they have the same generation), they should have the same key.
	self->persistentData->set(
	    KeyValueRef(BinaryWriter::toValue(logData->logId, Unversioned()).withPrefix(persistCurrentVersionKeys.begin),
	                BinaryWriter::toValue(newPersistentDataVersion, Unversioned())));
	self->persistentData->set(KeyValueRef(
	    BinaryWriter::toValue(logData->logId, Unversioned()).withPrefix(persistKnownCommittedVersionKeys.begin),
	    BinaryWriter::toValue(logData->knownCommittedVersion, Unversioned())));
	// TODO: update teams -> tags mapping when it changes!
	logData->persistentDataVersion = newPersistentDataVersion;

	wait(self->persistentData->commit()); // SOMEDAY: This seems to be running pretty often, should we slow it down???
	wait(delay(0, TaskPriority::UpdateStorage));

	// Now that the changes we made to persistentData are durable, erase the data we moved from memory and the queue,
	// increase bytesDurable accordingly, and update persistentDataDurableVersion.

	TEST(anyData); // TLog moved data to persistentData
	logData->persistentDataDurableVersion = newPersistentDataVersion;

	for (it = logData->storageTeamData.begin(); it != logData->storageTeamData.end(); it++) {
		if (it->second) {
			// uncomment this once we have StorageTeamData::eraseMessagesBefore
			// wait(it->second->eraseMessagesBefore(
			// 	newPersistentDataVersion + 1, self, logData, TaskPriority::UpdateStorage));
			wait(yield(TaskPriority::UpdateStorage));
		}
	}

	logData->version_sizes.erase(logData->version_sizes.begin(),
	                             logData->version_sizes.lower_bound(logData->persistentDataDurableVersion));

	wait(yield(TaskPriority::UpdateStorage));

	while (!logData->messageBlocks.empty() && logData->messageBlocks.front().first <= newPersistentDataVersion) {
		int64_t bytesErased =
		    int64_t(logData->messageBlocks.front().second.size()) * SERVER_KNOBS->TLOG_MESSAGE_BLOCK_OVERHEAD_FACTOR;
		logData->bytesDurable += bytesErased;
		self->bytesDurable += bytesErased;
		logData->messageBlocks.pop_front();
		wait(yield(TaskPriority::UpdateStorage));
	}

	if (logData->bytesDurable.getValue() > logData->bytesInput.getValue() || self->bytesDurable > self->bytesInput) {
		TraceEvent(SevError, "BytesDurableTooLarge", logData->logId)
		    .detail("SharedBytesInput", self->bytesInput)
		    .detail("SharedBytesDurable", self->bytesDurable)
		    .detail("LocalBytesInput", logData->bytesInput.getValue())
		    .detail("LocalBytesDurable", logData->bytesDurable.getValue());
	}

	ASSERT(logData->bytesDurable.getValue() <= logData->bytesInput.getValue());
	ASSERT(self->bytesDurable <= self->bytesInput);

	if (self->queueCommitEnd.get() > 0) {
		// FIXME: Maintain a heap of tags ordered by version to make this O(1) instead of O(n).
		Version minVersion = std::numeric_limits<Version>::max();
		for (it = logData->storageTeamData.begin(); it != logData->storageTeamData.end(); it++) {
			if (it->second) {
				if (logData->shouldSpillByValue(it->second->storageTeamId)) {
					minVersion = std::min(minVersion, newPersistentDataVersion);
				} else {
					minVersion = std::min(minVersion, it->second->popped);
				}
			}
		}
		if (minVersion != std::numeric_limits<Version>::max()) {
			self->persistentQueue->forgetBefore(
			    newPersistentDataVersion,
			    logData); // SOMEDAY: this can cause a slow task (~0.5ms), presumably from erasing too many versions.
			              // Should we limit the number of versions cleared at a time?
		}
	}
	logData->newPersistentDataVersion = invalidVersion;

	return Void();
}

// This function (and updatePersistentData, which is called by this function) run at a low priority and can soak up all
// CPU resources. For this reason, they employ aggressive use of yields to avoid causing slow tasks that could introduce
// latencies for more important work (e.g. commits).
// This actor is just a loop that calls updatePersistentData and popDiskQueue whenever
// (a) there's data to be spilled or (b) we should update metadata after some commits have been fully popped.
ACTOR Future<Void> updateStorage(Reference<TLogGroupData> self) {
	while (self->spillOrder.size() && !self->id_data.count(self->spillOrder.front())) {
		self->spillOrder.pop_front();
	}

	if (!self->spillOrder.size()) {
		wait(delay(BUGGIFY ? SERVER_KNOBS->BUGGIFY_TLOG_STORAGE_MIN_UPDATE_INTERVAL
		                   : SERVER_KNOBS->TLOG_STORAGE_MIN_UPDATE_INTERVAL,
		           TaskPriority::UpdateStorage));
		return Void();
	}

	state Reference<LogGenerationData> logData = self->id_data[self->spillOrder.front()];
	state Version nextVersion = 0;
	state int totalSize = 0;

	state FlowLock::Releaser commitLockReleaser;

	// FIXME: This policy for calculating the cache pop version could end up popping recent data in the remote DC after
	// two consecutive recoveries.
	// It also does not protect against spilling the cache tag directly, so it is theoretically possible to spill this
	// tag; which is not intended to ever happen.
	Optional<Version> cachePopVersion;
	for (auto& it : self->id_data) {
		if (!it.second->stopped) {
			if (it.second->version.get() - it.second->unrecoveredBefore >
			    SERVER_KNOBS->MAX_VERSIONS_IN_FLIGHT + SERVER_KNOBS->MAX_CACHE_VERSIONS) {
				cachePopVersion = it.second->version.get() - SERVER_KNOBS->MAX_CACHE_VERSIONS;
			}
			break;
		}
	}

	if (cachePopVersion.present()) {
		state std::vector<Future<Void>> cachePopFutures;
		for (auto& it : self->id_data) {
			// cacheTag is a special tag, not sure why we use it here in old path
			cachePopFutures.push_back(tLogPop(self, TLogPopRequest(cachePopVersion.get(), 0, cacheTag), it.second));
		}
		wait(waitForAll(cachePopFutures));
	}

	if (logData->stopped) {
		if (self->bytesInput - self->bytesDurable >= self->targetVolatileBytes) {
			while (logData->persistentDataDurableVersion != logData->version.get()) {
				totalSize = 0;
				Map<Version, std::pair<int, int>>::iterator sizeItr = logData->version_sizes.begin();
				nextVersion = logData->version.get();
				while (totalSize < SERVER_KNOBS->REFERENCE_SPILL_UPDATE_STORAGE_BYTE_LIMIT &&
				       sizeItr != logData->version_sizes.end()) {
					totalSize += sizeItr->value.first + sizeItr->value.second;
					++sizeItr;
					nextVersion = sizeItr == logData->version_sizes.end() ? logData->version.get() : sizeItr->key;
				}

				wait(logData->queueCommittedVersion.whenAtLeast(nextVersion));
				wait(delay(0, TaskPriority::UpdateStorage));

				//TraceEvent("TlogUpdatePersist", self->dbgid).detail("LogId", logData->logId).detail("NextVersion", nextVersion).detail("Version", logData->version.get()).detail("PersistentDataDurableVer", logData->persistentDataDurableVersion).detail("QueueCommitVer", logData->queueCommittedVersion.get()).detail("PersistDataVer", logData->persistentDataVersion);
				if (nextVersion > logData->persistentDataVersion) {
					wait(self->persistentDataCommitLock.take());
					commitLockReleaser = FlowLock::Releaser(self->persistentDataCommitLock);
					wait(updatePersistentData(self, logData, nextVersion));
					// Concurrently with this loop, the last stopped TLog could have been removed.
					if (self->popOrder.size()) {
						// TODO: add popDiskQueue()
						// wait(popDiskQueue(self, self->id_data[self->popOrder.front()]));
					}
					commitLockReleaser.release();
				} else {
					wait(delay(BUGGIFY ? SERVER_KNOBS->BUGGIFY_TLOG_STORAGE_MIN_UPDATE_INTERVAL
					                   : SERVER_KNOBS->TLOG_STORAGE_MIN_UPDATE_INTERVAL,
					           TaskPriority::UpdateStorage));
				}

				if (logData->removed.isReady()) {
					break;
				}
			}

			if (logData->persistentDataDurableVersion == logData->version.get()) {
				self->spillOrder.pop_front();
			}
			wait(delay(0.0, TaskPriority::UpdateStorage));
		} else {
			wait(delay(BUGGIFY ? SERVER_KNOBS->BUGGIFY_TLOG_STORAGE_MIN_UPDATE_INTERVAL
			                   : SERVER_KNOBS->TLOG_STORAGE_MIN_UPDATE_INTERVAL,
			           TaskPriority::UpdateStorage));
		}
	} else if (logData->initialized) {
		ASSERT(self->spillOrder.size() == 1);
		if (logData->version_sizes.empty()) {
			nextVersion = logData->version.get();
		} else {
			// Double check that a running TLog wasn't wrongly affected by spilling locked SharedTLogs.
			ASSERT_WE_THINK(self->targetVolatileBytes == SERVER_KNOBS->TLOG_SPILL_THRESHOLD);
			Map<Version, std::pair<int, int>>::iterator sizeItr = logData->version_sizes.begin();
			while (totalSize < SERVER_KNOBS->REFERENCE_SPILL_UPDATE_STORAGE_BYTE_LIMIT &&
			       sizeItr != logData->version_sizes.end() &&
			       (logData->bytesInput.getValue() - logData->bytesDurable.getValue() - totalSize >=
			            self->targetVolatileBytes ||
			        sizeItr->value.first == 0)) {
				totalSize += sizeItr->value.first + sizeItr->value.second;
				++sizeItr;
				nextVersion = sizeItr == logData->version_sizes.end() ? logData->version.get() : sizeItr->key;
			}
		}

		//TraceEvent("UpdateStorageVer", logData->logId).detail("NextVersion", nextVersion).detail("PersistentDataVersion", logData->persistentDataVersion).detail("TotalSize", totalSize);

		wait(logData->queueCommittedVersion.whenAtLeast(nextVersion));
		wait(delay(0, TaskPriority::UpdateStorage));

		if (nextVersion > logData->persistentDataVersion) {
			wait(self->persistentDataCommitLock.take());
			commitLockReleaser = FlowLock::Releaser(self->persistentDataCommitLock);
			wait(updatePersistentData(self, logData, nextVersion));
			if (self->popOrder.size()) {
				// TODO: add popDiskQueue()
				// wait(popDiskQueue(self, self->id_data[self->popOrder.front()]));
			}
			commitLockReleaser.release();
		}

		if (totalSize < SERVER_KNOBS->REFERENCE_SPILL_UPDATE_STORAGE_BYTE_LIMIT) {
			wait(delay(BUGGIFY ? SERVER_KNOBS->BUGGIFY_TLOG_STORAGE_MIN_UPDATE_INTERVAL
			                   : SERVER_KNOBS->TLOG_STORAGE_MIN_UPDATE_INTERVAL,
			           TaskPriority::UpdateStorage));
		} else {
			// recovery wants to commit to persistant data when updatePersistentData is not active, this delay ensures
			// that immediately after updatePersist returns another one has not been started yet.
			wait(delay(0.0, TaskPriority::UpdateStorage));
		}
	} else {
		wait(delay(BUGGIFY ? SERVER_KNOBS->BUGGIFY_TLOG_STORAGE_MIN_UPDATE_INTERVAL
		                   : SERVER_KNOBS->TLOG_STORAGE_MIN_UPDATE_INTERVAL,
		           TaskPriority::UpdateStorage));
	}
	return Void();
}

ACTOR Future<Void> updateStorageLoop(Reference<TLogGroupData> self) {
	wait(delay(0, TaskPriority::UpdateStorage));

	loop { wait(updateStorage(self)); }
}

ACTOR Future<Void> tLog(
    std::unordered_map<ptxn::TLogGroupID, std::pair<IKeyValueStore*, IDiskQueue*>> oldPersistentDataAndQueues,
    Reference<AsyncVar<ServerDBInfo>> db,
    LocalityData locality,
    PromiseStream<InitializePtxnTLogRequest> tlogRequests,
    UID tlogId,
    UID workerID,
    bool restoreFromDisk,
    Promise<Void> recovered,
    Promise<Void> oldLog,
    std::string folder,
    Reference<AsyncVar<bool>> degraded,
    Reference<AsyncVar<UID>> activeSharedTLog) {

	// TODO: persist old tlog group metadata in tlog disk and read it from here, rather than when receive request.
	state Reference<TLogServerData> self = makeReference<TLogServerData>(tlogId, workerID, db, degraded, folder);
	state Future<Void> error = actorCollection(self->sharedActors.getFuture());

	TraceEvent("SharedTlog", tlogId);

	try {
		state Future<Void> activeSharedChange = Void();
		state std::vector<Future<Void>> tlogGroupTerminated = { Never() };
		state std::vector<Future<Void>> tlogGroupRecoveries;

		for (auto& [id, p] : oldPersistentDataAndQueues) {
			// old log groups must be recovered by restored from persistent state from disk.
			// each group might have multiple generations
			Reference<TLogGroupData> tlogGroup =
			    makeReference<TLogGroupData>(tlogId, id, workerID, p.first, p.second, db, degraded, folder, self);
			self->oldTLogGroups[id] = tlogGroup; // Reference, so that restorePersistentState should change this var
			tlogGroupRecoveries.push_back(restorePersistentState(tlogGroup, locality, self));
		}

		loop choose {
			// TODO: build overlapping tlog groups from disk
			when(state InitializePtxnTLogRequest req = waitNext(tlogRequests.getFuture())) {
				if (!self->tlogCache.exists(req.recruitmentID)) {
					self->tlogCache.set(req.recruitmentID, req.reply.getFuture());
					std::vector<Future<Void>> tlogGroupRecoveries;
					for (auto& group : req.tlogGroups) {
						// memory managed by each tlog group
						IKeyValueStore* persistentData = req.persistentDataAndQueues[group.logGroupId].first;
						IDiskQueue* persistentQueue = req.persistentDataAndQueues[group.logGroupId].second;
						Reference<TLogGroupData> tlogGroup = makeReference<TLogGroupData>(tlogId,
						                                                                  group.logGroupId,
						                                                                  workerID,
						                                                                  persistentData,
						                                                                  persistentQueue,
						                                                                  db,
						                                                                  degraded,
						                                                                  folder,
						                                                                  self);
						TraceEvent("SharedTlogGroup").detail("LogId", tlogId).detail("GroupID", group.logGroupId);
						self->tlogGroups[group.logGroupId] = tlogGroup;
						tlogGroupRecoveries.push_back(
						    ioTimeoutError(checkEmptyQueue(tlogGroup) && checkRecovered(tlogGroup),
						                   SERVER_KNOBS->TLOG_MAX_CREATE_DURATION));
						tlogGroupTerminated.push_back(tlogGroup->terminated.getFuture());
					}
					choose {
						when(wait(waitForAny(tlogGroupTerminated))) { throw tlog_stopped(); }
						when(wait(waitForAll(tlogGroupRecoveries))) {}
					}

					if (restoreFromDisk) {
						// restore information for each (generation, group), aggregated by generation, then group.
						// then cal tLogCore() for each generation.
						std::unordered_map<
						    UID,
						    std::shared_ptr<std::unordered_map<TLogGroupID, Reference<LogGenerationData>>>>
						    generations;
						for (auto& [_, group] : self->oldTLogGroups) {
							for (auto it : group->id_data) {
								if (it.second->queueCommittedVersion.get() == 0) {
									TraceEvent("TLogZeroVersion", group->dbgid).detail("LogId", it.first);
									it.second->queueCommittedVersion.set(it.second->version.get());
								}
								it.second->recoveryComplete.sendError(end_of_stream());

								if (generations.find(it.first) == generations.end()) {
									generations[it.first] = std::make_shared<
									    std::unordered_map<TLogGroupID, Reference<LogGenerationData>>>();
								}
								(*generations[it.first])[group->tlogGroupID] = it.second;
							}
						}
						for (auto& [id, generation] : generations) {
							self->sharedActors.send(tLogCore(self, generation, self->id_interf[id], id));
						}
						for (auto& [id, registerWithMaster] : self->registerWithMasters) {
							if (registerWithMaster.canBeSet())
								registerWithMaster.send(Void());
						}
					}

					// Disk errors need a chance to kill this actor.
					wait(delay(0.000001));

					for (auto& [_, tlogGroup] : self->tlogGroups) {
						tlogGroup->sharedActors.send(commitQueue(tlogGroup));
						tlogGroup->sharedActors.send(updateStorageLoop(tlogGroup));
					}

					// start the new generation
					self->sharedActors.send(tLogStart(self, req, locality));
				} else {
					forwardPromise(req.reply, self->tlogCache.get(req.recruitmentID));
				}
			}
			when(wait(error)) { throw internal_error(); }
			when(wait(activeSharedChange)) {
				if (activeSharedTLog->get() == tlogId) {
					TraceEvent("SharedTLogNowActive", self->dbgid).detail("NowActive", activeSharedTLog->get());
					self->targetVolatileBytes = SERVER_KNOBS->TLOG_SPILL_THRESHOLD;
				} else {
					stopAllTLogs(self, tlogId);
				}
				activeSharedChange = activeSharedTLog->onChange();
			}
		}
	} catch (Error& e) {
		self->terminated.send(Void());
		TraceEvent("TLogError", tlogId).error(e, true);
		if (recovered.canBeSet())
			recovered.send(Void());

		while (!tlogRequests.isEmpty()) {
			tlogRequests.getFuture().pop().reply.sendError(recruitment_failed());
		}

		for (auto& [_, group] : self->tlogGroups) {
			for (auto& [_, generationData] : group->id_data) {
				if (!generationData->recoveryComplete.isSet()) {
					generationData->recoveryComplete.sendError(end_of_stream());
				}
			}
		}

		for (auto& [_, group] : self->tlogGroups) {
			if (!tlogTerminated(group, group->persistentData, group->persistentQueue, e)) {
				throw;
			}
		}
		return Void();
	}
}

} // namespace ptxn
