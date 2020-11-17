/*
 * NetworkTest.h
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

#ifndef FDBSERVER_NETWORKTEST_H
#define FDBSERVER_NETWORKTEST_H
#pragma once

#include "fdbclient/FDBTypes.h"
#include "fdbrpc/fdbrpc.h"
#include "flow/FileIdentifier.h"

struct NetworkTestInterface {
	RequestStream< struct NetworkTestRequest > test;
	NetworkTestInterface() {}
	NetworkTestInterface( NetworkAddress remote );
	NetworkTestInterface( INetwork* local );
};

struct NetworkTestReply {
	constexpr static FileIdentifier file_identifier = 14465374;
	static uint32_t accumulativeIndex;

	uint32_t index;
	Value value;

	NetworkTestReply() : index(++accumulativeIndex) {}
	NetworkTestReply( Value value ) : index(++accumulativeIndex), value(value) {}

	template <class Ar>
	void serialize(Ar& ar) {
		serializer(ar, index, value);
	}
};

struct NetworkTestRequest {
	constexpr static FileIdentifier file_identifier = 4146513;
	static uint32_t accumulativeIndex;

	uint32_t index;
	Key key;
	uint32_t replySize;
	ReplyPromise<struct NetworkTestReply> reply;

	NetworkTestRequest() : index(++accumulativeIndex) {}
	NetworkTestRequest( Key key, uint32_t replySize ) : index(++accumulativeIndex), key(key), replySize(replySize) {}

	template <class Ar>
	void serialize(Ar& ar) {
		serializer(ar, index, key, replySize, reply);
	}
};

Future<Void> networkTestServer();

Future<Void> networkTestClient( std:: string const& testServers );

#endif
