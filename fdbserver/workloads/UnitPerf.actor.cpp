/*
 * UnitPerf.actor.cpp
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

#include "fdbrpc/ActorFuzz.h"
#include "fdbserver/TesterInterface.h"
#include "workloads.h"

ACTOR Future<Void> sleepyActor(double interval, int* counter) {
	loop {
		wait(delay(interval));
		++*counter;
	}
}

ACTOR Future<Void> unitPerfTest() {
	printf("\n");

	state int counter = 0;
	state vector<Future<Void>> sleepy;
	for (int i = 0; i < 100000; i++) sleepy.push_back(sleepyActor(.1, &counter));

	wait(delay(10));
	sleepy.clear();
	TraceEvent("Completed").detail("Count", counter);
	printf("Completed: %d\n", counter);

	printf("\n");
	return Void();
}

struct UnitPerfWorkload : TestWorkload {
	bool enabled;

	UnitPerfWorkload(WorkloadContext const& wcx) : TestWorkload(wcx) {
		enabled = !clientId; // only do this on the "first" client
	}

	virtual std::string description() { return "UnitPerfWorkload"; }
	virtual Future<Void> setup(Database const& cx) { return Void(); }
	virtual Future<Void> start(Database const& cx) {
		if (enabled) return unitPerfTest();
		return Void();
	}
	virtual Future<bool> check(Database const& cx) { return true; }
	virtual void getMetrics(vector<PerfMetric>& m) {}
};

WorkloadFactory<UnitPerfWorkload> UnitPerfWorkloadFactory("UnitPerf");