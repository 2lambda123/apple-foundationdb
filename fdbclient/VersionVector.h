/*
 * VersionVector.h
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

#ifndef FDBCLIENT_VERSION_VECTOR_H
#define FDBCLIENT_VERSION_VECTOR_H

#pragma once

#include <unordered_map>

#include "fdbclient/FDBTypes.h"

struct VersionVector {
	std::unordered_map<Tag, Version> versions;
	Version maxVersion; // Specifies the max version in this version vector. (Note:
	                    // there may or may not be a corresponding entry for this
	                    // version in the "versions" map.)

	VersionVector() : maxVersion(invalidVersion) {}
	VersionVector(Version version) : maxVersion(version) {}

	void setVersion(const Tag& tag, Version version) {
		ASSERT(tag != invalidTag);
		ASSERT(version > maxVersion);
		versions[tag] = version;
		maxVersion = version;
	}

	bool hasVersion(const Tag& tag) const {
		ASSERT(tag != invalidTag);
		return versions.find(tag) != versions.end();
	}

	// @pre assumes that the given tag has an entry in the version vector.
	Version getVersion(const Tag& tag) const {
		ASSERT(tag != invalidTag);
		auto iter = versions.find(tag);
		ASSERT(iter != versions.end());
		return iter->second;
	}

	bool operator==(const VersionVector& vv) const { return maxVersion == vv.maxVersion; }
	bool operator!=(const VersionVector& vv) const { return maxVersion != vv.maxVersion; }
	bool operator<(const VersionVector& vv) const { return maxVersion < vv.maxVersion; }

	template <class Ar>
	void serialize(Ar& ar) {
		serializer(ar, versions, maxVersion);
	}
};

static const VersionVector minVersionVector{ 0 };
static const VersionVector maxVersionVector{ MAX_VERSION };
static const VersionVector invalidVersionVector{ invalidVersion };

#endif /* FDBCLIENT_VERSION_VECTOR_H */