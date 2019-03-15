/*
 * TLogInterface.h
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

#ifndef FDBSERVER_TLOGINTERFACE_H
#define FDBSERVER_TLOGINTERFACE_H
#pragma once

#include "fdbclient/FDBTypes.h"
#include "fdbclient/CommitTransaction.h"
#include "fdbclient/MutationList.h"
#include "fdbclient/StorageServerInterface.h"
#include <iterator>

struct TLogInterface {
	enum { LocationAwareLoadBalance = 1 };
	enum { AlwaysFresh = 1 };

	LocalityData locality;
	UID uniqueID;
	UID sharedTLogID;
	RequestStream< struct TLogPeekRequest > peekMessages;
	RequestStream< struct TLogPopRequest > popMessages;

	RequestStream< struct TLogCommitRequest > commit;
	RequestStream< ReplyPromise< struct TLogLockResult > > lock; // first stage of database recovery
	RequestStream< struct TLogQueuingMetricsRequest > getQueuingMetrics;
	RequestStream< struct TLogConfirmRunningRequest > confirmRunning; // used for getReadVersion requests from client
	RequestStream<ReplyPromise<Void>> waitFailure;
	RequestStream< struct TLogRecoveryFinishedRequest > recoveryFinished;
	
	TLogInterface() {}
	explicit TLogInterface(LocalityData locality) : uniqueID( g_random->randomUniqueID() ), locality(locality) { sharedTLogID = uniqueID; }
	TLogInterface(UID sharedTLogID, LocalityData locality) : uniqueID( g_random->randomUniqueID() ), sharedTLogID(sharedTLogID), locality(locality) {}
	TLogInterface(UID uniqueID, UID sharedTLogID, LocalityData locality) : uniqueID(uniqueID), sharedTLogID(sharedTLogID), locality(locality) {}
	UID id() const { return uniqueID; }
	UID getSharedTLogID() const { return sharedTLogID; }
	std::string toString() const { return id().shortString(); }
	bool operator == ( TLogInterface const& r ) const { return id() == r.id(); }
	NetworkAddress address() const { return peekMessages.getEndpoint().getPrimaryAddress(); }
	void initEndpoints() {
		Endpoint base = peekMessages.initEndpoint(nullptr, TaskTLogPeek);
		popMessages.initEndpoint(&base, TaskTLogPop);
		commit.initEndpoint(&base, TaskTLogCommit);
		lock.initEndpoint(&base);
		getQueuingMetrics.initEndpoint(&base, TaskTLogQueuingMetrics);
		confirmRunning.initEndpoint(&base, TaskTLogConfirmRunning);
		waitFailure.initEndpoint(&base);
		recoveryFinished.initEndpoint(&base);
		TraceEvent("DumpToken", id()).detail("Name", "TLogInterface").detail("Token", base.token);
	}

	template <class Ar> 
	void serialize( Ar& ar ) {
		ASSERT(ar.isDeserializing || uniqueID != UID());
		serializer(ar, uniqueID, sharedTLogID, locality, peekMessages);
		auto holder = FlowTransport::transport().setBaseEndpoint(peekMessages.getEndpoint());
		serializer(ar, popMessages, commit, lock, getQueuingMetrics, confirmRunning, waitFailure, recoveryFinished);
	}
};

struct TLogRecoveryFinishedRequest {
	ReplyPromise<Void> reply;

	TLogRecoveryFinishedRequest() {}

	template <class Ar> 
	void serialize( Ar& ar ) {
		serializer(ar, reply);
	}
};

struct TLogLockResult {
	Version end;
	Version knownCommittedVersion;

	template <class Ar>
	void serialize( Ar& ar ) {
		serializer(ar, end, knownCommittedVersion);
	}
};

struct TLogConfirmRunningRequest {
	Optional<UID> debugID;
	ReplyPromise<Void> reply;

	TLogConfirmRunningRequest() {}
	TLogConfirmRunningRequest( Optional<UID> debugID ) : debugID(debugID) {}

	template <class Ar> 
	void serialize( Ar& ar ) {
		serializer(ar, debugID, reply);
	}
};

struct VersionUpdateRef {
	Version version;
	MutationListRef mutations;
	bool isPrivateData;

	VersionUpdateRef() : isPrivateData(false), version(invalidVersion) {}
	VersionUpdateRef( Arena& to, const VersionUpdateRef& from ) : version(from.version), mutations( to, from.mutations ), isPrivateData( from.isPrivateData ) {}
	int totalSize() const { return mutations.totalSize(); }
	int expectedSize() const { return mutations.expectedSize(); }

	template <class Ar> 
	void serialize( Ar& ar ) {
		serializer(ar, version, mutations, isPrivateData);
	}
};

struct VerUpdateRef {
	Version version;
	VectorRef<MutationRef> mutations;
	bool isPrivateData;

	VerUpdateRef() : isPrivateData(false), version(invalidVersion) {}
	VerUpdateRef( Arena& to, const VerUpdateRef& from ) : version(from.version), mutations( to, from.mutations ), isPrivateData( from.isPrivateData ) {}
	int expectedSize() const { return mutations.expectedSize(); }

	template <class Ar>
	void serialize( Ar& ar ) {
		serializer(ar, version, mutations, isPrivateData);
	}
};

struct TLogPeekReply {
	Arena arena;
	StringRef messages;
	Version end;
	Optional<Version> popped;
	Version maxKnownVersion;
	Version minKnownCommittedVersion;
	Optional<Version> begin;

	template <class Ar>
	void serialize(Ar& ar) {
		serializer(ar, arena, messages, end, popped, maxKnownVersion, minKnownCommittedVersion, begin);
	}
};

struct TLogPeekRequest {
	Arena arena;
	Version begin;
	Tag tag;
	bool returnIfBlocked;
	Optional<std::pair<UID, int>> sequence;
	ReplyPromise<TLogPeekReply> reply;

	TLogPeekRequest( Version begin, Tag tag, bool returnIfBlocked, Optional<std::pair<UID, int>> sequence = Optional<std::pair<UID, int>>() ) : begin(begin), tag(tag), returnIfBlocked(returnIfBlocked), sequence(sequence) {}
	TLogPeekRequest() {}

	template <class Ar>
	void serialize(Ar& ar) {
		serializer(ar, arena, begin, tag, returnIfBlocked, sequence, reply);
	}
};

struct TLogPopRequest {
	Arena arena;
	Version to;
	Version durableKnownCommittedVersion;
	Tag tag;
	ReplyPromise<Void> reply;

	TLogPopRequest( Version to, Version durableKnownCommittedVersion, Tag tag ) : to(to), durableKnownCommittedVersion(durableKnownCommittedVersion), tag(tag) {}
	TLogPopRequest() {}

	template <class Ar>
	void serialize(Ar& ar) {
		serializer(ar, arena, to, durableKnownCommittedVersion, tag, reply);
	}
};

struct TagMessagesRef {
	Tag tag;
	VectorRef<int> messageOffsets;

	TagMessagesRef() {}
	TagMessagesRef(Arena &a, const TagMessagesRef &from) : tag(from.tag), messageOffsets(a, from.messageOffsets) {}

	size_t expectedSize() const {
		return messageOffsets.expectedSize();
	}

	template <class Ar>
	void serialize(Ar& ar) {
		serializer(ar, tag, messageOffsets);
	}
};

struct TLogCommitRequest {
	Arena arena;
	Version prevVersion, version, knownCommittedVersion, minKnownCommittedVersion;

	StringRef messages;// Each message prefixed by a 4-byte length

	ReplyPromise<Version> reply;
	Optional<UID> debugID;

	TLogCommitRequest() {}
	TLogCommitRequest( const Arena& a, Version prevVersion, Version version, Version knownCommittedVersion, Version minKnownCommittedVersion, StringRef messages, Optional<UID> debugID )
		: arena(a), prevVersion(prevVersion), version(version), knownCommittedVersion(knownCommittedVersion), minKnownCommittedVersion(minKnownCommittedVersion), messages(messages), debugID(debugID) {}
	template <class Ar>
	void serialize( Ar& ar ) {
		serializer(ar, prevVersion, version, knownCommittedVersion, minKnownCommittedVersion, messages, reply, arena, debugID);
	}
};

struct TLogQueuingMetricsRequest {
	ReplyPromise<struct TLogQueuingMetricsReply> reply;

	template <class Ar>
	void serialize(Ar& ar) {
		serializer(ar, reply);
	}
};

struct TLogQueuingMetricsReply {
	double localTime;
	int64_t instanceID;  // changes if bytesDurable and bytesInput reset
	int64_t bytesDurable, bytesInput;
	StorageBytes storageBytes;
	Version v; // committed version

	template <class Ar>
	void serialize(Ar& ar) {
		serializer(ar, localTime, instanceID, bytesDurable, bytesInput, storageBytes, v);
	}
};

#endif
