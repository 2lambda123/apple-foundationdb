/*
 * MasterData.actor.h
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

#pragma once

#include <map>

#include "fdbrpc/sim_validation.h"
#include "fdbserver/CoordinatedState.h"
#include "fdbserver/CoordinationInterface.h" // copy constructors for ServerCoordinators class
#include "fdbserver/Knobs.h"
#include "fdbserver/MasterInterface.h"
#include "fdbserver/ResolutionBalancer.actor.h"
#include "fdbserver/ServerDBInfo.h"
#include "flow/ActorCollection.h"
#include "flow/Trace.h"
#include "flow/swift_compat.h"
#include "fdbclient/VersionVector.h"

// When actually compiled (NO_INTELLISENSE), include the generated version of this file.  In intellisense use the source
// version.
#if defined(NO_INTELLISENSE) && !defined(FDBSERVER_MASTERDATA_ACTOR_G_H)
#define FDBSERVER_MASTERDATA_ACTOR_G_H
#include "fdbserver/MasterData.actor.g.h"
#elif !defined(FDBSERVER_MASTERDATA_ACTOR_H)
#define FDBSERVER_MASTERDATA_ACTOR_H
#include "flow/actorcompiler.h" // This must be the last #include

struct MasterData : NonCopyable, ReferenceCounted<MasterData> {
    UID dbgid;

    Version lastEpochEnd, // The last version in the old epoch not (to be) rolled back in this recovery
        recoveryTransactionVersion; // The first version in this epoch

    NotifiedVersion prevTLogVersion; // Order of transactions to tlogs

    NotifiedVersion liveCommittedVersion; // The largest live committed version reported by commit proxies.
    bool databaseLocked;
    Optional<Value> proxyMetadataVersion;
    Version minKnownCommittedVersion;

    ServerCoordinators coordinators;

    Version version; // The last version assigned to a proxy by getVersion()
    double lastVersionTime;
    Optional<Version> referenceVersion;

    std::map<UID, CommitProxyVersionReplies> lastCommitProxyVersionReplies;

    MasterInterface myInterface;

    ResolutionBalancer resolutionBalancer;

    bool forceRecovery;

    // Captures the latest commit version targeted for each storage server in the cluster.
    // @todo We need to ensure that the latest commit versions of storage servers stay
    // up-to-date in the presence of key range splits/merges.
    VersionVector ssVersionVector;

    int8_t locality; // sequencer locality

    CounterCollection cc;
    Counter getCommitVersionRequests;
    Counter getLiveCommittedVersionRequests;
    Counter reportLiveCommittedVersionRequests;
    // This counter gives an estimate of the number of non-empty peeks that storage servers
    // should do from tlogs (in the worst case, ignoring blocking peek timeouts).
    LatencySample versionVectorTagUpdates;
    Counter waitForPrevCommitRequests;
    Counter nonWaitForPrevCommitRequests;
    LatencySample versionVectorSizeOnCVReply;
    LatencySample waitForPrevLatencies;

    PromiseStream<Future<Void>> addActor;

    Future<Void> logger;
    Future<Void> balancer;

    MasterData(Reference<AsyncVar<ServerDBInfo> const> const& dbInfo,
               MasterInterface const& myInterface,
               ServerCoordinators const& coordinators,
               ClusterControllerFullInterface const& clusterController,
               Standalone<StringRef> const& dbId,
               PromiseStream<Future<Void>> addActor,
               bool forceRecovery)
      : dbgid(myInterface.id()),
      lastEpochEnd(invalidVersion),
      recoveryTransactionVersion(invalidVersion),
        liveCommittedVersion(invalidVersion),
      databaseLocked(false),
      minKnownCommittedVersion(invalidVersion),
        coordinators(coordinators),
      version(invalidVersion),
      lastVersionTime(0),
      myInterface(myInterface),
        resolutionBalancer(&version),
      forceRecovery(forceRecovery),
      cc("Master", dbgid.toString()),
        getCommitVersionRequests("GetCommitVersionRequests", cc),
        getLiveCommittedVersionRequests("GetLiveCommittedVersionRequests", cc),
        reportLiveCommittedVersionRequests("ReportLiveCommittedVersionRequests", cc),
        versionVectorTagUpdates("VersionVectorTagUpdates",
                                dbgid,
                                SERVER_KNOBS->LATENCY_METRICS_LOGGING_INTERVAL,
                                SERVER_KNOBS->LATENCY_SAMPLE_SIZE),
        waitForPrevCommitRequests("WaitForPrevCommitRequests", cc),
        nonWaitForPrevCommitRequests("NonWaitForPrevCommitRequests", cc),
        versionVectorSizeOnCVReply("VersionVectorSizeOnCVReply",
                                   dbgid,
                                   SERVER_KNOBS->LATENCY_METRICS_LOGGING_INTERVAL,
                                   SERVER_KNOBS->LATENCY_SAMPLE_SIZE),
        waitForPrevLatencies("WaitForPrevLatencies",
                             dbgid,
                             SERVER_KNOBS->LATENCY_METRICS_LOGGING_INTERVAL,
                             SERVER_KNOBS->LATENCY_SAMPLE_SIZE),
        addActor(addActor) {
        logger = traceCounters("MasterMetrics", dbgid, SERVER_KNOBS->WORKER_LOGGING_INTERVAL, &cc, "MasterMetrics");
        if (forceRecovery && !myInterface.locality.dcId().present()) {
            TraceEvent(SevError, "ForcedRecoveryRequiresDcID").log();
            forceRecovery = false;
        }
        balancer = resolutionBalancer.resolutionBalancing();
        locality = tagLocalityInvalid;
    }

    ~MasterData() = default;

    inline ResolutionBalancer &getResolutionBalancer() {
        return resolutionBalancer;
    }
    inline Counter &getGetCommitVersionRequests() {
        return getCommitVersionRequests;
    }
};

using ReferenceMasterData = Reference<MasterData>;

// FIXME: Workaround for linker issue (rdar://101092732).
void swift_workaround_setLatestRequestNumber(NotifiedVersion &latestRequestNum,
                                             Version v);

// FIXME: Workaround for issue with FRT type layout (rdar://101092361).
struct MasterDataSwiftReference {
    MasterData &myself;

    MasterDataSwiftReference(MasterData &myself);

    Counter &getGetCommitVersionRequests() const __attribute__((swift_attr("import_unsafe")));

    Version getVersion() const;
    void setVersion(Version v);

    double getLastVersionTime() const;
    void setLastVersionTime(double v);

    Version getRecoveryTransactionVersion() const;

    Version getLastEpochEnd() const;

    using OptionalVersion = Optional<Version>;
    Optional<Version> getReferenceVersion() const;

    ResolutionBalancer &getResolutionBalancer() const __attribute__((swift_attr("import_unsafe")));
};

// FIXME: Workaround for runtime issue #1 (rdar://101092612).
CommitProxyVersionReplies *_Nullable swift_lookup_Map_UID_CommitProxyVersionReplies(MasterDataSwiftReference rd, UID value);

#include "flow/unactorcompiler.h"
#endif
