/*
 * IdempotencyIdsCommand.actor.cpp
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

#include "fdbcli/fdbcli.actor.h"
#include "fdbclient/IdempotencyId.actor.h"
#include "fdbclient/JsonBuilder.h"
#include "fdbclient/json_spirit/json_spirit_reader_template.h"
#include "flow/actorcompiler.h" // This must be the last include

namespace {

constexpr auto usage = "idempotencyids [status | clear <min_age_seconds>]";

bool exitFailure() {
	fmt::print(usage);
	return false;
}

Optional<double> parseAgeValue(StringRef token) {
	try {
		return std::stod(token.toString());
	} catch (...) {
		return {};
	}
}

} // namespace

namespace fdb_cli {

ACTOR Future<JsonBuilderObject> getIdmpKeyStatus(Database db) {
	JsonBuilderObject out;
	out["bytes"] = 100;
	out["new"] = 5;
	return out;
}

ACTOR Future<bool> idempotencyIdsCommandActor(Database db, std::vector<StringRef> tokens) {
	if (tokens.size() < 2 || tokens.size() > 3) {
		return exitFailure();
	} else {
		auto const action = tokens[1];
		if (action == "status"_sr) {
			if (tokens.size() != 2) {
				return exitFailure();
			}
			JsonBuilderObject status = wait(getIdmpKeyStatus(db));
			fmt::print("{}\n", status.getJson());
			return true;
		} else if (action == "clear"_sr) {
			if (tokens.size() != 3) {
				return exitFailure();
			}
			auto const age = parseAgeValue(tokens[2]);
			if (!age.present()) {
				return exitFailure();
			}
			wait(cleanIdempotencyIds(db, age.get()));
			fmt::print("Successfully cleared idempotency IDs.\n");
			return true;
		} else {
			return exitFailure();
		}
	}
}

} // namespace fdb_cli
