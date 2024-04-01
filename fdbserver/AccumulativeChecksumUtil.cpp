/*
 * AccumulativeChecksumUtil.cpp
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

#include "fdbserver/AccumulativeChecksumUtil.h"
#include "fdbserver/Knobs.h"

void updateMutationWithAcsAndAddMutationToAcsBuilder(std::shared_ptr<AccumulativeChecksumBuilder> acsBuilder,
                                                     MutationRef& mutation,
                                                     Tag inputTag,
                                                     uint16_t acsIndex,
                                                     LogEpoch epoch,
                                                     Version commitVersion,
                                                     UID commitProxyId) {
	mutation.populateChecksum();
	mutation.setAccumulativeChecksumIndex(acsIndex);
	acsBuilder->addMutation(mutation, inputTag, epoch, commitProxyId, commitVersion);
	return;
}

void updateMutationWithAcsAndAddMutationToAcsBuilder(std::shared_ptr<AccumulativeChecksumBuilder> acsBuilder,
                                                     MutationRef& mutation,
                                                     const std::vector<Tag>& inputTags,
                                                     uint16_t acsIndex,
                                                     LogEpoch epoch,
                                                     Version commitVersion,
                                                     UID commitProxyId) {
	mutation.populateChecksum();
	mutation.setAccumulativeChecksumIndex(acsIndex);
	for (const auto& inputTag : inputTags) {
		acsBuilder->addMutation(mutation, inputTag, epoch, commitProxyId, commitVersion);
	}
	return;
}

void updateMutationWithAcsAndAddMutationToAcsBuilder(std::shared_ptr<AccumulativeChecksumBuilder> acsBuilder,
                                                     MutationRef& mutation,
                                                     const std::set<Tag>& inputTags,
                                                     uint16_t acsIndex,
                                                     LogEpoch epoch,
                                                     Version commitVersion,
                                                     UID commitProxyId) {
	mutation.populateChecksum();
	mutation.setAccumulativeChecksumIndex(acsIndex);
	for (const auto& inputTag : inputTags) {
		acsBuilder->addMutation(mutation, inputTag, epoch, commitProxyId, commitVersion);
	}
	return;
}

void AccumulativeChecksumBuilder::addMutation(const MutationRef& mutation,
                                              Tag tag,
                                              LogEpoch epoch,
                                              UID commitProxyId,
                                              Version commitVersion) {
	ASSERT(CLIENT_KNOBS->ENABLE_MUTATION_CHECKSUM && CLIENT_KNOBS->ENABLE_ACCUMULATIVE_CHECKSUM);
	if (!tagSupportAccumulativeChecksum(tag)) {
		return;
	}
	uint32_t oldAcs = 0;
	auto it = acsTable.find(tag);
	if (it != acsTable.end()) {
		oldAcs = it->second.acs;
	}
	uint32_t newAcs = updateTable(tag, mutation.checksum.get(), commitVersion, epoch);
	if (CLIENT_KNOBS->ENABLE_ACCUMULATIVE_CHECKSUM_LOGGING) {
		TraceEvent(SevInfo, "AcsBuilderAddMutation", commitProxyId)
		    .detail("AcsTag", tag)
		    .detail("AcsIndex", mutation.accumulativeChecksumIndex.get())
		    .detail("CommitVersion", commitVersion)
		    .detail("OldAcs", oldAcs)
		    .detail("NewAcs", newAcs)
		    .detail("Mutation", mutation);
	}
	return;
}

uint32_t AccumulativeChecksumBuilder::updateTable(Tag tag, uint32_t checksum, Version version, LogEpoch epoch) {
	ASSERT(CLIENT_KNOBS->ENABLE_MUTATION_CHECKSUM && CLIENT_KNOBS->ENABLE_ACCUMULATIVE_CHECKSUM);
	uint32_t newAcs = 0;
	auto it = acsTable.find(tag);
	if (it == acsTable.end()) {
		newAcs = checksum;
		acsTable[tag] = AccumulativeChecksumState(acsIndex, newAcs, version, epoch);
	} else {
		ASSERT(version >= it->second.version);
		ASSERT(version >= currentVersion);
		newAcs = calculateAccumulativeChecksum(it->second.acs, checksum);
		it->second = AccumulativeChecksumState(acsIndex, newAcs, version, epoch);
	}
	currentVersion = version;
	return newAcs;
}

void AccumulativeChecksumBuilder::newTag(Tag tag, UID ssid, Version commitVersion) {
	ASSERT(CLIENT_KNOBS->ENABLE_MUTATION_CHECKSUM && CLIENT_KNOBS->ENABLE_ACCUMULATIVE_CHECKSUM);
	bool exist = acsTable.erase(tag) > 0;
	if (CLIENT_KNOBS->ENABLE_ACCUMULATIVE_CHECKSUM_LOGGING) {
		TraceEvent(SevInfo, "AcsBuilderNewAcsTag")
		    .detail("AcsIndex", acsIndex)
		    .detail("AcsTag", tag)
		    .detail("CommitVersion", commitVersion)
		    .detail("Exist", exist)
		    .detail("SSID", ssid);
	}
}

void AccumulativeChecksumValidator::addMutation(const MutationRef& mutation, UID ssid, Tag tag, Version ssVersion) {
	ASSERT(CLIENT_KNOBS->ENABLE_MUTATION_CHECKSUM && CLIENT_KNOBS->ENABLE_ACCUMULATIVE_CHECKSUM);
	ASSERT(mutation.checksum.present() && mutation.accumulativeChecksumIndex.present());
	const uint16_t& acsIndex = mutation.accumulativeChecksumIndex.get();
	Version atAcsVersion = 0;
	if (!mutationBuffer.empty()) {
		ASSERT(mutationBuffer[0].accumulativeChecksumIndex.present());
		if (mutationBuffer[0].accumulativeChecksumIndex.get() != acsIndex) {
			TraceEvent(SevError, "AcsValidatorMissingAcs", ssid)
			    .detail("AcsTag", tag)
			    .detail("AcsIndex", acsIndex)
			    .detail("MissingAcsIndex", mutationBuffer[0].accumulativeChecksumIndex.get())
			    .detail("Mutation", mutation.toString())
			    .detail("LastAcsVersion", atAcsVersion)
			    .detail("SSVersion", ssVersion);
		}
	}
	mutationBuffer.push_back(mutationBuffer.arena(), mutation);
	totalAddedMutations++;
	if (CLIENT_KNOBS->ENABLE_ACCUMULATIVE_CHECKSUM_LOGGING) {
		TraceEvent(SevInfo, "AcsValidatorAddMutation", ssid)
		    .detail("AcsTag", tag)
		    .detail("AcsIndex", acsIndex)
		    .detail("Mutation", mutation.toString())
		    .detail("LastAcsVersion", atAcsVersion)
		    .detail("SSVersion", ssVersion);
	}
}

Optional<AccumulativeChecksumState> AccumulativeChecksumValidator::processAccumulativeChecksum(
    const AccumulativeChecksumState& acsMutationState,
    UID ssid,
    Tag tag,
    Version ssVersion) {
	ASSERT(CLIENT_KNOBS->ENABLE_MUTATION_CHECKSUM && CLIENT_KNOBS->ENABLE_ACCUMULATIVE_CHECKSUM);
	const LogEpoch& epoch = acsMutationState.epoch;
	const uint16_t& acsIndex = acsMutationState.acsIndex;
	auto it = acsTable.find(acsIndex);
	if (it == acsTable.end()) {
		// Unexpected. Since we assign acs mutation in commit batch
		// So, there must be acs entry set up when adding the mutations of the batch
		acsTable[acsIndex] = acsMutationState;
		mutationBuffer.clear();
		if (CLIENT_KNOBS->ENABLE_ACCUMULATIVE_CHECKSUM_LOGGING) {
			TraceEvent(SevError, "AcsValidatorAcsMutationSkip", ssid)
			    .detail("Reason", "No Entry")
			    .detail("AcsTag", tag)
			    .detail("AcsIndex", acsIndex)
			    .detail("SSVersion", ssVersion)
			    .detail("Epoch", epoch);
		}
		return acsMutationState;
	}
	if ((acsMutationState.version < it->second.version || acsMutationState.epoch < it->second.epoch)) {
		mutationBuffer.clear();
		if (CLIENT_KNOBS->ENABLE_ACCUMULATIVE_CHECKSUM_LOGGING) {
			TraceEvent(SevError, "AcsValidatorAcsMutationSkip", ssid)
			    .detail("Reason", "Acs Mutation Too Old")
			    .detail("AcsTag", tag)
			    .detail("AcsIndex", acsIndex)
			    .detail("SSVersion", ssVersion)
			    .detail("AcsMutation", acsMutationState.toString())
			    .detail("Epoch", epoch);
		}
		return Optional<AccumulativeChecksumState>();
	}
	// Clear the old acs state if new epoch comes
	bool cleared = false;
	if (acsMutationState.epoch > it->second.epoch) {
		acsTable.erase(it);
		cleared = true;
	}
	// Apply mutations in cache to acs
	ASSERT(mutationBuffer.size() >= 1);
	uint32_t oldAcs = !cleared ? it->second.acs : initialAccumulativeChecksum;
	Version oldVersion = !cleared ? it->second.version : 0;
	uint32_t newAcs = aggregateAcs(oldAcs, mutationBuffer);
	checkedMutations = checkedMutations + mutationBuffer.size();
	checkedVersions = checkedVersions + 1;
	Version newVersion = acsMutationState.version;
	if (newAcs != acsMutationState.acs) {
		TraceEvent(SevError, "AcsValidatorAcsMutationMismatch", ssid)
		    .detail("AcsTag", tag)
		    .detail("AcsIndex", acsIndex)
		    .detail("SSVersion", ssVersion)
		    .detail("FromAcs", oldAcs)
		    .detail("FromVersion", oldVersion)
		    .detail("ToAcs", newAcs)
		    .detail("ToVersion", newVersion)
		    .detail("AcsToValidate", acsMutationState.acs)
		    .detail("Epoch", acsMutationState.epoch)
		    .detail("Cleared", cleared);
		throw please_reboot();
	} else {
		if (CLIENT_KNOBS->ENABLE_ACCUMULATIVE_CHECKSUM_LOGGING) {
			TraceEvent(SevInfo, "AcsValidatorAcsMutationValidated", ssid)
			    .detail("AcsTag", tag)
			    .detail("AcsIndex", acsIndex)
			    .detail("SSVersion", ssVersion)
			    .detail("FromAcs", oldAcs)
			    .detail("FromVersion", oldVersion)
			    .detail("ToAcs", newAcs)
			    .detail("ToVersion", newVersion)
			    .detail("Epoch", acsMutationState.epoch)
			    .detail("Cleared", cleared);
		}
	}
	it->second = acsMutationState;
	mutationBuffer.clear();
	return acsMutationState;
}

void AccumulativeChecksumValidator::restore(const AccumulativeChecksumState& acsState,
                                            UID ssid,
                                            Tag tag,
                                            Version ssVersion) {
	ASSERT(CLIENT_KNOBS->ENABLE_MUTATION_CHECKSUM && CLIENT_KNOBS->ENABLE_ACCUMULATIVE_CHECKSUM);
	const uint16_t& acsIndex = acsState.acsIndex;
	acsTable[acsIndex] = acsState;
	if (CLIENT_KNOBS->ENABLE_ACCUMULATIVE_CHECKSUM_LOGGING) {
		TraceEvent(SevInfo, "AcsValidatorRestore", ssid)
		    .detail("AcsIndex", acsIndex)
		    .detail("AcsTag", tag)
		    .detail("AcsState", acsState.toString())
		    .detail("SSVersion", ssVersion)
		    .detail("Epoch", acsState.epoch);
	}
}

void AccumulativeChecksumValidator::clearCache(UID ssid, Tag tag, Version ssVersion) {
	ASSERT(CLIENT_KNOBS->ENABLE_MUTATION_CHECKSUM && CLIENT_KNOBS->ENABLE_ACCUMULATIVE_CHECKSUM);
	if (!mutationBuffer.empty()) {
		TraceEvent(SevError, "AcsValidatorCachedMutationNotChecked", ssid)
		    .detail("AcsTag", tag)
		    .detail("SSVersion", ssVersion);
	}
}

uint64_t AccumulativeChecksumValidator::getAndClearCheckedMutations() {
	ASSERT(CLIENT_KNOBS->ENABLE_MUTATION_CHECKSUM && CLIENT_KNOBS->ENABLE_ACCUMULATIVE_CHECKSUM);
	uint64_t res = checkedMutations;
	checkedMutations = 0;
	return res;
}

uint64_t AccumulativeChecksumValidator::getAndClearCheckedVersions() {
	ASSERT(CLIENT_KNOBS->ENABLE_MUTATION_CHECKSUM && CLIENT_KNOBS->ENABLE_ACCUMULATIVE_CHECKSUM);
	uint64_t res = checkedVersions;
	checkedVersions = 0;
	return res;
}

uint64_t AccumulativeChecksumValidator::getAndClearTotalMutations() {
	ASSERT(CLIENT_KNOBS->ENABLE_MUTATION_CHECKSUM && CLIENT_KNOBS->ENABLE_ACCUMULATIVE_CHECKSUM);
	uint64_t res = totalMutations;
	totalMutations = 0;
	return res;
}

uint64_t AccumulativeChecksumValidator::getAndClearTotalAcsMutations() {
	ASSERT(CLIENT_KNOBS->ENABLE_MUTATION_CHECKSUM && CLIENT_KNOBS->ENABLE_ACCUMULATIVE_CHECKSUM);
	uint64_t res = totalAcsMutations;
	totalAcsMutations = 0;
	return res;
}

uint64_t AccumulativeChecksumValidator::getAndClearTotalAddedMutations() {
	ASSERT(CLIENT_KNOBS->ENABLE_MUTATION_CHECKSUM && CLIENT_KNOBS->ENABLE_ACCUMULATIVE_CHECKSUM);
	uint64_t res = totalAddedMutations;
	totalAddedMutations = 0;
	return res;
}

TEST_CASE("noSim/AccumulativeChecksum/MutationRef") {
	printf("testing MutationRef encoding/decoding\n");
	MutationRef m(MutationRef::SetValue, "TestKey"_sr, "TestValue"_sr);
	m.setAccumulativeChecksumIndex(512);
	BinaryWriter wr(AssumeVersion(ProtocolVersion::withMutationChecksum()));

	wr << m;

	Standalone<StringRef> value = wr.toValue();
	TraceEvent("EncodedMutation").detail("RawBytes", value);

	BinaryReader rd(value, AssumeVersion(ProtocolVersion::withMutationChecksum()));
	Standalone<MutationRef> de;

	rd >> de;

	printf("Deserialized mutation: %s\n", de.toString().c_str());

	if (de.type != m.type || de.param1 != m.param1 || de.param2 != m.param2) {
		TraceEvent(SevError, "MutationMismatch")
		    .detail("OldType", m.type)
		    .detail("NewType", de.type)
		    .detail("OldParam1", m.param1)
		    .detail("NewParam1", de.param1)
		    .detail("OldParam2", m.param2)
		    .detail("NewParam2", de.param2);
		ASSERT(false);
	}

	ASSERT(de.validateChecksum());

	Standalone<MutationRef> acsMutation;
	LogEpoch epoch = 0;
	uint16_t acsIndex = 1;
	Standalone<StringRef> param2 = accumulativeChecksumValue(AccumulativeChecksumState(acsIndex, 1, 20, epoch));
	acsMutation.type = MutationRef::SetValue;
	acsMutation.param1 = accumulativeChecksumKey;
	acsMutation.param2 = param2;
	acsMutation.setAccumulativeChecksumIndex(1);
	acsMutation.populateChecksum();
	BinaryWriter acsWr(IncludeVersion());
	acsWr << acsMutation;
	Standalone<StringRef> acsValue = acsWr.toValue();
	BinaryReader acsRd(acsValue, IncludeVersion());
	Standalone<MutationRef> acsDe;
	acsRd >> acsDe;
	if (acsDe.type != MutationRef::SetValue || acsDe.param1 != accumulativeChecksumKey || acsDe.param2 != param2) {
		TraceEvent(SevError, "AcsMutationMismatch");
		ASSERT(false);
	}
	ASSERT(acsDe.validateChecksum());

	return Void();
}
