/*
 * ProxyTLogPushMessageSerializer.h
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

#ifndef FDBSERVER_PTXN_PROXYTLOGPUSHMESSAGESERIALIZER_H
#define FDBSERVER_PTXN_PROXYTLOGPUSHMESSAGESERIALIZER_H

#pragma once

#include <cstdint>
#include <unordered_map>

#include "fdbserver/ptxn/MessageTypes.h"
#include "fdbserver/ptxn/Serializer.h"
#include "flow/Error.h"

namespace ptxn {

const SerializationProtocolVersion ProxyTLogMessageProtocolVersion = 1;

// When passing a series of mutations, or a commit, from Proxy to TLog, the ProxyTLogMessageHeader
// is prefixed to the mutations.
struct ProxyTLogMessageHeader : MultipleItemHeaderBase {
	static constexpr FileIdentifier file_identifier = 356918;

	ProxyTLogMessageHeader() : MultipleItemHeaderBase(ProxyTLogMessageProtocolVersion) {}
};

// Encodes the mutations that commit proxy have received, for TLog's consumption
class ProxyTLogPushMessageSerializer {
	// Maps the TeamID to the list of BinaryWriters
	std::unordered_map<TeamID, HeaderedItemsSerializer<ProxyTLogMessageHeader, SubsequenceMutationItem>> writers;

	// Subsequence of the mutation
	// NOTE: The subsequence is designed to start at 1. This allows a cursor,
	// which initialized at subsequence 0, not positioned at a mutation. This
	// simplifies the implementation of iteration.
	// e.g. for a given TeamID and a given version
	//  Subversion      1    3    6    7   ...
	//              ^ cursor starts here, thus we can write
	//  while(pCursor->hasMessage()) pCursor->getMessage();
	// If the currentSubsequence starts at 0, we have to verify if the initial
	// cursor is located at a mutation, or located at end-of-subsequences,
	// brings extra complexity.
	// This is the sequential of using unsigned integer as the subsequence.
	Subsequence currentSubsequence = 1;

public:
	// For a given TeamID, serialize a new mutation
	void writeMessage(const MutationRef& mutation, const TeamID& teamID);

	// For a given TeamID, mark the serializer not accepting more mutations, and write the header.
	void completeMessageWriting(const TeamID& teamID);

	// Get the serialized data for a given TeamID
	Standalone<StringRef> getSerialized(const TeamID& teamID);
};

// Deserialize the ProxyTLogPushMessage
bool proxyTLogPushMessageDeserializer(const Arena& arena,
                                      StringRef serialized,
                                      ProxyTLogMessageHeader& header,
                                      std::vector<SubsequenceMutationItem>& messages);

} // namespace ptxn

#endif // FDBSERVER_PTXN_PROXYTLOGPUSHMESSAGESERIALIZER_H
