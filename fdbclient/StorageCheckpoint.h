/*
 * StorageCheckpoint.h
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

#ifndef FDBCLIENT_STORAGCHECKPOINT_H
#define FDBCLIENT_STORAGCHECKPOINT_H
#pragma once

#include "fdbclient/FDBTypes.h"

// FDB storage checkpoint format.
enum CheckpointFormat {
	InvalidFormat = 0,
	// For RocksDB, checkpoint generated via rocksdb::Checkpoint::ExportColumnFamily().
	RocksDBColumnFamily = 1,
	// For RocksDB, checkpoint generated via rocksdb::Checkpoint::CreateCheckpoint().
	RocksDB = 2,
};

// Metadata of a FDB checkpoint.
struct CheckpointMetaData {
	enum CheckpointState {
		InvalidState = 0,
		Pending = 1, // Checkpoint creation pending.
		Complete = 2, // Checkpoint is created and ready to be read.
		Deleting = 3, // Checkpoint deletion requested.
		Fail = 4,
	};

	constexpr static FileIdentifier file_identifier = 13804342;
	Version version;
	KeyRange range;
	int16_t format; // CheckpointFormat.
	int16_t state; // CheckpointState.
	UID dataMoveID;
	UID checkpointID; // A unique id for this checkpoint.
	UID ssID; // Storage server ID on which this checkpoint is created.
	int referenceCount; // A reference count on the checkpoint, it can only be deleted when this is 0.
	int64_t gcTime; // Time to delete this checkpoint, a Unix timestamp in seconds.

	// A serialized metadata associated with format, this data can be understood by the corresponding KVS.
	Standalone<StringRef> serializedCheckpoint;

	CheckpointMetaData() : format(InvalidFormat), state(InvalidState), referenceCount(0) {}
	CheckpointMetaData(KeyRange const& range, CheckpointFormat format, UID const& ssID, UID const& checkpointID)
	  : version(invalidVersion), range(range), format(format), ssID(ssID), checkpointID(checkpointID), state(Pending),
	    referenceCount(0) {}
	CheckpointMetaData(Version version, KeyRange const& range, CheckpointFormat format, UID checkpointID)
	  : version(version), range(range), format(format), checkpointID(checkpointID), referenceCount(0) {}

	CheckpointState getState() const { return static_cast<CheckpointState>(state); }

	void setState(CheckpointState state) { this->state = static_cast<int16_t>(state); }

	CheckpointFormat getFormat() const { return static_cast<CheckpointFormat>(format); }

	void setFormat(CheckpointFormat format) { this->format = static_cast<int16_t>(format); }

	std::string toString() const {
		std::string res = "Checkpoint MetaData:\nRange: " + range.toString() + "\nVersion: " + std::to_string(version) +
		                  "\nFormat: " + std::to_string(format) + "\nID: " + checkpointID.toString() +
		                  "\nDataMoveID: " + dataMoveID.toString() + "\nServer: " + ssID.toString() +
		                  "\nState: " + std::to_string(static_cast<int>(state)) + "\n";
		return res;
	}

	template <class Ar>
	void serialize(Ar& ar) {
		serializer(ar, version, range, format, state, dataMoveID, checkpointID, ssID, gcTime, serializedCheckpoint);
	}
};

struct DataMoveMetaData {
	enum Phase {
		InvalidPhase = 0,
		Pending = 1, // Checkpoint creation pending.
		Complete = 2, // Checkpoint is created and ready to be read.
		Deleting = 3, // Checkpoint deletion requested.
		Fail = 4,
	};

	constexpr static FileIdentifier file_identifier = 13804362;
	UID id; // A unique id for this checkpoint.
	Version version;
	KeyRange range;
	int priority;
	std::set<UID> src;
	std::set<UID> dest;
	int16_t phase; // CheckpointState.

	DataMoveMetaData() : phase(InvalidPhase), priority(0) {}
	DataMoveMetaData(UID id, Version version, KeyRange const& range)
	  : id(id), version(version), range(range), priority(0) {}
	DataMoveMetaData(UID id, KeyRange const& range) : id(id), version(invalidVersion), range(range), priority(0) {}

	Phase getPhase() const { return static_cast<Phase>(phase); }

	void setPhase(Phase phase) { this->phase = static_cast<int16_t>(phase); }

	std::string toString() const {
		std::string res = "DataMoveMetaData:\nID: " + id.toString() + "\nRange: " + range.toString() +
		                  "\nVersion: " + std::to_string(version) + "\nPriority: " + std::to_string(priority) +
		                  "\nPhase: " + std::to_string(static_cast<int>(phase)) + "\nSource Servers: " + describe(src) +
		                  "\nDestination Servers: " + describe(dest) + "\n";
		return res;
	}

	// bool operator==(const DataMoveMetaData& other) {
	// 	return this->range == other.range && this->id == other.id &&
	// 	       std::equal(this->src.begin(), this->src.end(), other.src.begin()) &&
	// 	       std::equal(this->dest.begin(), this->dest.end(), other.dest.begin());
	// }

	template <class Ar>
	void serialize(Ar& ar) {
		serializer(ar, id, version, range, phase, src, dest);
	}
};

#endif
