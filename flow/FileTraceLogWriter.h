/*
 * FileTraceLogWriter.h
 *
 * This source file is part of the FoundationDB open source project
 *
 * Copyright 2018 Apple Inc. and the FoundationDB project authors
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

#ifndef FLOW_FILE_TRACE_LOG_WRITER_H
#define FLOW_FILE_TRACE_LOG_WRITER_H
#pragma once

#include "flow/Arena.h"
#include "flow/FastRef.h"
#include "flow/Trace.h"
#include "flow/network.h"

#include <functional>

class FileTraceLogWriter final : public ITraceLogWriter, ReferenceCounted<FileTraceLogWriter> {
private:
	void write(const char* str, size_t size);
	void initializeFile();

	TraceLogWriterParams writerParams;
	Optional<NetworkAddress> address;
	bool opened = false;
	bool initialized = false;
	uint32_t index = 0;
	int traceFileFD;
	std::string basenameWithProcess;
	std::string noAddressBasename;
	std::string finalname;

	std::vector<std::string> pendingLogs;

public:
	FileTraceLogWriter();

	void addref() override;
	void delref() override;

	void lastError(int err);

	void write(const std::string& str) override;
	void write(StringRef const& str) override;
	void open(TraceLogWriterParams const& params) override;
	void setNetworkAddress(NetworkAddress const& address) override;
	void close() override;
	void roll() override;
	void sync() override;

	void cleanupTraceFiles();
};

#endif
