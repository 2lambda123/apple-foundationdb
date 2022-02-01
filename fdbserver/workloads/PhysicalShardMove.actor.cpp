/*
 *PhysicalShardMove.actor.cpp
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

#include "fdbclient/ManagementAPI.actor.h"
#include "fdbclient/NativeAPI.actor.h"
#include "fdbrpc/simulator.h"
#include "fdbserver/IKeyValueStore.h"
#include "fdbserver/MoveKeys.actor.h"
#include "fdbserver/QuietDatabase.h"
#include "fdbserver/workloads/workloads.actor.h"
#include "flow/Error.h"
#include "flow/IRandom.h"
#include "flow/flow.h"
#include <cstdint>
#include <limits>
#include <rocksdb/options.h>
#include <rocksdb/sst_file_reader.h>

#include "flow/actorcompiler.h" // This must be the last #include.

namespace {
std::string printValue(const ErrorOr<Optional<Value>>& value) {
	if (value.isError()) {
		return value.getError().name();
	}
	return value.get().present() ? value.get().get().toString() : "Value Not Found.";
}
} // namespace

struct SSCheckpointWorkload : TestWorkload {
	const bool enabled;
	bool pass;

	SSCheckpointWorkload(WorkloadContext const& wcx) : TestWorkload(wcx), enabled(!clientId), pass(true) {}

	void validationFailed(ErrorOr<Optional<Value>> expectedValue, ErrorOr<Optional<Value>> actualValue) {
		TraceEvent(SevError, "TestFailed")
		    .detail("ExpectedValue", printValue(expectedValue))
		    .detail("ActualValue", printValue(actualValue));
		pass = false;
	}

	std::string description() const override { return "SSCheckpoint"; }

	Future<Void> setup(Database const& cx) override { return Void(); }

	Future<Void> start(Database const& cx) override {
		if (!enabled) {
			return Void();
		}
		return _start(this, cx);
	}

	ACTOR Future<Void> _start(SSCheckpointWorkload* self, Database cx) {
		state Key key = "TestKey"_sr;
		state Key endKey = "TestKey0"_sr;
		state Value oldValue = "TestValue"_sr;
		state Value newValue = "TestNewValue"_sr;

		state Version version = wait(self->writeAndVerify(self, cx, key, oldValue));

		std::cout << "Initialized" << std::endl;

		// loop {
		// 	try {
		// 		std::cout << "Creating checkpoint." << std::endl;
		// 		state CheckpointMetaData checkpoint = wait(getCheckpoint(cx, normalKeys, version, RocksDBColumnFamily));
		// 		break;
		// 	} catch (Error& e) {
		// 		std::cout << "Creating checkpoint failure: " << e.name() << std::endl;
		// 		wait(delay(1));
		// 	}
		// }
		// std::cout << "Created checkpoint:" << checkpoint.toString() << std::endl;

		// state std::string pwd = platform::getWorkingDirectory();
		// state std::string folder = pwd + "/checkpoints";
		// platform::eraseDirectoryRecursive(folder);
		// ASSERT(platform::createDirectory(folder));

		// loop {
		// 	try {
		// 		std::cout << "Getting checkpoint." << std::endl;
		// 		state CheckpointMetaData record = wait(
		// 		    fetchCheckpoint(cx, KeyRangeRef(key, endKey), checkpoint.version, RocksDBColumnFamily, folder));
		// 		break;
		// 	} catch (Error& e) {
		// 		std::cout << "Getting checkpoint failure: " << e.name() << std::endl;
		// 		wait(delay(1));
		// 	}
		// }

		// std::cout << "Got checkpoint:" << checkpoint.toString() << std::endl;

		// std::vector<std::string> files = platform::listFiles(folder);
		// std::cout << "Received checkpoint files on disk: " << folder << std::endl;
		// for (auto& file : files) {
		// 	std::cout << file << std::endl;
		// }
		// std::cout << std::endl;

		// ASSERT(files.size() == record.rocksCF.get().sstFiles.size());
		// std::unordered_set<std::string> sstFiles(files.begin(), files.end());
		// // for (const LiveFileMetaData& metaData : record.sstFiles) {
		// // 	std::cout << "Checkpoint file:" << metaData.db_path << metaData.name << std::endl;
		// // 	// ASSERT(sstFiles.count(metaData.name.subString) > 0);
		// // }

		// rocksdb::Options options;
		// rocksdb::ReadOptions ropts;
		// state std::unordered_map<Key, Value> kvs;
		// for (auto& file : files) {
		// 	rocksdb::SstFileReader reader(options);
		// 	std::cout << file << std::endl;
		// 	ASSERT(reader.Open(folder + "/" + file).ok());
		// 	ASSERT(reader.VerifyChecksum().ok());
		// 	std::unique_ptr<rocksdb::Iterator> iter(reader.NewIterator(ropts));
		// 	iter->SeekToFirst();
		// 	while (iter->Valid()) {
		// 		if (normalKeys.contains(Key(iter->key().ToString()))) {
		// 			std::cout << "Key: " << iter->key().ToString() << ", Value: " << iter->value().ToString()
		// 			          << std::endl;
		// 		}
		// 		// std::endl; writer.Put(iter->key().ToString(), iter->value().ToString());
		// 		// kvs[Key(iter->key().ToString())] = Value(iter->value().ToString());
		// 		iter->Next();
		// 	}
		// }

		// state std::string rocksDBTestDir = "rocksdb-kvstore-test-db";
		// platform::eraseDirectoryRecursive(rocksDBTestDir);

		// state IKeyValueStore* kvStore = keyValueStoreRocksDB(
		//     rocksDBTestDir, deterministicRandom()->randomUniqueID(), KeyValueStoreType::SSD_ROCKSDB_V1);
		// try {
		// 	wait(kvStore->restore(record));
		// } catch (Error& e) {
		// 	std::cout << e.name() << std::endl;
		// }

		// std::cout << "Restore complete" << std::endl;

		// state Transaction tr(cx);
		// tr.setOption(FDBTransactionOptions::LOCK_AWARE);
		// loop {
		// 	try {
		// 		state RangeResult res = wait(tr.getRange(KeyRangeRef(key, endKey), CLIENT_KNOBS->TOO_MANY));
		// 		break;
		// 	} catch (Error& e) {
		// 		wait(tr.onError(e));
		// 	}
		// }

		state Transaction tr(cx);
		tr.setOption(FDBTransactionOptions::LOCK_AWARE);
		tr.setOption(FDBTransactionOptions::ACCESS_SYSTEM_KEYS);
		loop {
			std::cout << "Commit checkpoint key." << std::endl;
			try {
				// state RangeResult serverList = wait(tr->getRange(serverListKeys, CLIENT_KNOBS->TOO_MANY));
				// ASSERT(!serverList.more && serverList.size() < CLIENT_KNOBS->TOO_MANY);

				// const int idx = deterministicRandom()->randomInt(0, serverList.size());
				// UID ssID = decodeServerListKey(serverList[idx].key);
				// CheckpointMetaData checkpoint;
				wait(createCheckpoint(&tr, KeyRangeRef(key, endKey), RocksDBColumnFamily));
				// tr.set(checkpointKeyFor(checkpoint.ssID), checkpointValue(checkpoint), .getPtr());
				std::cout << "Buffer write done." << std::endl;
				wait(tr.commit());
				break;
			} catch (Error& e) {
				wait(tr.onError(e));
			}
		}
		// state int i = 0;

		// for (i = 0; i < res.size(); ++i) {
		// 	std::cout << "Reading key:" << res[i].key.toString() << std::endl;
		// 	Optional<Value> value = wait(kvStore->readValue(res[i].key));
		// 	ASSERT(value.present());
		// 	ASSERT(value.get() == res[i].value);
		// }

		// std::cout << "Done print." << std::endl;

		// state std::unordered_map<Key, Value>::iterator it = kvs.begin();
		// for (; it != kvs.end(); ++it) {
		// 	if (normalKeys.contains(it->first)) {
		// 		std::cout << "Key: " << it->first.toString() << ", Value: " << it->second.toString() << std::endl;
		// 		ErrorOr<Optional<Value>> value(Optional<Value>(it->second));
		// 		wait(self->readAndVerify(self, cx, it->first, value));
		// 	}
		// }

		return Void();
	}

	ACTOR Future<Void> readAndVerify(SSCheckpointWorkload* self,
	                                 Database cx,
	                                 Key key,
	                                 ErrorOr<Optional<Value>> expectedValue) {
		state Transaction tr(cx);
		tr.setOption(FDBTransactionOptions::ACCESS_SYSTEM_KEYS);

		loop {
			try {
				state Optional<Value> res = wait(timeoutError(tr.get(key), 30.0));
				const bool equal = !expectedValue.isError() && res == expectedValue.get();
				if (!equal) {
					self->validationFailed(expectedValue, ErrorOr<Optional<Value>>(res));
				}
				break;
			} catch (Error& e) {
				if (expectedValue.isError() && expectedValue.getError().code() == e.code()) {
					break;
				}
				wait(tr.onError(e));
			}
		}

		return Void();
	}

	ACTOR Future<Version> writeAndVerify(SSCheckpointWorkload* self, Database cx, Key key, Optional<Value> value) {
		state Transaction tr(cx);
		state Version version;
		loop {
			try {
				if (value.present()) {
					tr.set(key, value.get());
				} else {
					tr.clear(key);
				}
				wait(timeoutError(tr.commit(), 30.0));
				version = tr.getCommittedVersion();
				break;
			} catch (Error& e) {
				wait(tr.onError(e));
			}
		}

		wait(self->readAndVerify(self, cx, key, value));

		return version;
	}

	Future<bool> check(Database const& cx) override { return pass; }

	void getMetrics(std::vector<PerfMetric>& m) override {}
};

WorkloadFactory<SSCheckpointWorkload> SSCheckpointWorkloadFactory("SSCheckpointWorkload");