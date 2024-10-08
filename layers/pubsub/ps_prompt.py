#!/usr/bin/python -i
#
# ps_prompt.py
#
# This source file is part of the FoundationDB open source project
#
# Copyright 2013-2024 Apple Inc. and the FoundationDB project authors
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
#


import os
import sys

sys.path[:0] = [
    os.path.join(os.path.dirname(__file__), "..", "..", "bindings", "python")
]
import fdb
from pubsub_bigdoc import PubSub

db = fdb.open("10.0.3.1:2181/bbc", "TwitDB")

ps = PubSub(db)
