/*
 * flow.cpp
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

#include "flow/flow.h"
#include "fdbserver/RestoreInterface.h"
#include "fdbserver/NetworkTest.h"
#include "fdbserver/WorkerInterface.actor.h"
#include "fdbserver/MasterInterface.h"
#include "fdbserver/ClusterRecruitmentInterface.h"
#include "flow/serialize.h"
#include "flow/SerializeImpl.h"

template struct ObjectSerializedMsg<CandidacyRequest>;
template struct ObjectSerializedMsg<ChangeCoordinatorsRequest>;
template struct ObjectSerializedMsg<CoordinationPingMessage>;
template struct ObjectSerializedMsg<DiskStoreRequest>;
template struct ObjectSerializedMsg<DistributorSnapRequest>;
template struct ObjectSerializedMsg<ErrorOr<EnsureTable<DataDistributorInterface>>>;
template struct ObjectSerializedMsg<ErrorOr<EnsureTable<GenerationRegReadReply>>>;
template struct ObjectSerializedMsg<ErrorOr<EnsureTable<GetCommitVersionReply>>>;
template struct ObjectSerializedMsg<ErrorOr<EnsureTable<GetRateInfoReply>>>;
template struct ObjectSerializedMsg<ErrorOr<EnsureTable<GetStorageServerRejoinInfoReply>>>;
template struct ObjectSerializedMsg<ErrorOr<EnsureTable<InitializeStorageReply>>>;
template struct ObjectSerializedMsg<ErrorOr<EnsureTable<LoadedReply>>>;
template struct ObjectSerializedMsg<ErrorOr<EnsureTable<MasterInterface>>>;
template struct ObjectSerializedMsg<ErrorOr<EnsureTable<NetworkTestReply>>>;
template struct ObjectSerializedMsg<ErrorOr<EnsureTable<RatekeeperInterface>>>;
template struct ObjectSerializedMsg<ErrorOr<EnsureTable<RecruitFromConfigurationReply>>>;
template struct ObjectSerializedMsg<ErrorOr<EnsureTable<RecruitRemoteFromConfigurationReply>>>;
template struct ObjectSerializedMsg<ErrorOr<EnsureTable<RecruitStorageReply>>>;
template struct ObjectSerializedMsg<ErrorOr<EnsureTable<RegisterWorkerReply>>>;
template struct ObjectSerializedMsg<ErrorOr<EnsureTable<ResolutionSplitReply>>>;
template struct ObjectSerializedMsg<ErrorOr<EnsureTable<ResolveTransactionBatchReply>>>;
template struct ObjectSerializedMsg<ErrorOr<EnsureTable<ResolverInterface>>>;
template struct ObjectSerializedMsg<ErrorOr<EnsureTable<ServerDBInfo>>>;
template struct ObjectSerializedMsg<ErrorOr<EnsureTable<Standalone<VectorRef<UID, (VecSerStrategy)0>>>>>;
template struct ObjectSerializedMsg<ErrorOr<EnsureTable<std::vector<PerfMetric>>>>;
template struct ObjectSerializedMsg<ErrorOr<EnsureTable<std::vector<WorkerDetails>>>>;
template struct ObjectSerializedMsg<ErrorOr<EnsureTable<TLogInterface>>>;
template struct ObjectSerializedMsg<ErrorOr<EnsureTable<TLogLockResult>>>;
template struct ObjectSerializedMsg<ErrorOr<EnsureTable<TLogPeekReply>>>;
template struct ObjectSerializedMsg<ErrorOr<EnsureTable<TLogQueuingMetricsReply>>>;
template struct ObjectSerializedMsg<ErrorOr<EnsureTable<TestReply>>>;
template struct ObjectSerializedMsg<ErrorOr<EnsureTable<TraceEventFields>>>;
template struct ObjectSerializedMsg<ErrorOr<EnsureTable<UniqueGeneration>>>;
template struct ObjectSerializedMsg<ErrorOr<EnsureTable<WorkloadInterface>>>;
template struct ObjectSerializedMsg<EventLogRequest>;
template struct ObjectSerializedMsg<ExecuteRequest>;
template struct ObjectSerializedMsg<ForwardRequest>;
template struct ObjectSerializedMsg<GenerationRegReadRequest>;
template struct ObjectSerializedMsg<GenerationRegWriteRequest>;
template struct ObjectSerializedMsg<GetCommitVersionRequest>;
template struct ObjectSerializedMsg<GetRateInfoRequest>;
template struct ObjectSerializedMsg<GetServerDBInfoRequest>;
template struct ObjectSerializedMsg<GetWorkersRequest>;
template struct ObjectSerializedMsg<HaltDataDistributorRequest>;
template struct ObjectSerializedMsg<HaltRatekeeperRequest>;
template struct ObjectSerializedMsg<InitializeDataDistributorRequest>;
template struct ObjectSerializedMsg<InitializeLogRouterRequest>;
template struct ObjectSerializedMsg<InitializeMasterProxyRequest>;
template struct ObjectSerializedMsg<InitializeRatekeeperRequest>;
template struct ObjectSerializedMsg<InitializeResolverRequest>;
template struct ObjectSerializedMsg<InitializeStorageRequest>;
template struct ObjectSerializedMsg<InitializeTLogRequest>;
template struct ObjectSerializedMsg<LeaderHeartbeatRequest>;
template struct ObjectSerializedMsg<LoadedPingRequest>;
template struct ObjectSerializedMsg<NetworkTestRequest>;
template struct ObjectSerializedMsg<RecruitFromConfigurationRequest>;
template struct ObjectSerializedMsg<RecruitMasterRequest>;
template struct ObjectSerializedMsg<RecruitRemoteFromConfigurationRequest>;
template struct ObjectSerializedMsg<RecruitStorageRequest>;
template struct ObjectSerializedMsg<RegisterMasterRequest>;
template struct ObjectSerializedMsg<RegisterWorkerRequest>;
template struct ObjectSerializedMsg<ReplyPromise<std::vector<PerfMetric>>>;
template struct ObjectSerializedMsg<ReplyPromise<TLogLockResult>>;
template struct ObjectSerializedMsg<ResolutionMetricsRequest>;
template struct ObjectSerializedMsg<ResolutionSplitRequest>;
template struct ObjectSerializedMsg<ResolveTransactionBatchRequest>;
template struct ObjectSerializedMsg<SetMetricsLogRateRequest>;
template struct ObjectSerializedMsg<TLogCommitRequest>;
template struct ObjectSerializedMsg<TLogConfirmRunningRequest>;
template struct ObjectSerializedMsg<TLogDisablePopRequest>;
template struct ObjectSerializedMsg<TLogEnablePopRequest>;
template struct ObjectSerializedMsg<TLogPeekRequest>;
template struct ObjectSerializedMsg<TLogPopRequest>;
template struct ObjectSerializedMsg<TLogQueuingMetricsRequest>;
template struct ObjectSerializedMsg<TLogRecoveryFinishedRequest>;
template struct ObjectSerializedMsg<TLogRejoinRequest>;
template struct ObjectSerializedMsg<TLogSnapRequest>;
template struct ObjectSerializedMsg<TestRequest>;
template struct ObjectSerializedMsg<TraceBatchDumpRequest>;
template struct ObjectSerializedMsg<WorkerSnapRequest>;
template struct ObjectSerializedMsg<WorkloadRequest>;
template struct SerializedMsg<ArenaReader, CandidacyRequest>;
template struct SerializedMsg<ArenaReader, ChangeCoordinatorsRequest>;
template struct SerializedMsg<ArenaReader, CoordinationPingMessage>;
template struct SerializedMsg<ArenaReader, DiskStoreRequest>;
template struct SerializedMsg<ArenaReader, DistributorSnapRequest>;
template struct SerializedMsg<ArenaReader, ExecuteRequest>;
template struct SerializedMsg<ArenaReader, EventLogRequest>;
template struct SerializedMsg<ArenaReader, ForwardRequest>;
template struct SerializedMsg<ArenaReader, GenerationRegReadRequest>;
template struct SerializedMsg<ArenaReader, GenerationRegWriteRequest>;
template struct SerializedMsg<ArenaReader, GetCommitVersionRequest>;
template struct SerializedMsg<ArenaReader, GetRateInfoRequest>;
template struct SerializedMsg<ArenaReader, GetServerDBInfoRequest>;
template struct SerializedMsg<ArenaReader, GetWorkersRequest>;
template struct SerializedMsg<ArenaReader, HaltDataDistributorRequest>;
template struct SerializedMsg<ArenaReader, HaltRatekeeperRequest>;
template struct SerializedMsg<ArenaReader, InitializeDataDistributorRequest>;
template struct SerializedMsg<ArenaReader, InitializeLogRouterRequest>;
template struct SerializedMsg<ArenaReader, InitializeMasterProxyRequest>;
template struct SerializedMsg<ArenaReader, InitializeRatekeeperRequest>;
template struct SerializedMsg<ArenaReader, InitializeResolverRequest>;
template struct SerializedMsg<ArenaReader, InitializeStorageRequest>;
template struct SerializedMsg<ArenaReader, InitializeTLogRequest>;
template struct SerializedMsg<ArenaReader, LeaderHeartbeatRequest>;
template struct SerializedMsg<ArenaReader, LoadedPingRequest>;
template struct SerializedMsg<ArenaReader, NetworkTestRequest>;
template struct SerializedMsg<ArenaReader, RecruitFromConfigurationRequest>;
template struct SerializedMsg<ArenaReader, RecruitMasterRequest>;
template struct SerializedMsg<ArenaReader, RecruitRemoteFromConfigurationRequest>;
template struct SerializedMsg<ArenaReader, RecruitStorageRequest>;
template struct SerializedMsg<ArenaReader, RegisterMasterRequest>;
template struct SerializedMsg<ArenaReader, RegisterWorkerRequest>;
template struct SerializedMsg<ArenaReader, ReplyPromise<TLogLockResult>>;
template struct SerializedMsg<ArenaReader, ResolutionMetricsRequest>;
template struct SerializedMsg<ArenaReader, ResolutionSplitRequest>;
template struct SerializedMsg<ArenaReader, ResolveTransactionBatchRequest>;
template struct SerializedMsg<ArenaReader, SetMetricsLogRateRequest>;
template struct SerializedMsg<ArenaReader, TestRequest>;
template struct SerializedMsg<ArenaReader, TLogCommitRequest>;
template struct SerializedMsg<ArenaReader, TLogConfirmRunningRequest>;
template struct SerializedMsg<ArenaReader, TLogDisablePopRequest>;
template struct SerializedMsg<ArenaReader, TLogEnablePopRequest>;
template struct SerializedMsg<ArenaReader, TLogPeekRequest>;
template struct SerializedMsg<ArenaReader, TLogPopRequest>;
template struct SerializedMsg<ArenaReader, TLogQueuingMetricsRequest>;
template struct SerializedMsg<ArenaReader, TLogRecoveryFinishedRequest>;
template struct SerializedMsg<ArenaReader, TLogRejoinRequest>;
template struct SerializedMsg<ArenaReader, TLogSnapRequest>;
template struct SerializedMsg<ArenaReader, TraceBatchDumpRequest>;
template struct SerializedMsg<ArenaReader, WorkerSnapRequest>;
template struct SerializedMsg<ArenaReader, WorkloadRequest>;
template struct SerializedMsg<BinaryWriter, CandidacyRequest>;
template struct SerializedMsg<BinaryWriter, ChangeCoordinatorsRequest>;
template struct SerializedMsg<BinaryWriter, CoordinationPingMessage>;
template struct SerializedMsg<BinaryWriter, DiskStoreRequest>;
template struct SerializedMsg<BinaryWriter, DistributorSnapRequest>;
template struct SerializedMsg<BinaryWriter, EventLogRequest>;
template struct SerializedMsg<BinaryWriter, ExecuteRequest>;
template struct SerializedMsg<BinaryWriter, ForwardRequest>;
template struct SerializedMsg<BinaryWriter, GenerationRegReadRequest>;
template struct SerializedMsg<BinaryWriter, GenerationRegWriteRequest>;
template struct SerializedMsg<BinaryWriter, GetCommitVersionRequest>;
template struct SerializedMsg<BinaryWriter, GetRateInfoRequest>;
template struct SerializedMsg<BinaryWriter, GetServerDBInfoRequest>;
template struct SerializedMsg<BinaryWriter, GetWorkersRequest>;
template struct SerializedMsg<BinaryWriter, HaltDataDistributorRequest>;
template struct SerializedMsg<BinaryWriter, HaltRatekeeperRequest>;
template struct SerializedMsg<BinaryWriter, InitializeDataDistributorRequest>;
template struct SerializedMsg<BinaryWriter, InitializeLogRouterRequest>;
template struct SerializedMsg<BinaryWriter, InitializeMasterProxyRequest>;
template struct SerializedMsg<BinaryWriter, InitializeRatekeeperRequest>;
template struct SerializedMsg<BinaryWriter, InitializeResolverRequest>;
template struct SerializedMsg<BinaryWriter, InitializeStorageRequest>;
template struct SerializedMsg<BinaryWriter, InitializeTLogRequest>;
template struct SerializedMsg<BinaryWriter, LeaderHeartbeatRequest>;
template struct SerializedMsg<BinaryWriter, LoadedPingRequest>;
template struct SerializedMsg<BinaryWriter, NetworkTestRequest>;
template struct SerializedMsg<BinaryWriter, RecruitFromConfigurationRequest>;
template struct SerializedMsg<BinaryWriter, RecruitMasterRequest>;
template struct SerializedMsg<BinaryWriter, RecruitRemoteFromConfigurationRequest>;
template struct SerializedMsg<BinaryWriter, RecruitStorageRequest>;
template struct SerializedMsg<BinaryWriter, RegisterMasterRequest>;
template struct SerializedMsg<BinaryWriter, RegisterWorkerRequest>;
template struct SerializedMsg<BinaryWriter, ReplyPromise<KeyValueStoreType>>;
template struct SerializedMsg<BinaryWriter, ReplyPromise<TLogLockResult>>;
template struct SerializedMsg<BinaryWriter, ReplyPromise<std::vector<PerfMetric>>>;
template struct SerializedMsg<BinaryWriter, ResolutionMetricsRequest>;
template struct SerializedMsg<BinaryWriter, ResolutionSplitRequest>;
template struct SerializedMsg<BinaryWriter, ResolveTransactionBatchRequest>;
template struct SerializedMsg<BinaryWriter, SetMetricsLogRateRequest>;
template struct SerializedMsg<BinaryWriter, TLogCommitRequest>;
template struct SerializedMsg<BinaryWriter, TLogConfirmRunningRequest>;
template struct SerializedMsg<BinaryWriter, TLogDisablePopRequest>;
template struct SerializedMsg<BinaryWriter, TLogEnablePopRequest>;
template struct SerializedMsg<BinaryWriter, TLogPeekRequest>;
template struct SerializedMsg<BinaryWriter, TLogPopRequest>;
template struct SerializedMsg<BinaryWriter, TLogQueuingMetricsRequest>;
template struct SerializedMsg<BinaryWriter, TLogRecoveryFinishedRequest>;
template struct SerializedMsg<BinaryWriter, TLogRejoinRequest>;
template struct SerializedMsg<BinaryWriter, TLogSnapRequest>;
template struct SerializedMsg<BinaryWriter, TestRequest>;
template struct SerializedMsg<BinaryWriter, TraceBatchDumpRequest>;
template struct SerializedMsg<BinaryWriter, WorkerSnapRequest>;
template struct SerializedMsg<BinaryWriter, WorkloadRequest>;
template struct SerializedMsg<PacketWriter, CandidacyRequest>;
template struct SerializedMsg<PacketWriter, ChangeCoordinatorsRequest>;
template struct SerializedMsg<PacketWriter, CoordinationPingMessage>;
template struct SerializedMsg<PacketWriter, DiskStoreRequest>;
template struct SerializedMsg<PacketWriter, DistributorSnapRequest>;
template struct SerializedMsg<PacketWriter, EventLogRequest>;
template struct SerializedMsg<PacketWriter, ExecuteRequest>;
template struct SerializedMsg<PacketWriter, ForwardRequest>;
template struct SerializedMsg<PacketWriter, GenerationRegReadRequest>;
template struct SerializedMsg<PacketWriter, GenerationRegWriteRequest>;
template struct SerializedMsg<PacketWriter, GetCommitVersionRequest>;
template struct SerializedMsg<PacketWriter, GetRateInfoRequest>;
template struct SerializedMsg<PacketWriter, GetServerDBInfoRequest>;
template struct SerializedMsg<PacketWriter, GetWorkersRequest>;
template struct SerializedMsg<PacketWriter, HaltDataDistributorRequest>;
template struct SerializedMsg<PacketWriter, HaltRatekeeperRequest>;
template struct SerializedMsg<PacketWriter, InitializeDataDistributorRequest>;
template struct SerializedMsg<PacketWriter, InitializeLogRouterRequest>;
template struct SerializedMsg<PacketWriter, InitializeMasterProxyRequest>;
template struct SerializedMsg<PacketWriter, InitializeRatekeeperRequest>;
template struct SerializedMsg<PacketWriter, InitializeResolverRequest>;
template struct SerializedMsg<PacketWriter, InitializeStorageRequest>;
template struct SerializedMsg<PacketWriter, InitializeTLogRequest>;
template struct SerializedMsg<PacketWriter, LeaderHeartbeatRequest>;
template struct SerializedMsg<PacketWriter, LoadedPingRequest>;
template struct SerializedMsg<PacketWriter, NetworkTestRequest>;
template struct SerializedMsg<PacketWriter, RecruitFromConfigurationRequest>;
template struct SerializedMsg<PacketWriter, RecruitMasterRequest>;
template struct SerializedMsg<PacketWriter, RecruitRemoteFromConfigurationRequest>;
template struct SerializedMsg<PacketWriter, RecruitStorageRequest>;
template struct SerializedMsg<PacketWriter, RegisterMasterRequest>;
template struct SerializedMsg<PacketWriter, RegisterWorkerRequest>;
template struct SerializedMsg<PacketWriter, ReplyPromise<KeyValueStoreType>>;
template struct SerializedMsg<PacketWriter, ReplyPromise<std::vector<PerfMetric>>>;
template struct SerializedMsg<PacketWriter, ReplyPromise<TLogLockResult>>;
template struct SerializedMsg<PacketWriter, ResolutionMetricsRequest>;
template struct SerializedMsg<PacketWriter, ResolutionSplitRequest>;
template struct SerializedMsg<PacketWriter, ResolveTransactionBatchRequest>;
template struct SerializedMsg<PacketWriter, SetMetricsLogRateRequest>;
template struct SerializedMsg<PacketWriter, TLogCommitRequest>;
template struct SerializedMsg<PacketWriter, TLogConfirmRunningRequest>;
template struct SerializedMsg<PacketWriter, TLogDisablePopRequest>;
template struct SerializedMsg<PacketWriter, TLogEnablePopRequest>;
template struct SerializedMsg<PacketWriter, TLogPeekRequest>;
template struct SerializedMsg<PacketWriter, TLogPopRequest>;
template struct SerializedMsg<PacketWriter, TLogQueuingMetricsRequest>;
template struct SerializedMsg<PacketWriter, TLogRecoveryFinishedRequest>;
template struct SerializedMsg<PacketWriter, TLogRejoinRequest>;
template struct SerializedMsg<PacketWriter, TLogSnapRequest>;
template struct SerializedMsg<PacketWriter, TestRequest>;
template struct SerializedMsg<PacketWriter, TraceBatchDumpRequest>;
template struct SerializedMsg<PacketWriter, WorkerSnapRequest>;
template struct SerializedMsg<PacketWriter, WorkloadRequest>;
