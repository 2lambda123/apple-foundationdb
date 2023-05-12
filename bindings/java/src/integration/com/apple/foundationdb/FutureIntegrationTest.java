/*
 * FutureIntegrationTest.java
 *
 * This source file is part of the FoundationDB open source project
 *
 * Copyright 2013-2023 Apple Inc. and the FoundationDB project authors
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
package com.apple.foundationdb;

import java.util.concurrent.CompletableFuture;
import java.util.concurrent.Executor;
import java.util.concurrent.TimeUnit;

import org.junit.jupiter.api.Tag;
import org.junit.jupiter.api.Test;
import org.junit.jupiter.api.Timeout;
import org.junit.jupiter.api.extension.ExtendWith;

/**
 * Tests for working with FDB futures
 */
@ExtendWith(RequiresDatabase.class)
class FutureIntegrationTest {
  private static final FDB fdb = FDB.selectAPIVersion(ApiVersion.LATEST);

  static class DirectExecutor implements Executor {
    @Override
    public void execute(Runnable command) {
      System.out.println("Executing callback");
      command.run();
    }
  }

  @Test
  @Tag("SupportsExternalClient")
  public void testCancelFutureOnThreadPool() throws Exception {
    try (Database db = fdb.open()) {
      System.out.println("Executing transaction");
      Transaction tr = db.createTransaction();
      CompletableFuture<byte[]> result = tr.get("hello".getBytes("US-ASCII"));
      System.out.println("Cancelling future");
      result.cancel(true);
    }
  }

  @Test
  @Tag("SupportsExternalClient")
  public void testCancelFutureOnSameThread() throws Exception {
    try (Database db = fdb.open(null, new DirectExecutor())) {
      System.out.println("Executing transaction");
      Transaction tr = db.createTransaction();
      CompletableFuture<Void> result = tr.watch("hello".getBytes("US-ASCII"));
      System.out.println("Cancelling future");
      result.cancel(true);
    }
  }

}
