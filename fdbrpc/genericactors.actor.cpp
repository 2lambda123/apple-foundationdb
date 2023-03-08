/*
 * genericactors.actor.cpp
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

#include "flow/flow.h"
#include "fdbrpc/genericactors.actor.h" // Gets genericactors.actor.g.h indirectly
#include "flow/network.h"
#include "fdbrpc/simulator.h"
#include "flow/actorcompiler.h"

void enableConnectionFailures(std::string const& context) {
	if (g_network->isSimulated()) {
		g_simulator.connectionFailuresDisableDuration = 0;
		g_simulator.speedUpSimulation = false;
		TraceEvent(SevWarnAlways, ("EnableConnectionFailures_" + context).c_str());
	}
}

void disableConnectionFailures(std::string const& context) {
	if (g_network->isSimulated()) {
		g_simulator.connectionFailuresDisableDuration = 1e6;
		g_simulator.speedUpSimulation = true;
		TraceEvent(SevWarnAlways, ("DisableConnectionFailures_" + context).c_str());
	}
}

ACTOR Future<Void> disableConnectionFailuresAfter(double seconds, std::string context) {
	if (g_network->isSimulated()) {
		TraceEvent(SevWarnAlways, ("DisableConnectionFailures_" + context).c_str()).detail("At", now() + seconds);
		wait(delay(seconds));
		g_simulator.connectionFailuresDisableDuration = 1e6;
		g_simulator.speedUpSimulation = true;
		TraceEvent(SevWarnAlways, ("DisableConnectionFailures_" + context).c_str());
	}
	return Void();
}
