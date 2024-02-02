/*
 * CommitTransaction.h
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

#ifndef FLOW_FDBCLIENT_COMMITTRANSACTION_H
#define FLOW_FDBCLIENT_COMMITTRANSACTION_H
#pragma once

#include "fdbclient/BlobCipher.h"
#include "fdbclient/FDBTypes.h"
#include "fdbclient/GetEncryptCipherKeys.h"
#include "fdbclient/Knobs.h"
#include "fdbclient/Tracing.h"
#include "flow/EncryptUtils.h"
#include "flow/Knobs.h"
#include "flow/UnitTest.h"

#include "crc32/crc32c.h"
#include <unordered_set>

// The versioned message has wire format : -1, version, messages
static const int32_t VERSION_HEADER = -1;

static const char* typeString[] = { "SetValue",
	                                "ClearRange",
	                                "AddValue",
	                                "DebugKeyRange",
	                                "DebugKey",
	                                "NoOp",
	                                "And",
	                                "Or",
	                                "Xor",
	                                "AppendIfFits",
	                                "AvailableForReuse",
	                                "Reserved_For_LogProtocolMessage",
	                                "Max",
	                                "Min",
	                                "SetVersionstampedKey",
	                                "SetVersionstampedValue",
	                                "ByteMin",
	                                "ByteMax",
	                                "MinV2",
	                                "AndV2",
	                                "CompareAndClear",
	                                "Reserved_For_SpanContextMessage",
	                                "Reserved_For_OTELSpanContextMessage",
	                                "Encrypted",
	                                "MAX_ATOMIC_OP" };

struct MutationRef {
	static const int OVERHEAD_BYTES = 12; // 12 is the size of Header in MutationList entries
	enum Type : uint8_t {
		SetValue = 0,
		ClearRange,
		AddValue,
		DebugKeyRange,
		DebugKey,
		NoOp,
		And,
		Or,
		Xor,
		AppendIfFits,
		AvailableForReuse,
		Reserved_For_LogProtocolMessage /* See fdbserver/LogProtocolMessage.h */,
		Max,
		Min,
		SetVersionstampedKey,
		SetVersionstampedValue,
		ByteMin,
		ByteMax,
		MinV2,
		AndV2,
		CompareAndClear,
		Reserved_For_SpanContextMessage /* See fdbserver/SpanContextMessage.h */,
		Reserved_For_OTELSpanContextMessage,
		Encrypted, /* Represents an encrypted mutation and cannot be used directly before decrypting */
		MAX_ATOMIC_OP
	};
	// This is stored this way for serialization purposes.
	uint8_t type;
	StringRef param1, param2;
	Optional<uint32_t> checksum;

	MutationRef() : type(MAX_ATOMIC_OP) {}
	MutationRef(Type t, StringRef a, StringRef b) : type(t), param1(a), param2(b) {}
	MutationRef(Arena& to, Type t, StringRef a, StringRef b) : type(t), param1(to, a), param2(to, b) {}
	MutationRef(Arena& to, const MutationRef& from)
	  : type(from.type), param1(to, from.param1), param2(to, from.param2) {}
	int totalSize() const { return OVERHEAD_BYTES + param1.size() + param2.size(); }
	int expectedSize() const { return param1.size() + param2.size(); }
	int weightedTotalSize() const {
		// AtomicOp can cause more workload to FDB cluster than the same-size set mutation;
		// Amplify atomicOp size to consider such extra workload.
		// A good value for FASTRESTORE_ATOMICOP_WEIGHT needs experimental evaluations.
		if (isAtomicOp()) {
			return totalSize() * CLIENT_KNOBS->FASTRESTORE_ATOMICOP_WEIGHT;
		} else {
			return totalSize();
		}
	}

	std::string toString() const {
		return format("code: %s param1: %s param2: %s",
		              type < MutationRef::MAX_ATOMIC_OP ? typeString[(int)type] : "Unset",
		              printable(param1).c_str(),
		              printable(param2).c_str());
	}

	uint8_t typeWithChecksum() const { return this->type | (uint8_t)(128); }
	void removeChecksum() {
		ASSERT(param2.size() >= 4);
		type &= ~(uint8_t)(128);
		param2 = param2.substr(0, param2.size() - 4);
	}
	bool withChecksum() const { return this->type & (uint8_t)(128); }

	bool isAtomicOp() const { return (ATOMIC_MASK & (1 << type)) != 0; }
	bool isValid() const { return type < MAX_ATOMIC_OP; }

	template <class Ar>
	void serialize(Ar& ar) {
		if (ar.isSerializing && type == ClearRange && equalsKeyAfter(param1, param2)) {
			StringRef empty;
			if (!isEncrypted() && ar.protocolVersion().hasMutationChecksum() &&
			    CLIENT_KNOBS->ENABLE_MUTATION_CHECKSUM) {
				// serializeWithChecksum(ar, param2, empty);
				uint32_t c = crc32c_append(static_cast<uint32_t>(this->type), param1.begin(), param1.size());
				crc32c_append(c, param2.begin(), param2.size());
				if (this->checksum.present()) {
					// ASSERT_EQ(this->checksum.get(), c);
				} else {
					this->checksum = c;
				}
				// StringRef cs = *(StringRef*)&c;
				StringRef cs = StringRef((uint8_t*)&c, 4);
				uint8_t cType = this->typeWithChecksum();
				serializer(ar, cType, param2, cs);
				TraceEvent(SevVerbose, "MutationRefChecksum")
				    .detail("CType", cType)
				    .detail("Mutation", this->toString())
				    .detail("Checksum", std::to_string(c))
				    .detail("ChecksumString", cs);
			} else {
				serializer(ar, type, param2, empty);
			}
		} else if (!isEncrypted() && ar.isSerializing && ar.protocolVersion().hasMutationChecksum() &&
		           CLIENT_KNOBS->ENABLE_MUTATION_CHECKSUM) {
			// serializeWithChecksum(ar, param1, param2);
			uint32_t c = crc32c_append(static_cast<uint32_t>(this->type), param1.begin(), param1.size());
			crc32c_append(c, param2.begin(), param2.size());
			if (this->checksum.present()) {
				// ASSERT_EQ(this->checksum.get(), c);
			} else {
				this->checksum = c;
			}
			StringRef cs = StringRef((uint8_t*)&c, 4);
			uint8_t cType = this->typeWithChecksum();
			Standalone<StringRef> param2WithChecksum = param2.withSuffix(cs);
			StringRef p2 = param2WithChecksum;
			serializer(ar, cType, param1, p2);
			TraceEvent(SevVerbose, "MutationRefChecksum")
			    .detail("CType", cType)
			    .detail("Mutation", this->toString())
			    .detail("Checksum", std::to_string(c))
			    .detail("ChecksumString", cs);
		} else {
			serializer(ar, type, param1, param2);
		}
		if (ar.isDeserializing && withChecksum()) {
			removeChecksum();
		}
		if (ar.isDeserializing && type == ClearRange && param2 == StringRef() && param1 != StringRef()) {
			ASSERT(param1[param1.size() - 1] == '\x00');
			param2 = param1;
			param1 = param2.substr(0, param2.size() - 1);
		}
	}

	// template <class Ar>
	// void serializeWithChecksum(Ar& ar, StringRef cParam1, StringRef cParam2) {
	// 	uint32_t c = crc32c_append(static_cast<uint32_t>(this->type), param1.begin(), param1.size());
	// 	crc32c_append(c, param2.begin(), param2.size());
	// 	if (this->checksum.present()) {
	// 		ASSERT_EQ(this->checksum.get(), c);
	// 	} else {
	// 		this->checksum = c;
	// 	}
	// 	StringRef cs = *(StringRef*)&c;
	// 	serializer(ar, this->typeWithChecksum(), cParam1, cParam2.withSuffix(cs));
	// }

	// An encrypted mutation has type Encrypted, encryption header (which contains encryption metadata) as param1,
	// and the payload as param2. It can be serialize/deserialize as normal mutation, but can only be used after
	// decryption via decrypt().
	bool isEncrypted() const { return type == Encrypted; }

	const BlobCipherEncryptHeader* encryptionHeader() const {
		ASSERT(isEncrypted());
		return reinterpret_cast<const BlobCipherEncryptHeader*>(param1.begin());
	}

	const BlobCipherEncryptHeaderRef configurableEncryptionHeader() const {
		ASSERT(isEncrypted());
		return BlobCipherEncryptHeaderRef::fromStringRef(param1);
	}

	EncryptCipherDomainId encryptDomainId() const {
		ASSERT(isEncrypted());
		return configurableEncryptionHeader().getDomainId();
	}

	void updateEncryptCipherDetails(std::unordered_set<BlobCipherDetails>& cipherDetails) {
		ASSERT(isEncrypted());

		BlobCipherEncryptHeaderRef header = configurableEncryptionHeader();
		EncryptHeaderCipherDetails details = header.getCipherDetails();
		ASSERT(details.textCipherDetails.isValid());
		cipherDetails.insert(details.textCipherDetails);
		if (details.headerCipherDetails.present()) {
			ASSERT(details.headerCipherDetails.get().isValid());
			cipherDetails.insert(details.headerCipherDetails.get());
		}
	}

	MutationRef encrypt(TextAndHeaderCipherKeys cipherKeys,
	                    Arena& arena,
	                    BlobCipherMetrics::UsageType usageType,
	                    double* encryptTime = nullptr) const {
		uint8_t iv[AES_256_IV_LENGTH] = { 0 };
		deterministicRandom()->randomBytes(iv, AES_256_IV_LENGTH);
		BinaryWriter bw(AssumeVersion(ProtocolVersion::withEncryptionAtRest()));
		bw << *this;

		EncryptBlobCipherAes265Ctr cipher(
		    cipherKeys.cipherTextKey,
		    cipherKeys.cipherHeaderKey,
		    iv,
		    AES_256_IV_LENGTH,
		    getEncryptAuthTokenMode(EncryptAuthTokenMode::ENCRYPT_HEADER_AUTH_TOKEN_MODE_SINGLE),
		    usageType);

		StringRef serializedHeader;
		StringRef payload;
		BlobCipherEncryptHeaderRef header;
		payload =
		    cipher.encrypt(static_cast<const uint8_t*>(bw.getData()), bw.getLength(), &header, arena, encryptTime);
		Standalone<StringRef> headerStr = BlobCipherEncryptHeaderRef::toStringRef(header);
		arena.dependsOn(headerStr.arena());
		serializedHeader = headerStr;
		return MutationRef(Encrypted, serializedHeader, payload);
	}

	MutationRef encrypt(const std::unordered_map<EncryptCipherDomainId, Reference<BlobCipherKey>>& cipherKeys,
	                    const EncryptCipherDomainId& domainId,
	                    Arena& arena,
	                    BlobCipherMetrics::UsageType usageType,
	                    double* encryptionTime = nullptr) const {
		ASSERT_NE(domainId, INVALID_ENCRYPT_DOMAIN_ID);
		auto getCipherKey = [&](const EncryptCipherDomainId& domainId) {
			auto iter = cipherKeys.find(domainId);
			ASSERT(iter != cipherKeys.end() && iter->second.isValid());
			return iter->second;
		};
		Reference<BlobCipherKey> textCipherKey = getCipherKey(domainId);
		Reference<BlobCipherKey> headerCipherKey;
		if (FLOW_KNOBS->ENCRYPT_HEADER_AUTH_TOKEN_ENABLED) {
			headerCipherKey = getCipherKey(ENCRYPT_HEADER_DOMAIN_ID);
		}
		uint8_t iv[AES_256_IV_LENGTH] = { 0 };
		deterministicRandom()->randomBytes(iv, AES_256_IV_LENGTH);
		BinaryWriter bw(AssumeVersion(ProtocolVersion::withEncryptionAtRest()));
		bw << *this;

		EncryptBlobCipherAes265Ctr cipher(
		    textCipherKey,
		    headerCipherKey,
		    iv,
		    AES_256_IV_LENGTH,
		    getEncryptAuthTokenMode(EncryptAuthTokenMode::ENCRYPT_HEADER_AUTH_TOKEN_MODE_SINGLE),
		    usageType);

		BlobCipherEncryptHeaderRef header;
		auto payload =
		    cipher.encrypt(static_cast<const uint8_t*>(bw.getData()), bw.getLength(), &header, arena, encryptionTime);
		Standalone<StringRef> serializedHeader = BlobCipherEncryptHeaderRef::toStringRef(header);
		arena.dependsOn(serializedHeader.arena());
		return MutationRef(Encrypted, serializedHeader, payload);
	}

	MutationRef encryptMetadata(const std::unordered_map<EncryptCipherDomainId, Reference<BlobCipherKey>>& cipherKeys,
	                            Arena& arena,
	                            BlobCipherMetrics::UsageType usageType,
	                            double* encryptionTime = nullptr) const {
		return encrypt(cipherKeys, SYSTEM_KEYSPACE_ENCRYPT_DOMAIN_ID, arena, usageType, encryptionTime);
	}

	MutationRef decrypt(TextAndHeaderCipherKeys cipherKeys,
	                    Arena& arena,
	                    BlobCipherMetrics::UsageType usageType,
	                    StringRef* buf = nullptr,
	                    double* decryptTime = nullptr) const {
		StringRef plaintext;
		const BlobCipherEncryptHeaderRef header = configurableEncryptionHeader();
		DecryptBlobCipherAes256Ctr cipher(
		    cipherKeys.cipherTextKey, cipherKeys.cipherHeaderKey, header.getIV(), usageType);
		plaintext = cipher.decrypt(param2.begin(), param2.size(), header, arena, decryptTime);
		if (buf != nullptr) {
			*buf = plaintext;
		}
		ArenaReader reader(arena, plaintext, AssumeVersion(ProtocolVersion::withEncryptionAtRest()));
		MutationRef mutation;
		reader >> mutation;
		return mutation;
	}

	MutationRef decrypt(const std::unordered_map<BlobCipherDetails, Reference<BlobCipherKey>>& cipherKeys,
	                    Arena& arena,
	                    BlobCipherMetrics::UsageType usageType,
	                    StringRef* buf = nullptr,
	                    double* decryptTime = nullptr) const {
		TextAndHeaderCipherKeys textAndHeaderKeys = getCipherKeys(cipherKeys);
		return decrypt(textAndHeaderKeys, arena, usageType, buf, decryptTime);
	}

	TextAndHeaderCipherKeys getCipherKeys(
	    const std::unordered_map<BlobCipherDetails, Reference<BlobCipherKey>>& cipherKeys) const {
		auto getCipherKey = [&](const BlobCipherDetails& details) -> Reference<BlobCipherKey> {
			if (!details.isValid()) {
				return {};
			}
			auto iter = cipherKeys.find(details);
			ASSERT(iter != cipherKeys.end() && iter->second.isValid());
			return iter->second;
		};
		TextAndHeaderCipherKeys textAndHeaderKeys;
		const BlobCipherEncryptHeaderRef header = configurableEncryptionHeader();
		EncryptHeaderCipherDetails cipherDetails = header.getCipherDetails();
		ASSERT(cipherDetails.textCipherDetails.isValid());
		textAndHeaderKeys.cipherTextKey = getCipherKey(cipherDetails.textCipherDetails);
		if (cipherDetails.headerCipherDetails.present()) {
			ASSERT(cipherDetails.headerCipherDetails.get().isValid());
			textAndHeaderKeys.cipherHeaderKey = getCipherKey(cipherDetails.headerCipherDetails.get());
		} else {
			ASSERT(!FLOW_KNOBS->ENCRYPT_HEADER_AUTH_TOKEN_ENABLED);
		}
		return textAndHeaderKeys;
	}

	// These masks define which mutation types have particular properties (they are used to implement
	// isSingleKeyMutation() etc)
	enum {
		ATOMIC_MASK = (1 << AddValue) | (1 << And) | (1 << Or) | (1 << Xor) | (1 << AppendIfFits) | (1 << Max) |
		              (1 << Min) | (1 << SetVersionstampedKey) | (1 << SetVersionstampedValue) | (1 << ByteMin) |
		              (1 << ByteMax) | (1 << MinV2) | (1 << AndV2) | (1 << CompareAndClear),
		SINGLE_KEY_MASK = ATOMIC_MASK | (1 << SetValue),
		NON_ASSOCIATIVE_MASK = (1 << AddValue) | (1 << Or) | (1 << Xor) | (1 << Max) | (1 << Min) |
		                       (1 << SetVersionstampedKey) | (1 << SetVersionstampedValue) | (1 << MinV2) |
		                       (1 << CompareAndClear)
	};
};

template <>
struct Traceable<MutationRef> : std::true_type {
	static std::string toString(MutationRef const& value) { return value.toString(); }
};

static inline std::string getTypeString(MutationRef::Type type) {
	return type < MutationRef::MAX_ATOMIC_OP ? typeString[(int)type] : "Unset";
}

static inline std::string getTypeString(uint8_t type) {
	return type < MutationRef::MAX_ATOMIC_OP ? typeString[type] : "Unset";
}

// A 'single key mutation' is one which affects exactly the value of the key specified by its param1
static inline bool isSingleKeyMutation(MutationRef::Type type) {
	return (MutationRef::SINGLE_KEY_MASK & (1 << type)) != 0;
}

// Returns true if the given type can be safely cast to MutationRef::Type and used as a parameter to
// isSingleKeyMutation, isAtomicOp, etc.  It does NOT mean that the type is a valid type of a MutationRef in any
// particular context.
static inline bool isValidMutationType(uint32_t type) {
	return (type < MutationRef::MAX_ATOMIC_OP);
}

// An 'atomic operation' is a single key mutation which sets the key specified by its param1 to a
//   nontrivial function of the previous value of the key and param2, and thus requires a
//   read/modify/write to implement.  (Basically a single key mutation other than a set)
static inline bool isAtomicOp(MutationRef::Type mutationType) {
	return (MutationRef::ATOMIC_MASK & (1 << mutationType)) != 0;
}

// Returns true for operations which do not obey the associative law (i.e. a*(b*c) == (a*b)*c) in all cases
// unless a, b, and c have equal lengths, in which case even these operations are associative.
static inline bool isNonAssociativeOp(MutationRef::Type mutationType) {
	return (MutationRef::NON_ASSOCIATIVE_MASK & (1 << mutationType)) != 0;
}

struct CommitTransactionRef {
	CommitTransactionRef() = default;
	CommitTransactionRef(Arena& a, const CommitTransactionRef& from)
	  : read_conflict_ranges(a, from.read_conflict_ranges), write_conflict_ranges(a, from.write_conflict_ranges),
	    mutations(a, from.mutations), read_snapshot(from.read_snapshot),
	    report_conflicting_keys(from.report_conflicting_keys), lock_aware(from.lock_aware),
	    spanContext(from.spanContext) {}

	VectorRef<KeyRangeRef> read_conflict_ranges;
	VectorRef<KeyRangeRef> write_conflict_ranges;
	VectorRef<MutationRef> mutations; // metadata mutations
	// encryptedMutations should be a 1-1 corespondence with mutations field above. That is either
	// encryptedMutations.size() == 0 or encryptedMutations.size() == mutations.size() and encryptedMutations[i] =
	// mutations[i].encrypt(). Currently this field is not serialized so clients should NOT set this field during a
	// usual commit path. It is currently only used during backup mutation log restores.
	VectorRef<Optional<MutationRef>> encryptedMutations;
	Version read_snapshot = 0;
	bool report_conflicting_keys = false;
	bool lock_aware = false; // set when metadata mutations are present
	Optional<SpanContext> spanContext;

	// set by Commit Proxy
	// The tenants associated with this transaction. This field only existing
	// when tenant mode is required and this transaction has metadata mutations
	Optional<VectorRef<int64_t>> tenantIds;

	template <class Ar>
	force_inline void serialize(Ar& ar) {
		if constexpr (is_fb_function<Ar>) {
			serializer(ar,
			           read_conflict_ranges,
			           write_conflict_ranges,
			           mutations,
			           read_snapshot,
			           report_conflicting_keys,
			           lock_aware,
			           spanContext,
			           tenantIds);
		} else {
			serializer(ar, read_conflict_ranges, write_conflict_ranges, mutations, read_snapshot);
			if (ar.protocolVersion().hasReportConflictingKeys()) {
				serializer(ar, report_conflicting_keys);
			}
			if (ar.protocolVersion().hasResolverPrivateMutations()) {
				serializer(ar, lock_aware);
				if (!ar.protocolVersion().hasOTELSpanContext()) {
					Optional<UID> context;
					serializer(ar, context);
					if (context.present()) {
						SpanContext res;
						res.traceID = context.get();
						spanContext = res;
					}
				}
			}
			if (ar.protocolVersion().hasOTELSpanContext()) {
				serializer(ar, spanContext);
			}
		}
	}

	// Convenience for internal code required to manipulate these without the Native API
	void set(Arena& arena, KeyRef const& key, ValueRef const& value) {
		mutations.push_back_deep(arena, MutationRef(MutationRef::SetValue, key, value));
		write_conflict_ranges.push_back(arena, singleKeyRange(key, arena));
	}

	void clear(Arena& arena, KeyRangeRef const& keys) {
		mutations.push_back_deep(arena, MutationRef(MutationRef::ClearRange, keys.begin, keys.end));
		write_conflict_ranges.push_back_deep(arena, keys);
	}

	size_t expectedSize() const {
		return read_conflict_ranges.expectedSize() + write_conflict_ranges.expectedSize() + mutations.expectedSize();
	}
};

struct MutationsAndVersionRef {
	VectorRef<MutationRef> mutations;
	Version version = invalidVersion;
	Version knownCommittedVersion = invalidVersion;

	MutationsAndVersionRef() {}
	explicit MutationsAndVersionRef(Version version, Version knownCommittedVersion)
	  : version(version), knownCommittedVersion(knownCommittedVersion) {}
	MutationsAndVersionRef(VectorRef<MutationRef> mutations, Version version, Version knownCommittedVersion)
	  : mutations(mutations), version(version), knownCommittedVersion(knownCommittedVersion) {}
	MutationsAndVersionRef(Arena& to, VectorRef<MutationRef> mutations, Version version, Version knownCommittedVersion)
	  : mutations(to, mutations), version(version), knownCommittedVersion(knownCommittedVersion) {}
	MutationsAndVersionRef(Arena& to, const MutationsAndVersionRef& from)
	  : mutations(to, from.mutations), version(from.version), knownCommittedVersion(from.knownCommittedVersion) {}
	int expectedSize() const { return mutations.expectedSize(); }

	struct OrderByVersion {
		bool operator()(MutationsAndVersionRef const& a, MutationsAndVersionRef const& b) const {
			return a.version < b.version;
		}
	};

	template <class Ar>
	void serialize(Ar& ar) {
		serializer(ar, mutations, version, knownCommittedVersion);
	}
};

struct MutationRefAndCipherKeys {
	MutationRef mutation;
	TextAndHeaderCipherKeys cipherKeys;
};

struct EncryptedMutationsAndVersionRef {
	VectorRef<MutationRef> mutations;
	Optional<VectorRef<MutationRef>> encrypted;
	std::vector<TextAndHeaderCipherKeys> cipherKeys;
	Version version = invalidVersion;
	Version knownCommittedVersion = invalidVersion;

	EncryptedMutationsAndVersionRef() {}
	explicit EncryptedMutationsAndVersionRef(Version version, Version knownCommittedVersion)
	  : version(version), knownCommittedVersion(knownCommittedVersion) {}
	EncryptedMutationsAndVersionRef(VectorRef<MutationRef> mutations,
	                                VectorRef<MutationRef> encrypted,
	                                const std::vector<TextAndHeaderCipherKeys>& cipherKeys,
	                                Version version,
	                                Version knownCommittedVersion)
	  : mutations(mutations), encrypted(encrypted), cipherKeys(cipherKeys), version(version),
	    knownCommittedVersion(knownCommittedVersion) {}
	EncryptedMutationsAndVersionRef(Arena& to,
	                                VectorRef<MutationRef> mutations,
	                                Optional<VectorRef<MutationRef>> encrypt,
	                                const std::vector<TextAndHeaderCipherKeys>& cipherKeys,
	                                Version version,
	                                Version knownCommittedVersion)
	  : mutations(to, mutations), cipherKeys(cipherKeys), version(version),
	    knownCommittedVersion(knownCommittedVersion) {
		if (encrypt.present()) {
			encrypted = VectorRef<MutationRef>(to, encrypt.get());
		}
	}
	EncryptedMutationsAndVersionRef(Arena& to, const EncryptedMutationsAndVersionRef& from)
	  : mutations(to, from.mutations), cipherKeys(from.cipherKeys), version(from.version),
	    knownCommittedVersion(from.knownCommittedVersion) {
		if (from.encrypted.present()) {
			encrypted = VectorRef<MutationRef>(to, from.encrypted.get());
		}
	}
	int expectedSize() const { return mutations.expectedSize(); }

	struct OrderByVersion {
		bool operator()(EncryptedMutationsAndVersionRef const& a, EncryptedMutationsAndVersionRef const& b) const {
			return a.version < b.version;
		}
	};
};

TEST_CASE("noSim/CommitTransaction/MutationRef") {
	printf("testing MutationRef encoding/decoding\n");
	MutationRef m(MutationRef::SetValue, "TestKey"_sr, "TestValue"_sr);
	// BinaryWriter wr(IncludeVersion(ProtocolVersion::withGcTxnGenerations()));
	BinaryWriter wr(AssumeVersion(ProtocolVersion::withMutationChecksum()));

	wr << m;

	Standalone<StringRef> value = wr.toValue();
	TraceEvent("EncodedMutation").detail("RawBytes", value);

	BinaryReader rd(value, AssumeVersion(ProtocolVersion::withBlobGranule()));
	// BinaryReader rd(value, IncludeVersion());
	Standalone<MutationRef> de;

	rd >> de;

	printf("Deserialized mutation: %s\n", de.toString().c_str());
	printf("testing data move ID encoding/decoding complete\n");

	return Void();
}

#endif
