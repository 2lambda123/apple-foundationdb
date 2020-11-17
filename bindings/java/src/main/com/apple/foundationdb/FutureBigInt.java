/*
 * FutureBigInt.java
 *
 * This source file is part of the FoundationDB open source project
 *
 * Copyright 2013-2019 Apple Inc. and the FoundationDB project authors
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

import java.math.BigInteger;
import java.util.concurrent.Executor;

class FutureBigInt extends NativeFuture<BigInteger> {
	FutureBigInt(long cPtr, Executor executor) {
		super(cPtr);
		registerMarshalCallback(executor);
	}

	@Override
	protected BigInteger getIfDone_internal(long cPtr) throws FDBException {
		long versionLong = FutureBigInt_get(cPtr);
        
        if(versionLong >= 0L) {
            return BigInteger.valueOf(versionLong);
		}
		
		int upper = (int) (versionLong >>> 32);
		int lower = (int) versionLong;

		return (BigInteger.valueOf(Integer.toUnsignedLong(upper))).shiftLeft(32).
				add(BigInteger.valueOf(Integer.toUnsignedLong(lower)));

	}

	private native long FutureBigInt_get(long cPtr) throws FDBException;
}
