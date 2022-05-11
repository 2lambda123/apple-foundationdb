/*
 * EncryptKeyProxyTest.actor.cpp
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

#include "fdbrpc/Locality.h"
#include "fdbserver/EncryptKeyProxyInterface.h"
#include "fdbserver/Knobs.h"
#include "fdbserver/ServerDBInfo.actor.h"
#include "fdbserver/WorkerInterface.actor.h"
#include "fdbserver/workloads/workloads.actor.h"
#include "flow/Error.h"
#include "flow/FastRef.h"
#include "flow/Trace.h"
#include "flow/IRandom.h"
#include "flow/flow.h"
#include "flow/xxhash.h"

#include <atomic>
#include <boost/range/const_iterator.hpp>
#include <utility>

#include "flow/actorcompiler.h" // This must be the last #include.

struct EncryptKeyProxyTestWorkload : TestWorkload {
	EncryptKeyProxyInterface ekpInf;
	Reference<AsyncVar<struct ServerDBInfo> const> dbInfo;
	Arena arena;
	uint64_t minDomainId;
	uint64_t maxDomainId;
	std::unordered_map<uint64_t, StringRef> cipherIdMap;
	std::vector<uint64_t> cipherIds;
	int numDomains;
	std::vector<uint64_t> domainIds;
	static std::atomic<int> seed;
	bool enableTest;

	EncryptKeyProxyTestWorkload(WorkloadContext const& wcx) : TestWorkload(wcx), dbInfo(wcx.dbInfo), enableTest(false) {
		if (wcx.clientId == 0) {
			enableTest = true;
			minDomainId = 1000 + (++seed * 30) + 1;
			maxDomainId = deterministicRandom()->randomInt(minDomainId, minDomainId + 50) + 5;
			TraceEvent("EKPTest_Init").detail("MinDomainId", minDomainId).detail("MaxDomainId", maxDomainId);
		}
	}

	std::string description() const override { return "EncryptKeyProxyTest"; }

	Future<Void> setup(Database const& ctx) override { return Void(); }

	ACTOR Future<Void> simEmptyDomainIdCache(EncryptKeyProxyTestWorkload* self) {
		TraceEvent("SimEmptyDomainIdCache_Start").log();

		for (int i = 0; i < self->numDomains / 2; i++) {
			self->domainIds.emplace_back(self->minDomainId + i);
		}

		state int nAttempts = 0;
		loop {
			EKPGetLatestBaseCipherKeysRequest req;
			req.encryptDomainIds = self->domainIds;
			if (deterministicRandom()->randomInt(0, 100) < 50) {
				req.debugId = deterministicRandom()->randomUniqueID();
			}
			ErrorOr<EKPGetLatestBaseCipherKeysReply> rep = wait(self->ekpInf.getLatestBaseCipherKeys.tryGetReply(req));
			if (rep.present()) {

				ASSERT(!rep.get().error.present());
				ASSERT_EQ(rep.get().baseCipherDetails.size(), self->domainIds.size());

				for (const uint64_t id : self->domainIds) {
					bool found = false;
					for (const auto& item : rep.get().baseCipherDetails) {
						if (item.encryptDomainId == id) {
							found = true;
							break;
						}
					}
					ASSERT(found);
				}

				// Ensure no hits reported by the cache.
				if (nAttempts == 0) {
					ASSERT_EQ(rep.get().numHits, 0);
				} else {
					ASSERT_GE(rep.get().numHits, 0);
				}
				break;
			} else {
				nAttempts++;
				wait(delay(0.0));
			}
		}

		TraceEvent("SimEmptyDomainIdCache_Done").log();
		return Void();
	}

	ACTOR Future<Void> simPartialDomainIdCache(EncryptKeyProxyTestWorkload* self) {
		state int expectedHits;
		state int expectedMisses;

		TraceEvent("SimPartialDomainIdCache_Start").log();

		self->domainIds.clear();

		expectedHits = deterministicRandom()->randomInt(1, self->numDomains / 2);
		for (int i = 0; i < expectedHits; i++) {
			self->domainIds.emplace_back(self->minDomainId + i);
		}

		expectedMisses = deterministicRandom()->randomInt(1, self->numDomains / 2);
		for (int i = 0; i < expectedMisses; i++) {
			self->domainIds.emplace_back(self->minDomainId + i + self->numDomains / 2 + 1);
		}

		state int nAttempts = 0;
		loop {
			// Test case given is measuring correctness for cache hit/miss scenarios is designed to have strict
			// assertions. However, in simulation runs, RPCs can be force failed to inject retries, hence, code leverage
			// tryGetReply to ensure at-most once delivery of message, further, assertions are relaxed to account of
			// cache warm-up due to retries.
			EKPGetLatestBaseCipherKeysRequest req;
			req.encryptDomainIds = self->domainIds;
			if (deterministicRandom()->randomInt(0, 100) < 50) {
				req.debugId = deterministicRandom()->randomUniqueID();
			}
			ErrorOr<EKPGetLatestBaseCipherKeysReply> rep = wait(self->ekpInf.getLatestBaseCipherKeys.tryGetReply(req));
			if (rep.present()) {
				ASSERT(!rep.get().error.present());
				ASSERT_EQ(rep.get().baseCipherDetails.size(), self->domainIds.size());

				for (const uint64_t id : self->domainIds) {
					bool found = false;
					for (const auto& item : rep.get().baseCipherDetails) {
						if (item.encryptDomainId == id) {
							found = true;
							break;
						}
					}
					ASSERT(found);
				}

				// Ensure desired cache-hit counts
				if (nAttempts == 0) {
					ASSERT_EQ(rep.get().numHits, expectedHits);
				} else {
					ASSERT_GE(rep.get().numHits, expectedHits);
				}
				break;
			} else {
				nAttempts++;
				wait(delay(0.0));
			}
		}
		self->domainIds.clear();

		TraceEvent("SimPartialDomainIdCache_Done").log();
		return Void();
	}

	ACTOR Future<Void> simRandomBaseCipherIdCache(EncryptKeyProxyTestWorkload* self) {
		state int expectedHits;

		TraceEvent("SimRandomDomainIdCache_Start").log();

		self->domainIds.clear();
		for (int i = 0; i < self->numDomains; i++) {
			self->domainIds.emplace_back(self->minDomainId + i);
		}

		EKPGetLatestBaseCipherKeysRequest req;
		req.encryptDomainIds = self->domainIds;
		if (deterministicRandom()->randomInt(0, 100) < 50) {
			req.debugId = deterministicRandom()->randomUniqueID();
		}
		EKPGetLatestBaseCipherKeysReply rep = wait(self->ekpInf.getLatestBaseCipherKeys.getReply(req));

		ASSERT(!rep.error.present());
		ASSERT_EQ(rep.baseCipherDetails.size(), self->domainIds.size());
		for (const uint64_t id : self->domainIds) {
			bool found = false;
			for (const auto& item : rep.baseCipherDetails) {
				if (item.encryptDomainId == id) {
					found = true;
					break;
				}
			}
			ASSERT(found);
		}

		self->cipherIdMap.clear();
		self->cipherIds.clear();
		for (auto& item : rep.baseCipherDetails) {
			self->cipherIdMap.emplace(item.baseCipherId, StringRef(self->arena, item.baseCipherKey));
			self->cipherIds.emplace_back(item.baseCipherId);
		}

		state int numIterations = deterministicRandom()->randomInt(512, 786);
		for (; numIterations > 0;) {
			int idx = deterministicRandom()->randomInt(1, self->cipherIds.size());
			int nIds = deterministicRandom()->randomInt(1, self->cipherIds.size());

			EKPGetBaseCipherKeysByIdsRequest req;
			if (deterministicRandom()->randomInt(0, 100) < 50) {
				req.debugId = deterministicRandom()->randomUniqueID();
			}
			for (int i = idx; i < nIds && i < self->cipherIds.size(); i++) {
				req.baseCipherIds.emplace_back(std::make_pair(self->cipherIds[i], 1));
			}
			if (req.baseCipherIds.empty()) {
				// No keys to query; continue
				continue;
			} else {
				numIterations--;
			}

			expectedHits = req.baseCipherIds.size();
			EKPGetBaseCipherKeysByIdsReply rep = wait(self->ekpInf.getBaseCipherKeysByIds.getReply(req));

			ASSERT(!rep.error.present());
			ASSERT_EQ(rep.baseCipherDetails.size(), expectedHits);
			ASSERT_EQ(rep.numHits, expectedHits);
			// Valdiate the 'cipherKey' content against the one read while querying by domainIds
			for (auto& item : rep.baseCipherDetails) {
				const auto itr = self->cipherIdMap.find(item.baseCipherId);
				ASSERT(itr != self->cipherIdMap.end());
				Standalone<StringRef> toCompare = self->cipherIdMap[item.baseCipherId];
				if (toCompare.compare(item.baseCipherKey) != 0) {
					TraceEvent("Mismatch")
					    .detail("Id", item.baseCipherId)
					    .detail("CipherMapDataHash", XXH3_64bits(toCompare.begin(), toCompare.size()))
					    .detail("CipherMapSize", toCompare.size())
					    .detail("CipherMapValue", toCompare.toString())
					    .detail("ReadDataHash", XXH3_64bits(item.baseCipherKey.begin(), item.baseCipherKey.size()))
					    .detail("ReadValue", item.baseCipherKey.toString())
					    .detail("ReadDataSize", item.baseCipherKey.size());
					ASSERT(false);
				}
			}
		}

		TraceEvent("SimRandomDomainIdCache_Done").log();
		return Void();
	}

	ACTOR Future<Void> simLookupInvalidKeyId(EncryptKeyProxyTestWorkload* self) {
		TraceEvent("SimLookupInvalidKeyId_Start").log();

		// Prepare a lookup with valid and invalid keyIds - SimEncryptKmsProxy should throw encrypt_key_not_found()
		std::vector<std::pair<uint64_t, int64_t>> baseCipherIds;
		for (auto id : self->cipherIds) {
			baseCipherIds.emplace_back(std::make_pair(id, 1));
		}
		baseCipherIds.emplace_back(std::make_pair(SERVER_KNOBS->SIM_KMS_MAX_KEYS + 10, 1));
		EKPGetBaseCipherKeysByIdsRequest req(deterministicRandom()->randomUniqueID(), baseCipherIds);
		EKPGetBaseCipherKeysByIdsReply rep = wait(self->ekpInf.getBaseCipherKeysByIds.getReply(req));

		ASSERT_EQ(rep.baseCipherDetails.size(), 0);
		ASSERT(rep.error.present());
		ASSERT_EQ(rep.error.get().code(), error_code_encrypt_key_not_found);

		TraceEvent("SimLookupInvalidKeyId_Done").log();
		return Void();
	}

	// Following test cases are covered:
	// 1. Simulate an empty domainIdCache.
	// 2. Simulate an mixed lookup (partial cache-hit) for domainIdCache.
	// 3. Simulate a lookup on all domainIdCache keys and validate lookup by baseCipherKeyIds.
	// 4. Simulate lookup for an invalid baseCipherKeyId.

	ACTOR Future<Void> testWorkload(Reference<AsyncVar<ServerDBInfo> const> dbInfo, EncryptKeyProxyTestWorkload* self) {
		// Ensure EncryptKeyProxy role is recruited (a singleton role)
		while (!dbInfo->get().encryptKeyProxy.present()) {
			wait(delay(.1));
		}

		self->ekpInf = dbInfo->get().encryptKeyProxy.get();
		self->numDomains = self->maxDomainId - self->minDomainId;

		// Simulate empty cache access
		wait(self->simEmptyDomainIdCache(self));

		// Simulate partial cache-hit usecase
		wait(self->simPartialDomainIdCache(self));

		// Warm up cached with all domain Ids and randomly access known baseCipherIds
		wait(self->simRandomBaseCipherIdCache(self));

		// Simulate lookup BaseCipherIds which aren't yet cached
		wait(self->simLookupInvalidKeyId(self));

		return Void();
	}

	Future<Void> start(Database const& cx) override {
		TEST(true); // Testing
		if (!enableTest) {
			return Void();
		}
		return testWorkload(dbInfo, this);
	}

	Future<bool> check(Database const& cx) override { return true; }

	void getMetrics(std::vector<PerfMetric>& m) override {}
};

std::atomic<int> EncryptKeyProxyTestWorkload::seed = 0;

WorkloadFactory<EncryptKeyProxyTestWorkload> EncryptKeyProxyTestWorkloadFactory("EncryptKeyProxyTest");