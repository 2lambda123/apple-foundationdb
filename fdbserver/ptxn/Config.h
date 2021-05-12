/*
 * Config.h
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

#ifndef FDBSERVER_PTXN_CONFIG_H
#define FDBSERVER_PTXN_CONFIG_H

#include <cstdint>

#pragma once

namespace ptxn {

/**
 * MessageTransferModel defines the model how TLog/StorageServer communicate:
 *	a) either TLog pushes the mutations to the storage server
 *	b) or the storage server pulls the mutations from TLog server
 */
enum class MessageTransferModel : uint8_t { TLogActivelyPush, StorageServerActivelyPull, InvalidModel };

} // namespace ptxn

#endif // FDBSERVER_PTXN_CONFIG_H
