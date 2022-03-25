/*
 * BlobGranuleServerCommon.actor.cpp
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

#include "fdbclient/SystemData.h"
#include "fdbclient/BlobGranuleCommon.h"
#include "fdbserver/BlobGranuleServerCommon.actor.h"
#include "fdbclient/CommitTransaction.h"
#include "fdbclient/FDBTypes.h"
#include "fdbclient/ReadYourWrites.h"
#include "flow/Arena.h"
#include "flow/actorcompiler.h" // has to be last include

// Gets the latest granule history node for range that was persisted
ACTOR Future<Optional<GranuleHistory>> getLatestGranuleHistory(Transaction* tr, KeyRange range) {
	state KeyRange historyRange = blobGranuleHistoryKeyRangeFor(range);
	state RangeResult result = wait(tr->getRange(historyRange, 1, Snapshot::False, Reverse::True));

	ASSERT(result.size() <= 1);

	Optional<GranuleHistory> history;
	if (!result.empty()) {
		std::pair<KeyRange, Version> decodedKey = decodeBlobGranuleHistoryKey(result[0].key);
		ASSERT(range == decodedKey.first);
		history = GranuleHistory(range, decodedKey.second, decodeBlobGranuleHistoryValue(result[0].value));
	}
	return history;
}

// Gets the files based on the file key range [startKey, endKey)
// and populates the files object accordingly
ACTOR Future<Void> readGranuleFiles(Transaction* tr, Key* startKey, Key endKey, GranuleFiles* files, UID granuleID) {

	loop {
		int lim = BUGGIFY ? 2 : 1000;
		RangeResult res = wait(tr->getRange(KeyRangeRef(*startKey, endKey), lim));
		for (auto& it : res) {
			UID gid;
			uint8_t fileType;
			Version version;

			Standalone<StringRef> filename;
			int64_t offset;
			int64_t length;

			std::tie(gid, version, fileType) = decodeBlobGranuleFileKey(it.key);
			ASSERT(gid == granuleID);

			std::tie(filename, offset, length) = decodeBlobGranuleFileValue(it.value);

			BlobFileIndex idx(version, filename.toString(), offset, length);
			if (fileType == 'S') {
				ASSERT(files->snapshotFiles.empty() || files->snapshotFiles.back().version < idx.version);
				files->snapshotFiles.push_back(idx);
			} else {
				ASSERT(fileType == 'D');
				ASSERT(files->deltaFiles.empty() || files->deltaFiles.back().version < idx.version);
				files->deltaFiles.push_back(idx);
			}
		}
		if (res.more) {
			*startKey = keyAfter(res.back().key);
		} else {
			break;
		}
	}
	return Void();
}

// Wrapper around readGranuleFiles
// Gets all files belonging to the granule with id granule ID
ACTOR Future<GranuleFiles> loadHistoryFiles(Database cx, UID granuleID) {
	state KeyRange range = blobGranuleFileKeyRangeFor(granuleID);
	state Key startKey = range.begin;
	state GranuleFiles files;
	state Transaction tr(cx);

	loop {
		try {
			tr.setOption(FDBTransactionOptions::ACCESS_SYSTEM_KEYS);
			tr.setOption(FDBTransactionOptions::PRIORITY_SYSTEM_IMMEDIATE);
			wait(readGranuleFiles(&tr, &startKey, range.end, &files, granuleID));
			return files;
		} catch (Error& e) {
			wait(tr.onError(e));
		}
	}
}
