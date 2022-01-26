#ifdef SSD_ROCKSDB_EXPERIMENTAL

#include <rocksdb/cache.h>
#include <rocksdb/db.h>
#include <rocksdb/filter_policy.h>
#include <rocksdb/options.h>
#include <rocksdb/metadata.h>
#include <rocksdb/slice_transform.h>
#include <rocksdb/sst_file_reader.h>
#include <rocksdb/sst_file_writer.h>
#include <rocksdb/slice.h>
#include <rocksdb/env.h>
#include <rocksdb/options.h>
#include <rocksdb/statistics.h>
#include <rocksdb/table.h>
#include <rocksdb/version.h>
#include <rocksdb/types.h>
#include <rocksdb/utilities/checkpoint.h>
#include <rocksdb/utilities/table_properties_collectors.h>
#include <rocksdb/version.h>

#include <rocksdb/rate_limiter.h>
#include "fdbclient/SystemData.h"
#include "fdbserver/CoroFlow.h"
#include "flow/flow.h"
#include "flow/IThreadPool.h"
#include "flow/ThreadHelper.actor.h"
#include "flow/Histogram.h"

#include <memory>
#include <tuple>
#include <vector>

#endif // SSD_ROCKSDB_EXPERIMENTAL

#include "fdbserver/IKeyValueStore.h"
#include "flow/actorcompiler.h" // has to be last include

#ifdef SSD_ROCKSDB_EXPERIMENTAL

// Enforcing rocksdb version to be 6.22.1 or greater.
static_assert(ROCKSDB_MAJOR >= 6, "Unsupported rocksdb version. Update the rocksdb to 6.22.1 version");
static_assert(ROCKSDB_MAJOR == 6 ? ROCKSDB_MINOR >= 22 : true,
              "Unsupported rocksdb version. Update the rocksdb to 6.22.1 version");
static_assert((ROCKSDB_MAJOR == 6 && ROCKSDB_MINOR == 22) ? ROCKSDB_PATCH >= 1 : true,
              "Unsupported rocksdb version. Update the rocksdb to 6.22.1 version");

namespace {
using DB = rocksdb::DB*;
using CF = rocksdb::ColumnFamilyHandle*;

#define PERSIST_PREFIX "\xff\xff"
const KeyRef persistVersion = LiteralStringRef(PERSIST_PREFIX "Version");
const StringRef ROCKSDBSTORAGE_HISTOGRAM_GROUP = LiteralStringRef("RocksDBStorage");
const StringRef ROCKSDB_COMMIT_LATENCY_HISTOGRAM = LiteralStringRef("RocksDBCommitLatency");
const StringRef ROCKSDB_COMMIT_ACTION_HISTOGRAM = LiteralStringRef("RocksDBCommitAction");
const StringRef ROCKSDB_COMMIT_QUEUEWAIT_HISTOGRAM = LiteralStringRef("RocksDBCommitQueueWait");
const StringRef ROCKSDB_WRITE_HISTOGRAM = LiteralStringRef("RocksDBWrite");
const StringRef ROCKSDB_DELETE_COMPACTRANGE_HISTOGRAM = LiteralStringRef("RocksDBDeleteCompactRange");
const StringRef ROCKSDB_READRANGE_LATENCY_HISTOGRAM = LiteralStringRef("RocksDBReadRangeLatency");
const StringRef ROCKSDB_READVALUE_LATENCY_HISTOGRAM = LiteralStringRef("RocksDBReadValueLatency");
const StringRef ROCKSDB_READPREFIX_LATENCY_HISTOGRAM = LiteralStringRef("RocksDBReadPrefixLatency");
const StringRef ROCKSDB_READRANGE_ACTION_HISTOGRAM = LiteralStringRef("RocksDBReadRangeAction");
const StringRef ROCKSDB_READVALUE_ACTION_HISTOGRAM = LiteralStringRef("RocksDBReadValueAction");
const StringRef ROCKSDB_READPREFIX_ACTION_HISTOGRAM = LiteralStringRef("RocksDBReadPrefixAction");
const StringRef ROCKSDB_READRANGE_QUEUEWAIT_HISTOGRAM = LiteralStringRef("RocksDBReadRangeQueueWait");
const StringRef ROCKSDB_READVALUE_QUEUEWAIT_HISTOGRAM = LiteralStringRef("RocksDBReadValueQueueWait");
const StringRef ROCKSDB_READPREFIX_QUEUEWAIT_HISTOGRAM = LiteralStringRef("RocksDBReadPrefixQueueWait");
const StringRef ROCKSDB_READRANGE_NEWITERATOR_HISTOGRAM = LiteralStringRef("RocksDBReadRangeNewIterator");
const StringRef ROCKSDB_READVALUE_GET_HISTOGRAM = LiteralStringRef("RocksDBReadValueGet");
const StringRef ROCKSDB_READPREFIX_GET_HISTOGRAM = LiteralStringRef("RocksDBReadPrefixGet");

rocksdb::ExportImportFilesMetaData getMetaData(const CheckpointMetaData& checkpoint) {
	rocksdb::ExportImportFilesMetaData metaData;
	metaData.db_comparator_name = checkpoint.rocksCF.get().dbComparatorName;

	if (checkpoint.rocksCF.present()) {
		for (const LiveFileMetaData& fileMetaData : checkpoint.rocksCF.get().sstFiles) {
			rocksdb::LiveFileMetaData liveFileMetaData;
			liveFileMetaData.size = fileMetaData.size;
			liveFileMetaData.name = fileMetaData.name;
			liveFileMetaData.file_number = fileMetaData.file_number;
			liveFileMetaData.db_path = fileMetaData.db_path;
			liveFileMetaData.smallest_seqno = fileMetaData.smallest_seqno;
			liveFileMetaData.largest_seqno = fileMetaData.largest_seqno;
			liveFileMetaData.smallestkey = fileMetaData.smallestkey;
			liveFileMetaData.largestkey = fileMetaData.largestkey;
			liveFileMetaData.num_reads_sampled = fileMetaData.num_reads_sampled;
			liveFileMetaData.being_compacted = fileMetaData.being_compacted;
			liveFileMetaData.num_entries = fileMetaData.num_entries;
			liveFileMetaData.num_deletions = fileMetaData.num_deletions;
			liveFileMetaData.temperature = static_cast<rocksdb::Temperature>(fileMetaData.temperature);
			liveFileMetaData.oldest_blob_file_number = fileMetaData.oldest_blob_file_number;
			liveFileMetaData.oldest_ancester_time = fileMetaData.oldest_ancester_time;
			liveFileMetaData.file_creation_time = fileMetaData.file_creation_time;
			liveFileMetaData.file_checksum = fileMetaData.file_checksum;
			liveFileMetaData.file_checksum_func_name = fileMetaData.file_checksum_func_name;
			liveFileMetaData.column_family_name = fileMetaData.column_family_name;
			liveFileMetaData.level = fileMetaData.level;
			metaData.files.push_back(liveFileMetaData);
		}
	}

	return metaData;
}

void populateMetaData(CheckpointMetaData* checkpoint, const rocksdb::ExportImportFilesMetaData& metaData) {
	RocksDBColumnFamilyCheckpoint rocksCF;
	rocksCF.dbComparatorName = metaData.db_comparator_name;
	for (const rocksdb::LiveFileMetaData& fileMetaData : metaData.files) {
		LiveFileMetaData liveFileMetaData;
		liveFileMetaData.size = fileMetaData.size;
		liveFileMetaData.name = fileMetaData.name;
		liveFileMetaData.file_number = fileMetaData.file_number;
		liveFileMetaData.db_path = fileMetaData.db_path;
		liveFileMetaData.smallest_seqno = fileMetaData.smallest_seqno;
		liveFileMetaData.largest_seqno = fileMetaData.largest_seqno;
		liveFileMetaData.smallestkey = fileMetaData.smallestkey;
		liveFileMetaData.largestkey = fileMetaData.largestkey;
		liveFileMetaData.num_reads_sampled = fileMetaData.num_reads_sampled;
		liveFileMetaData.being_compacted = fileMetaData.being_compacted;
		liveFileMetaData.num_entries = fileMetaData.num_entries;
		liveFileMetaData.num_deletions = fileMetaData.num_deletions;
		liveFileMetaData.temperature = static_cast<uint8_t>(fileMetaData.temperature);
		liveFileMetaData.oldest_blob_file_number = fileMetaData.oldest_blob_file_number;
		liveFileMetaData.oldest_ancester_time = fileMetaData.oldest_ancester_time;
		liveFileMetaData.file_creation_time = fileMetaData.file_creation_time;
		liveFileMetaData.file_checksum = fileMetaData.file_checksum;
		liveFileMetaData.file_checksum_func_name = fileMetaData.file_checksum_func_name;
		liveFileMetaData.column_family_name = fileMetaData.column_family_name;
		liveFileMetaData.level = fileMetaData.level;
		rocksCF.sstFiles.push_back(liveFileMetaData);
	}
	checkpoint->format = RocksDBColumnFamily;
	checkpoint->rocksCF = rocksCF;
}

rocksdb::Slice toSlice(StringRef s) {
	return rocksdb::Slice(reinterpret_cast<const char*>(s.begin()), s.size());
}

StringRef toStringRef(rocksdb::Slice s) {
	return StringRef(reinterpret_cast<const uint8_t*>(s.data()), s.size());
}

rocksdb::ColumnFamilyOptions getCFOptions() {
	rocksdb::ColumnFamilyOptions options;
	options.level_compaction_dynamic_level_bytes = true;
	options.OptimizeLevelStyleCompaction(SERVER_KNOBS->ROCKSDB_MEMTABLE_BYTES);
	if (SERVER_KNOBS->ROCKSDB_PERIODIC_COMPACTION_SECONDS > 0) {
		options.periodic_compaction_seconds = SERVER_KNOBS->ROCKSDB_PERIODIC_COMPACTION_SECONDS;
	}
	// Compact sstables when there's too much deleted stuff.
	options.table_properties_collector_factories = { rocksdb::NewCompactOnDeletionCollectorFactory(128, 1) };

	rocksdb::BlockBasedTableOptions bbOpts;
	// TODO: Add a knob for the block cache size. (Default is 8 MB)
	if (SERVER_KNOBS->ROCKSDB_PREFIX_LEN > 0) {
		// Prefix blooms are used during Seek.
		options.prefix_extractor.reset(rocksdb::NewFixedPrefixTransform(SERVER_KNOBS->ROCKSDB_PREFIX_LEN));

		// Also turn on bloom filters in the memtable.
		// TODO: Make a knob for this as well.
		options.memtable_prefix_bloom_size_ratio = 0.1;

		// 5 -- Can be read by RocksDB's versions since 6.6.0. Full and partitioned
		// filters use a generally faster and more accurate Bloom filter
		// implementation, with a different schema.
		// https://github.com/facebook/rocksdb/blob/b77569f18bfc77fb1d8a0b3218f6ecf571bc4988/include/rocksdb/table.h#L391
		bbOpts.format_version = 5;

		// Create and apply a bloom filter using the 10 bits
		// which should yield a ~1% false positive rate:
		// https://github.com/facebook/rocksdb/wiki/RocksDB-Bloom-Filter#full-filters-new-format
		bbOpts.filter_policy.reset(rocksdb::NewBloomFilterPolicy(10));

		// The whole key blooms are only used for point lookups.
		// https://github.com/facebook/rocksdb/wiki/RocksDB-Bloom-Filter#prefix-vs-whole-key
		bbOpts.whole_key_filtering = false;
	}

	if (SERVER_KNOBS->ROCKSDB_BLOCK_CACHE_SIZE > 0) {
		bbOpts.block_cache = rocksdb::NewLRUCache(SERVER_KNOBS->ROCKSDB_BLOCK_CACHE_SIZE);
	}

	options.table_factory.reset(rocksdb::NewBlockBasedTableFactory(bbOpts));

	return options;
}

rocksdb::Options getOptions() {
	rocksdb::Options options({}, getCFOptions());
	options.avoid_unnecessary_blocking_io = true;
	options.create_if_missing = true;
	if (SERVER_KNOBS->ROCKSDB_BACKGROUND_PARALLELISM > 0) {
		options.IncreaseParallelism(SERVER_KNOBS->ROCKSDB_BACKGROUND_PARALLELISM);
	}

	options.statistics = rocksdb::CreateDBStatistics();
	options.statistics->set_stats_level(rocksdb::kExceptHistogramOrTimers);

	options.db_log_dir = SERVER_KNOBS->LOG_DIRECTORY;
	return options;
}

// Set some useful defaults desired for all reads.
rocksdb::ReadOptions getReadOptions() {
	rocksdb::ReadOptions options;
	options.background_purge_on_iterator_cleanup = true;
	return options;
}

struct ReadIterator {
	CF& cf;
	uint64_t index; // incrementing counter to uniquely identify read iterator.
	bool inUse;
	std::shared_ptr<rocksdb::Iterator> iter;
	double creationTime;
	ReadIterator(CF& cf, uint64_t index, DB& db, rocksdb::ReadOptions& options)
	  : cf(cf), index(index), inUse(true), creationTime(now()), iter(db->NewIterator(options, cf)) {}
};

/*
ReadIteratorPool: Collection of iterators. Reuses iterators on non-concurrent multiple read operations,
instead of creating and deleting for every read.

Read: IteratorPool provides an unused iterator if exists or creates and gives a new iterator.
Returns back the iterator after the read is done.

Write: Iterators in the pool are deleted, forcing new iterator creation on next reads. The iterators
which are currently used by the reads can continue using the iterator as it is a shared_ptr. Once
the read is processed, shared_ptr goes out of scope and gets deleted. Eventually the iterator object
gets deleted as the ref count becomes 0.
*/
class ReadIteratorPool {
public:
	ReadIteratorPool(DB& db, CF& cf, const std::string& path)
	  : db(db), cf(cf), index(0), iteratorsReuseCount(0), readRangeOptions(getReadOptions()) {
		readRangeOptions.background_purge_on_iterator_cleanup = true;
		readRangeOptions.auto_prefix_mode = (SERVER_KNOBS->ROCKSDB_PREFIX_LEN > 0);
		TraceEvent("ReadIteratorPool")
		    .detail("Path", path)
		    .detail("KnobRocksDBReadRangeReuseIterators", SERVER_KNOBS->ROCKSDB_READ_RANGE_REUSE_ITERATORS)
		    .detail("KnobRocksDBPrefixLen", SERVER_KNOBS->ROCKSDB_PREFIX_LEN);
	}

	// Called on every db commit.
	void update() {
		if (SERVER_KNOBS->ROCKSDB_READ_RANGE_REUSE_ITERATORS) {
			std::lock_guard<std::mutex> lock(mutex);
			iteratorsMap.clear();
		}
	}

	// Called on every read operation.
	ReadIterator getIterator() {
		if (SERVER_KNOBS->ROCKSDB_READ_RANGE_REUSE_ITERATORS) {
			std::lock_guard<std::mutex> lock(mutex);
			for (it = iteratorsMap.begin(); it != iteratorsMap.end(); it++) {
				if (!it->second.inUse) {
					it->second.inUse = true;
					iteratorsReuseCount++;
					return it->second;
				}
			}
			index++;
			ReadIterator iter(cf, index, db, readRangeOptions);
			iteratorsMap.insert({ index, iter });
			return iter;
		} else {
			index++;
			ReadIterator iter(cf, index, db, readRangeOptions);
			return iter;
		}
	}

	// Called on every read operation, after the keys are collected.
	void returnIterator(ReadIterator& iter) {
		if (SERVER_KNOBS->ROCKSDB_READ_RANGE_REUSE_ITERATORS) {
			std::lock_guard<std::mutex> lock(mutex);
			it = iteratorsMap.find(iter.index);
			// iterator found: put the iterator back to the pool(inUse=false).
			// iterator not found: update would have removed the iterator from pool, so nothing to do.
			if (it != iteratorsMap.end()) {
				ASSERT(it->second.inUse);
				it->second.inUse = false;
			}
		}
	}

	// Called for every ROCKSDB_READ_RANGE_ITERATOR_REFRESH_TIME seconds in a loop.
	void refreshIterators() {
		std::lock_guard<std::mutex> lock(mutex);
		it = iteratorsMap.begin();
		while (it != iteratorsMap.end()) {
			if (now() - it->second.creationTime > SERVER_KNOBS->ROCKSDB_READ_RANGE_ITERATOR_REFRESH_TIME) {
				it = iteratorsMap.erase(it);
			} else {
				it++;
			}
		}
	}

	uint64_t numReadIteratorsCreated() { return index; }

	uint64_t numTimesReadIteratorsReused() { return iteratorsReuseCount; }

private:
	std::unordered_map<int, ReadIterator> iteratorsMap;
	std::unordered_map<int, ReadIterator>::iterator it;
	DB& db;
	CF& cf;
	rocksdb::ReadOptions readRangeOptions;
	std::mutex mutex;
	// incrementing counter for every new iterator creation, to uniquely identify the iterator in returnIterator().
	uint64_t index;
	uint64_t iteratorsReuseCount;
};

ACTOR Future<Void> refreshReadIteratorPool(std::shared_ptr<ReadIteratorPool> readIterPool) {
	if (SERVER_KNOBS->ROCKSDB_READ_RANGE_REUSE_ITERATORS) {
		loop {
			wait(delay(SERVER_KNOBS->ROCKSDB_READ_RANGE_ITERATOR_REFRESH_TIME));
			readIterPool->refreshIterators();
		}
	}
	return Void();
}

ACTOR Future<Void> flowLockLogger(const FlowLock* readLock, const FlowLock* fetchLock) {
	loop {
		wait(delay(SERVER_KNOBS->ROCKSDB_METRICS_DELAY));
		TraceEvent e("RocksDBFlowLock");
		e.detail("ReadAvailable", readLock->available());
		e.detail("ReadActivePermits", readLock->activePermits());
		e.detail("ReadWaiters", readLock->waiters());
		e.detail("FetchAvailable", fetchLock->available());
		e.detail("FetchActivePermits", fetchLock->activePermits());
		e.detail("FetchWaiters", fetchLock->waiters());
	}
}

ACTOR Future<Void> rocksDBMetricLogger(std::shared_ptr<rocksdb::Statistics> statistics,
                                       rocksdb::DB* db,
                                       std::shared_ptr<ReadIteratorPool> readIterPool) {
	state std::vector<std::tuple<const char*, uint32_t, uint64_t>> tickerStats = {
		{ "StallMicros", rocksdb::STALL_MICROS, 0 },
		{ "BytesRead", rocksdb::BYTES_READ, 0 },
		{ "IterBytesRead", rocksdb::ITER_BYTES_READ, 0 },
		{ "BytesWritten", rocksdb::BYTES_WRITTEN, 0 },
		{ "BlockCacheMisses", rocksdb::BLOCK_CACHE_MISS, 0 },
		{ "BlockCacheHits", rocksdb::BLOCK_CACHE_HIT, 0 },
		{ "BloomFilterUseful", rocksdb::BLOOM_FILTER_USEFUL, 0 },
		{ "BloomFilterFullPositive", rocksdb::BLOOM_FILTER_FULL_POSITIVE, 0 },
		{ "BloomFilterTruePositive", rocksdb::BLOOM_FILTER_FULL_TRUE_POSITIVE, 0 },
		{ "BloomFilterMicros", rocksdb::BLOOM_FILTER_MICROS, 0 },
		{ "MemtableHit", rocksdb::MEMTABLE_HIT, 0 },
		{ "MemtableMiss", rocksdb::MEMTABLE_MISS, 0 },
		{ "GetHitL0", rocksdb::GET_HIT_L0, 0 },
		{ "GetHitL1", rocksdb::GET_HIT_L1, 0 },
		{ "GetHitL2AndUp", rocksdb::GET_HIT_L2_AND_UP, 0 },
		{ "CountKeysWritten", rocksdb::NUMBER_KEYS_WRITTEN, 0 },
		{ "CountKeysRead", rocksdb::NUMBER_KEYS_READ, 0 },
		{ "CountDBSeek", rocksdb::NUMBER_DB_SEEK, 0 },
		{ "CountDBNext", rocksdb::NUMBER_DB_NEXT, 0 },
		{ "CountDBPrev", rocksdb::NUMBER_DB_PREV, 0 },
		{ "BloomFilterPrefixChecked", rocksdb::BLOOM_FILTER_PREFIX_CHECKED, 0 },
		{ "BloomFilterPrefixUseful", rocksdb::BLOOM_FILTER_PREFIX_USEFUL, 0 },
		{ "BlockCacheCompressedMiss", rocksdb::BLOCK_CACHE_COMPRESSED_MISS, 0 },
		{ "BlockCacheCompressedHit", rocksdb::BLOCK_CACHE_COMPRESSED_HIT, 0 },
		{ "CountWalFileSyncs", rocksdb::WAL_FILE_SYNCED, 0 },
		{ "CountWalFileBytes", rocksdb::WAL_FILE_BYTES, 0 },
		{ "CompactReadBytes", rocksdb::COMPACT_READ_BYTES, 0 },
		{ "CompactWriteBytes", rocksdb::COMPACT_WRITE_BYTES, 0 },
		{ "FlushWriteBytes", rocksdb::FLUSH_WRITE_BYTES, 0 },
		{ "CountBlocksCompressed", rocksdb::NUMBER_BLOCK_COMPRESSED, 0 },
		{ "CountBlocksDecompressed", rocksdb::NUMBER_BLOCK_DECOMPRESSED, 0 },
		{ "RowCacheHit", rocksdb::ROW_CACHE_HIT, 0 },
		{ "RowCacheMiss", rocksdb::ROW_CACHE_MISS, 0 },
		{ "CountIterSkippedKeys", rocksdb::NUMBER_ITER_SKIP, 0 },

	};
	state std::vector<std::pair<const char*, std::string>> propertyStats = {
		{ "NumCompactionsRunning", rocksdb::DB::Properties::kNumRunningCompactions },
		{ "NumImmutableMemtables", rocksdb::DB::Properties::kNumImmutableMemTable },
		{ "NumImmutableMemtablesFlushed", rocksdb::DB::Properties::kNumImmutableMemTableFlushed },
		{ "IsMemtableFlushPending", rocksdb::DB::Properties::kMemTableFlushPending },
		{ "NumRunningFlushes", rocksdb::DB::Properties::kNumRunningFlushes },
		{ "IsCompactionPending", rocksdb::DB::Properties::kCompactionPending },
		{ "NumRunningCompactions", rocksdb::DB::Properties::kNumRunningCompactions },
		{ "CumulativeBackgroundErrors", rocksdb::DB::Properties::kBackgroundErrors },
		{ "CurrentSizeActiveMemtable", rocksdb::DB::Properties::kCurSizeActiveMemTable },
		{ "AllMemtablesBytes", rocksdb::DB::Properties::kCurSizeAllMemTables },
		{ "ActiveMemtableBytes", rocksdb::DB::Properties::kSizeAllMemTables },
		{ "CountEntriesActiveMemtable", rocksdb::DB::Properties::kNumEntriesActiveMemTable },
		{ "CountEntriesImmutMemtables", rocksdb::DB::Properties::kNumEntriesImmMemTables },
		{ "CountDeletesActiveMemtable", rocksdb::DB::Properties::kNumDeletesActiveMemTable },
		{ "CountDeletesImmutMemtables", rocksdb::DB::Properties::kNumDeletesImmMemTables },
		{ "EstimatedCountKeys", rocksdb::DB::Properties::kEstimateNumKeys },
		{ "EstimateSstReaderBytes", rocksdb::DB::Properties::kEstimateTableReadersMem },
		{ "CountActiveSnapshots", rocksdb::DB::Properties::kNumSnapshots },
		{ "OldestSnapshotTime", rocksdb::DB::Properties::kOldestSnapshotTime },
		{ "CountLiveVersions", rocksdb::DB::Properties::kNumLiveVersions },
		{ "EstimateLiveDataSize", rocksdb::DB::Properties::kEstimateLiveDataSize },
		{ "BaseLevel", rocksdb::DB::Properties::kBaseLevel },
		{ "EstPendCompactBytes", rocksdb::DB::Properties::kEstimatePendingCompactionBytes },
	};

	state std::unordered_map<std::string, uint64_t> readIteratorPoolStats = {
		{ "NumReadIteratorsCreated", 0 },
		{ "NumTimesReadIteratorsReused", 0 },
	};

	loop {
		wait(delay(SERVER_KNOBS->ROCKSDB_METRICS_DELAY));
		TraceEvent e("RocksDBMetrics");
		uint64_t stat;
		for (auto& t : tickerStats) {
			auto& [name, ticker, cum] = t;
			stat = statistics->getTickerCount(ticker);
			e.detail(name, stat - cum);
			cum = stat;
		}

		for (auto& p : propertyStats) {
			auto& [name, property] = p;
			stat = 0;
			ASSERT(db->GetIntProperty(property, &stat));
			e.detail(name, stat);
		}

		stat = readIterPool->numReadIteratorsCreated();
		e.detail("NumReadIteratorsCreated", stat - readIteratorPoolStats["NumReadIteratorsCreated"]);
		readIteratorPoolStats["NumReadIteratorsCreated"] = stat;

		stat = readIterPool->numTimesReadIteratorsReused();
		e.detail("NumTimesReadIteratorsReused", stat - readIteratorPoolStats["NumTimesReadIteratorsReused"]);
		readIteratorPoolStats["NumTimesReadIteratorsReused"] = stat;
	}
}

void logRocksDBError(const rocksdb::Status& status, const std::string& method) {
	auto level = status.IsTimedOut() ? SevWarn : SevError;
	TraceEvent e(level, "RocksDBError");
	e.detail("Error", status.ToString()).detail("Method", method).detail("RocksDBSeverity", status.severity());
	if (status.IsIOError()) {
		e.detail("SubCode", status.subcode());
	}
}

Error statusToError(const rocksdb::Status& s) {
	if (s.IsIOError()) {
		return io_error();
	} else if (s.IsTimedOut()) {
		return transaction_too_old();
	} else {
		return unknown_error();
	}
}

struct RocksDBKeyValueStore : IKeyValueStore {
	struct Writer : IThreadPoolReceiver {
		DB& db;
		CF& cf;
		UID id;
		std::shared_ptr<rocksdb::RateLimiter> rateLimiter;
		Reference<Histogram> commitLatencyHistogram;
		Reference<Histogram> commitActionHistogram;
		Reference<Histogram> commitQueueWaitHistogram;
		Reference<Histogram> writeHistogram;
		Reference<Histogram> deleteCompactRangeHistogram;
		std::shared_ptr<ReadIteratorPool> readIterPool;

		explicit Writer(DB& db, CF& cf, UID id, std::shared_ptr<ReadIteratorPool> readIterPool)
		  : db(db), cf(cf), id(id), readIterPool(readIterPool),
		    rateLimiter(SERVER_KNOBS->ROCKSDB_WRITE_RATE_LIMITER_BYTES_PER_SEC > 0
		                    ? rocksdb::NewGenericRateLimiter(
		                          SERVER_KNOBS->ROCKSDB_WRITE_RATE_LIMITER_BYTES_PER_SEC, // rate_bytes_per_sec
		                          100 * 1000, // refill_period_us
		                          10, // fairness
		                          rocksdb::RateLimiter::Mode::kWritesOnly,
		                          SERVER_KNOBS->ROCKSDB_WRITE_RATE_LIMITER_AUTO_TUNE)
		                    : nullptr),
		    commitLatencyHistogram(Histogram::getHistogram(ROCKSDBSTORAGE_HISTOGRAM_GROUP,
		                                                   ROCKSDB_COMMIT_LATENCY_HISTOGRAM,
		                                                   Histogram::Unit::microseconds)),
		    commitActionHistogram(Histogram::getHistogram(ROCKSDBSTORAGE_HISTOGRAM_GROUP,
		                                                  ROCKSDB_COMMIT_ACTION_HISTOGRAM,
		                                                  Histogram::Unit::microseconds)),
		    commitQueueWaitHistogram(Histogram::getHistogram(ROCKSDBSTORAGE_HISTOGRAM_GROUP,
		                                                     ROCKSDB_COMMIT_QUEUEWAIT_HISTOGRAM,
		                                                     Histogram::Unit::microseconds)),
		    writeHistogram(Histogram::getHistogram(ROCKSDBSTORAGE_HISTOGRAM_GROUP,
		                                           ROCKSDB_WRITE_HISTOGRAM,
		                                           Histogram::Unit::microseconds)),
		    deleteCompactRangeHistogram(Histogram::getHistogram(ROCKSDBSTORAGE_HISTOGRAM_GROUP,
		                                                        ROCKSDB_DELETE_COMPACTRANGE_HISTOGRAM,
		                                                        Histogram::Unit::microseconds)) {}

		~Writer() override {
			if (db) {
				delete db;
			}
		}

		void init() override {}

		struct OpenAction : TypedAction<Writer, OpenAction> {
			std::string path;
			ThreadReturnPromise<Void> done;
			Optional<Future<Void>>& metrics;
			const FlowLock* readLock;
			const FlowLock* fetchLock;
			OpenAction(std::string path,
			           Optional<Future<Void>>& metrics,
			           const FlowLock* readLock,
			           const FlowLock* fetchLock)
			  : path(std::move(path)), metrics(metrics), readLock(readLock), fetchLock(fetchLock) {}

			double getTimeEstimate() const override { return SERVER_KNOBS->COMMIT_TIME_ESTIMATE; }
		};
		void action(OpenAction& a) {
			ASSERT(cf == nullptr);

			std::vector<std::string> columnFamilies;
			rocksdb::Options options = getOptions();
			rocksdb::Status status = rocksdb::DB::ListColumnFamilies(options, a.path, &columnFamilies);
			std::cout << "Open RocksDB Found Column Families: " << describe(columnFamilies) << std::endl;
			if (std::find(columnFamilies.begin(), columnFamilies.end(), "default") == columnFamilies.end()) {
				columnFamilies.push_back("default");
			}

			rocksdb::ColumnFamilyOptions cfOptions = getCFOptions();
			std::vector<rocksdb::ColumnFamilyDescriptor> descriptors;
			for (const std::string& name : columnFamilies) {
				descriptors.push_back(rocksdb::ColumnFamilyDescriptor{ name, cfOptions });
			}

			if (SERVER_KNOBS->ROCKSDB_WRITE_RATE_LIMITER_BYTES_PER_SEC > 0) {
				options.rate_limiter = rateLimiter;
			}

			std::vector<rocksdb::ColumnFamilyHandle*> handles;
			status = rocksdb::DB::Open(options, a.path, descriptors, &handles, &db);

			if (!status.ok()) {
				logRocksDBError(status, "Open");
				a.done.sendError(statusToError(status));
				return;
			}

			for (rocksdb::ColumnFamilyHandle* handle : handles) {
				if (handle->GetName() == SERVER_KNOBS->DEFAULT_FDB_ROCKSDB_COLUMN_FAMILY) {
					cf = handle;
					break;
				}
			}

			if (cf == nullptr) {
				status = db->CreateColumnFamily(cfOptions, SERVER_KNOBS->DEFAULT_FDB_ROCKSDB_COLUMN_FAMILY, &cf);
				if (!status.ok()) {
					logRocksDBError(status, "Open");
					a.done.sendError(statusToError(status));
				}
			}

			TraceEvent(SevInfo, "RocksDB")
			    .detail("Path", a.path)
			    .detail("Method", "Open")
			    .detail("KnobRocksDBWriteRateLimiterBytesPerSec",
			            SERVER_KNOBS->ROCKSDB_WRITE_RATE_LIMITER_BYTES_PER_SEC)
			    .detail("KnobRocksDBWriteRateLimiterAutoTune", SERVER_KNOBS->ROCKSDB_WRITE_RATE_LIMITER_AUTO_TUNE)
			    .detail("ColumnFamily", cf->GetName());
			if (g_network->isSimulated()) {
				// The current thread and main thread are same when the code runs in simulation.
				// blockUntilReady() is getting the thread into deadlock state, so directly calling
				// the metricsLogger.
				a.metrics = rocksDBMetricLogger(options.statistics, db, readIterPool) &&
				            flowLockLogger(a.readLock, a.fetchLock) && refreshReadIteratorPool(readIterPool);
			} else {
				onMainThread([&] {
					a.metrics = rocksDBMetricLogger(options.statistics, db, readIterPool) &&
					            flowLockLogger(a.readLock, a.fetchLock) && refreshReadIteratorPool(readIterPool);
					return Future<bool>(true);
				}).blockUntilReady();
			}
			a.done.send(Void());
		}

		struct DeleteVisitor : public rocksdb::WriteBatch::Handler {
			VectorRef<KeyRangeRef>& deletes;
			Arena& arena;

			DeleteVisitor(VectorRef<KeyRangeRef>& deletes, Arena& arena) : deletes(deletes), arena(arena) {}

			rocksdb::Status DeleteRangeCF(uint32_t /*column_family_id*/,
			                              const rocksdb::Slice& begin,
			                              const rocksdb::Slice& end) override {
				KeyRangeRef kr(toStringRef(begin), toStringRef(end));
				deletes.push_back_deep(arena, kr);
				return rocksdb::Status::OK();
			}

			rocksdb::Status PutCF(uint32_t column_family_id,
			                      const rocksdb::Slice& key,
			                      const rocksdb::Slice& value) override {
				return rocksdb::Status::OK();
			}

			rocksdb::Status DeleteCF(uint32_t column_family_id, const rocksdb::Slice& key) override {
				return rocksdb::Status::OK();
			}

			rocksdb::Status SingleDeleteCF(uint32_t column_family_id, const rocksdb::Slice& key) override {
				return rocksdb::Status::OK();
			}

			rocksdb::Status MergeCF(uint32_t column_family_id,
			                        const rocksdb::Slice& key,
			                        const rocksdb::Slice& value) override {
				return rocksdb::Status::OK();
			}
		};

		struct CommitAction : TypedAction<Writer, CommitAction> {
			std::unique_ptr<rocksdb::WriteBatch> batchToCommit;
			ThreadReturnPromise<Void> done;
			double startTime;
			bool getHistograms;
			double getTimeEstimate() const override { return SERVER_KNOBS->COMMIT_TIME_ESTIMATE; }
			CommitAction() {
				if (deterministicRandom()->random01() < SERVER_KNOBS->ROCKSDB_HISTOGRAMS_SAMPLE_RATE) {
					getHistograms = true;
					startTime = timer_monotonic();
				} else {
					getHistograms = false;
				}
			}
		};
		void action(CommitAction& a) {
			double commitBeginTime;
			if (a.getHistograms) {
				commitBeginTime = timer_monotonic();
				commitQueueWaitHistogram->sampleSeconds(commitBeginTime - a.startTime);
			}
			Standalone<VectorRef<KeyRangeRef>> deletes;
			DeleteVisitor dv(deletes, deletes.arena());
			rocksdb::Status s = a.batchToCommit->Iterate(&dv);
			if (!s.ok()) {
				logRocksDBError(s, "CommitDeleteVisitor");
				a.done.sendError(statusToError(s));
				return;
			}
			// If there are any range deletes, we should have added them to be deleted.
			ASSERT(!deletes.empty() || !a.batchToCommit->HasDeleteRange());
			rocksdb::WriteOptions options;
			options.sync = !SERVER_KNOBS->ROCKSDB_UNSAFE_AUTO_FSYNC;

			double writeBeginTime = a.getHistograms ? timer_monotonic() : 0;
			if (rateLimiter) {
				// Controls the total write rate of compaction and flush in bytes per second.
				// Request for batchToCommit bytes. If this request cannot be satisfied, the call is blocked.
				rateLimiter->Request(a.batchToCommit->GetDataSize() /* bytes */, rocksdb::Env::IO_HIGH);
			}
			s = db->Write(options, a.batchToCommit.get());
			readIterPool->update();
			if (a.getHistograms) {
				writeHistogram->sampleSeconds(timer_monotonic() - writeBeginTime);
			}

			if (!s.ok()) {
				logRocksDBError(s, "Commit");
				a.done.sendError(statusToError(s));
			} else {
				a.done.send(Void());

				double compactRangeBeginTime = a.getHistograms ? timer_monotonic() : 0;
				for (const auto& keyRange : deletes) {
					auto begin = toSlice(keyRange.begin);
					auto end = toSlice(keyRange.end);
					ASSERT(db->SuggestCompactRange(cf, &begin, &end).ok());
				}
				if (a.getHistograms) {
					deleteCompactRangeHistogram->sampleSeconds(timer_monotonic() - compactRangeBeginTime);
				}
			}
			if (a.getHistograms) {
				double currTime = timer_monotonic();
				commitActionHistogram->sampleSeconds(currTime - commitBeginTime);
				commitLatencyHistogram->sampleSeconds(currTime - a.startTime);
			}
		}

		struct CloseAction : TypedAction<Writer, CloseAction> {
			ThreadReturnPromise<Void> done;
			std::string path;
			bool deleteOnClose;
			CloseAction(std::string path, bool deleteOnClose) : path(path), deleteOnClose(deleteOnClose) {}
			double getTimeEstimate() const override { return SERVER_KNOBS->COMMIT_TIME_ESTIMATE; }
		};
		void action(CloseAction& a) {
			readIterPool.reset();
			if (db == nullptr) {
				a.done.send(Void());
				return;
			}
			auto s = db->Close();
			if (!s.ok()) {
				logRocksDBError(s, "Close");
			}
			if (a.deleteOnClose) {
				std::set<std::string> columnFamilies{ "default" };
				columnFamilies.insert(SERVER_KNOBS->DEFAULT_FDB_ROCKSDB_COLUMN_FAMILY);
				std::vector<rocksdb::ColumnFamilyDescriptor> descriptors;
				for (const std::string name : columnFamilies) {
					descriptors.push_back(rocksdb::ColumnFamilyDescriptor{ name, getCFOptions() });
				}
				s = rocksdb::DestroyDB(a.path, getOptions(), descriptors);
				if (!s.ok()) {
					logRocksDBError(s, "Destroy");
				} else {
					TraceEvent("RocksDB").detail("Path", a.path).detail("Method", "Destroy");
				}
			}
			TraceEvent("RocksDB").detail("Path", a.path).detail("Method", "Close");
			a.done.send(Void());
		}

		struct CheckpointAction : TypedAction<Writer, CheckpointAction> {
			CheckpointAction(const GetCheckpointRequest& request, const std::string& checkpointDir)
			  : request(request), checkpointDir(checkpointDir) {}

			double getTimeEstimate() const override { return SERVER_KNOBS->COMMIT_TIME_ESTIMATE; }

			const GetCheckpointRequest request;
			const std::string checkpointDir;
			ThreadReturnPromise<CheckpointMetaData> reply;
		};

		void action(CheckpointAction& a) {
			TraceEvent("RocksDBServeCheckpointBegin", id)
			    .detail("MinVersion", a.request.minVersion)
			    .detail("Range", a.request.range.toString())
			    .detail("Format", static_cast<int>(a.request.format))
			    .detail("CheckpointDir", a.checkpointDir);

			rocksdb::Checkpoint* checkpoint;
			rocksdb::Status s = rocksdb::Checkpoint::Create(db, &checkpoint);
			if (!s.ok()) {
				logRocksDBError(s, "Checkpoint");
				a.reply.sendError(statusToError(s));
				return;
			}

			rocksdb::PinnableSlice value;
			rocksdb::ReadOptions readOptions = getReadOptions();
			s = db->Get(readOptions, cf, toSlice(persistVersion), &value);

			if (!s.ok() && !s.IsNotFound()) {
				logRocksDBError(s, "Checkpoint");
				a.reply.sendError(statusToError(s));
				return;
			}

			const Version version = s.IsNotFound()
			                            ? latestVersion
			                            : BinaryReader::fromStringRef<Version>(toStringRef(value), Unversioned());
			// TODO: set the range as the actual shard range.
			CheckpointMetaData res(version, a.request.range, a.request.format, deterministicRandom()->randomUniqueID());
			const std::string& checkpointDir = a.checkpointDir;

			if (a.request.format == RocksDBColumnFamily) {
				rocksdb::ExportImportFilesMetaData* pMetadata;
				platform::eraseDirectoryRecursive(checkpointDir);
				const std::string cwd = platform::getWorkingDirectory() + "/";
				s = checkpoint->ExportColumnFamily(cf, checkpointDir, &pMetadata);

				if (!s.ok()) {
					logRocksDBError(s, "Checkpoint");
					a.reply.sendError(statusToError(s));
					return;
				}

				populateMetaData(&res, *pMetadata);
				delete pMetadata;
			} else {
				throw not_implemented();
			}

			TraceEvent("RocksDBServeCheckpointSuccess", id).detail("CheckpointMetaData", res.toString());
			a.reply.send(res);
		}

		struct RestoreAction : TypedAction<Writer, RestoreAction> {
			RestoreAction(const std::string& path, const CheckpointMetaData& checkpoint)
			  : path(path), checkpoint(checkpoint) {}

			double getTimeEstimate() const override { return SERVER_KNOBS->COMMIT_TIME_ESTIMATE; }

			const std::string path;
			const CheckpointMetaData checkpoint;
			ThreadReturnPromise<Void> done;
		};

		void action(RestoreAction& a) {
			TraceEvent("RocksDBServeRestoreBegin", id)
			    .detail("Path", a.path)
			    .detail("CheckpointMetaData", a.checkpoint.toString());

			if (a.checkpoint.format == RocksDBColumnFamily) {
				auto options = getOptions();
				rocksdb::Status status = rocksdb::DB::Open(options, a.path, &db);

				if (!status.ok()) {
					logRocksDBError(status, "Restore");
					a.done.sendError(statusToError(status));
					return;
				}

				rocksdb::ExportImportFilesMetaData metaData = getMetaData(a.checkpoint);
				rocksdb::ImportColumnFamilyOptions importOptions;
				importOptions.move_files = true;
				status = db->CreateColumnFamilyWithImport(
				    getCFOptions(), SERVER_KNOBS->DEFAULT_FDB_ROCKSDB_COLUMN_FAMILY, importOptions, metaData, &cf);

				if (!status.ok()) {
					logRocksDBError(status, "Restore");
					a.done.sendError(statusToError(status));
				} else {
					TraceEvent(SevInfo, "RocksDB").detail("Path", a.path).detail("Method", "Restore");
					a.done.send(Void());
				}
			} else {
				throw not_implemented();
			}
		}
	};

	struct Reader : IThreadPoolReceiver {
		DB& db;
		CF& cf;
		double readValueTimeout;
		double readValuePrefixTimeout;
		double readRangeTimeout;
		Reference<Histogram> readRangeLatencyHistogram;
		Reference<Histogram> readValueLatencyHistogram;
		Reference<Histogram> readPrefixLatencyHistogram;
		Reference<Histogram> readRangeActionHistogram;
		Reference<Histogram> readValueActionHistogram;
		Reference<Histogram> readPrefixActionHistogram;
		Reference<Histogram> readRangeQueueWaitHistogram;
		Reference<Histogram> readValueQueueWaitHistogram;
		Reference<Histogram> readPrefixQueueWaitHistogram;
		Reference<Histogram> readRangeNewIteratorHistogram;
		Reference<Histogram> readValueGetHistogram;
		Reference<Histogram> readPrefixGetHistogram;
		std::shared_ptr<ReadIteratorPool> readIterPool;

		explicit Reader(DB& db, CF& cf, std::shared_ptr<ReadIteratorPool> readIterPool)
		  : db(db), cf(cf), readIterPool(readIterPool),
		    readRangeLatencyHistogram(Histogram::getHistogram(ROCKSDBSTORAGE_HISTOGRAM_GROUP,
		                                                      ROCKSDB_READRANGE_LATENCY_HISTOGRAM,
		                                                      Histogram::Unit::microseconds)),
		    readValueLatencyHistogram(Histogram::getHistogram(ROCKSDBSTORAGE_HISTOGRAM_GROUP,
		                                                      ROCKSDB_READVALUE_LATENCY_HISTOGRAM,
		                                                      Histogram::Unit::microseconds)),
		    readPrefixLatencyHistogram(Histogram::getHistogram(ROCKSDBSTORAGE_HISTOGRAM_GROUP,
		                                                       ROCKSDB_READPREFIX_LATENCY_HISTOGRAM,
		                                                       Histogram::Unit::microseconds)),
		    readRangeActionHistogram(Histogram::getHistogram(ROCKSDBSTORAGE_HISTOGRAM_GROUP,
		                                                     ROCKSDB_READRANGE_ACTION_HISTOGRAM,
		                                                     Histogram::Unit::microseconds)),
		    readValueActionHistogram(Histogram::getHistogram(ROCKSDBSTORAGE_HISTOGRAM_GROUP,
		                                                     ROCKSDB_READVALUE_ACTION_HISTOGRAM,
		                                                     Histogram::Unit::microseconds)),
		    readPrefixActionHistogram(Histogram::getHistogram(ROCKSDBSTORAGE_HISTOGRAM_GROUP,
		                                                      ROCKSDB_READPREFIX_ACTION_HISTOGRAM,
		                                                      Histogram::Unit::microseconds)),
		    readRangeQueueWaitHistogram(Histogram::getHistogram(ROCKSDBSTORAGE_HISTOGRAM_GROUP,
		                                                        ROCKSDB_READRANGE_QUEUEWAIT_HISTOGRAM,
		                                                        Histogram::Unit::microseconds)),
		    readValueQueueWaitHistogram(Histogram::getHistogram(ROCKSDBSTORAGE_HISTOGRAM_GROUP,
		                                                        ROCKSDB_READVALUE_QUEUEWAIT_HISTOGRAM,
		                                                        Histogram::Unit::microseconds)),
		    readPrefixQueueWaitHistogram(Histogram::getHistogram(ROCKSDBSTORAGE_HISTOGRAM_GROUP,
		                                                         ROCKSDB_READPREFIX_QUEUEWAIT_HISTOGRAM,
		                                                         Histogram::Unit::microseconds)),
		    readRangeNewIteratorHistogram(Histogram::getHistogram(ROCKSDBSTORAGE_HISTOGRAM_GROUP,
		                                                          ROCKSDB_READRANGE_NEWITERATOR_HISTOGRAM,
		                                                          Histogram::Unit::microseconds)),
		    readValueGetHistogram(Histogram::getHistogram(ROCKSDBSTORAGE_HISTOGRAM_GROUP,
		                                                  ROCKSDB_READVALUE_GET_HISTOGRAM,
		                                                  Histogram::Unit::microseconds)),
		    readPrefixGetHistogram(Histogram::getHistogram(ROCKSDBSTORAGE_HISTOGRAM_GROUP,
		                                                   ROCKSDB_READPREFIX_GET_HISTOGRAM,
		                                                   Histogram::Unit::microseconds)) {
			if (g_network->isSimulated()) {
				// In simulation, increasing the read operation timeouts to 5 minutes, as some of the tests have
				// very high load and single read thread cannot process all the load within the timeouts.
				readValueTimeout = 5 * 60;
				readValuePrefixTimeout = 5 * 60;
				readRangeTimeout = 5 * 60;
			} else {
				readValueTimeout = SERVER_KNOBS->ROCKSDB_READ_VALUE_TIMEOUT;
				readValuePrefixTimeout = SERVER_KNOBS->ROCKSDB_READ_VALUE_PREFIX_TIMEOUT;
				readRangeTimeout = SERVER_KNOBS->ROCKSDB_READ_RANGE_TIMEOUT;
			}
		}

		void init() override {}

		struct ReadValueAction : TypedAction<Reader, ReadValueAction> {
			Key key;
			Optional<UID> debugID;
			double startTime;
			bool getHistograms;
			ThreadReturnPromise<Optional<Value>> result;
			ReadValueAction(KeyRef key, Optional<UID> debugID)
			  : key(key), debugID(debugID), startTime(timer_monotonic()),
			    getHistograms(
			        (deterministicRandom()->random01() < SERVER_KNOBS->ROCKSDB_HISTOGRAMS_SAMPLE_RATE) ? true : false) {
			}
			double getTimeEstimate() const override { return SERVER_KNOBS->READ_VALUE_TIME_ESTIMATE; }
		};
		void action(ReadValueAction& a) {
			ASSERT(cf != nullptr);
			double readBeginTime = timer_monotonic();
			if (a.getHistograms) {
				readValueQueueWaitHistogram->sampleSeconds(readBeginTime - a.startTime);
			}
			Optional<TraceBatch> traceBatch;
			if (a.debugID.present()) {
				traceBatch = { TraceBatch{} };
				traceBatch.get().addEvent("GetValueDebug", a.debugID.get().first(), "Reader.Before");
			}
			if (readBeginTime - a.startTime > readValueTimeout) {
				TraceEvent(SevWarn, "RocksDBError")
				    .detail("Error", "Read value request timedout")
				    .detail("Method", "ReadValueAction")
				    .detail("Timeout value", readValueTimeout);
				a.result.sendError(transaction_too_old());
				return;
			}

			rocksdb::PinnableSlice value;
			auto options = getReadOptions();
			uint64_t deadlineMircos =
			    db->GetEnv()->NowMicros() + (readValueTimeout - (readBeginTime - a.startTime)) * 1000000;
			std::chrono::seconds deadlineSeconds(deadlineMircos / 1000000);
			options.deadline = std::chrono::duration_cast<std::chrono::microseconds>(deadlineSeconds);

			double dbGetBeginTime = a.getHistograms ? timer_monotonic() : 0;
			auto s = db->Get(options, cf, toSlice(a.key), &value);
			if (!s.ok() && !s.IsNotFound()) {
				logRocksDBError(s, "ReadValue");
				a.result.sendError(statusToError(s));
				return;
			}

			if (a.getHistograms) {
				readValueGetHistogram->sampleSeconds(timer_monotonic() - dbGetBeginTime);
			}

			if (a.debugID.present()) {
				traceBatch.get().addEvent("GetValueDebug", a.debugID.get().first(), "Reader.After");
				traceBatch.get().dump();
			}
			if (s.ok()) {
				a.result.send(Value(toStringRef(value)));
			} else if (s.IsNotFound()) {
				a.result.send(Optional<Value>());
			} else {
				logRocksDBError(s, "ReadValue");
				a.result.sendError(statusToError(s));
			}

			if (a.getHistograms) {
				double currTime = timer_monotonic();
				readValueActionHistogram->sampleSeconds(currTime - readBeginTime);
				readValueLatencyHistogram->sampleSeconds(currTime - a.startTime);
			}
		}

		struct ReadValuePrefixAction : TypedAction<Reader, ReadValuePrefixAction> {
			Key key;
			int maxLength;
			Optional<UID> debugID;
			double startTime;
			bool getHistograms;
			ThreadReturnPromise<Optional<Value>> result;
			ReadValuePrefixAction(Key key, int maxLength, Optional<UID> debugID)
			  : key(key), maxLength(maxLength), debugID(debugID), startTime(timer_monotonic()),
			    getHistograms(
			        (deterministicRandom()->random01() < SERVER_KNOBS->ROCKSDB_HISTOGRAMS_SAMPLE_RATE) ? true : false) {
			}
			double getTimeEstimate() const override { return SERVER_KNOBS->READ_VALUE_TIME_ESTIMATE; }
		};
		void action(ReadValuePrefixAction& a) {
			double readBeginTime = timer_monotonic();
			if (a.getHistograms) {
				readPrefixQueueWaitHistogram->sampleSeconds(readBeginTime - a.startTime);
			}
			Optional<TraceBatch> traceBatch;
			if (a.debugID.present()) {
				traceBatch = { TraceBatch{} };
				traceBatch.get().addEvent("GetValuePrefixDebug",
				                          a.debugID.get().first(),
				                          "Reader.Before"); //.detail("TaskID", g_network->getCurrentTask());
			}
			if (readBeginTime - a.startTime > readValuePrefixTimeout) {
				TraceEvent(SevWarn, "RocksDBError")
				    .detail("Error", "Read value prefix request timedout")
				    .detail("Method", "ReadValuePrefixAction")
				    .detail("Timeout value", readValuePrefixTimeout);
				a.result.sendError(transaction_too_old());
				return;
			}

			rocksdb::PinnableSlice value;
			auto options = getReadOptions();
			uint64_t deadlineMircos =
			    db->GetEnv()->NowMicros() + (readValuePrefixTimeout - (readBeginTime - a.startTime)) * 1000000;
			std::chrono::seconds deadlineSeconds(deadlineMircos / 1000000);
			options.deadline = std::chrono::duration_cast<std::chrono::microseconds>(deadlineSeconds);

			double dbGetBeginTime = a.getHistograms ? timer_monotonic() : 0;
			auto s = db->Get(options, cf, toSlice(a.key), &value);
			if (a.getHistograms) {
				readPrefixGetHistogram->sampleSeconds(timer_monotonic() - dbGetBeginTime);
			}

			if (a.debugID.present()) {
				traceBatch.get().addEvent("GetValuePrefixDebug",
				                          a.debugID.get().first(),
				                          "Reader.After"); //.detail("TaskID", g_network->getCurrentTask());
				traceBatch.get().dump();
			}
			if (s.ok()) {
				a.result.send(Value(StringRef(reinterpret_cast<const uint8_t*>(value.data()),
				                              std::min(value.size(), size_t(a.maxLength)))));
			} else if (s.IsNotFound()) {
				a.result.send(Optional<Value>());
			} else {
				logRocksDBError(s, "ReadValuePrefix");
				a.result.sendError(statusToError(s));
			}
			if (a.getHistograms) {
				double currTime = timer_monotonic();
				readPrefixActionHistogram->sampleSeconds(currTime - readBeginTime);
				readPrefixLatencyHistogram->sampleSeconds(currTime - a.startTime);
			}
		}

		struct ReadRangeAction : TypedAction<Reader, ReadRangeAction>, FastAllocated<ReadRangeAction> {
			KeyRange keys;
			int rowLimit, byteLimit;
			double startTime;
			bool getHistograms;
			ThreadReturnPromise<RangeResult> result;
			ReadRangeAction(KeyRange keys, int rowLimit, int byteLimit)
			  : keys(keys), rowLimit(rowLimit), byteLimit(byteLimit), startTime(timer_monotonic()),
			    getHistograms(
			        (deterministicRandom()->random01() < SERVER_KNOBS->ROCKSDB_HISTOGRAMS_SAMPLE_RATE) ? true : false) {
			}
			double getTimeEstimate() const override { return SERVER_KNOBS->READ_RANGE_TIME_ESTIMATE; }
		};
		void action(ReadRangeAction& a) {
			double readBeginTime = timer_monotonic();
			if (a.getHistograms) {
				readRangeQueueWaitHistogram->sampleSeconds(readBeginTime - a.startTime);
			}
			if (readBeginTime - a.startTime > readRangeTimeout) {
				TraceEvent(SevWarn, "RocksDBError")
				    .detail("Error", "Read range request timedout")
				    .detail("Method", "ReadRangeAction")
				    .detail("Timeout value", readRangeTimeout);
				a.result.sendError(transaction_too_old());
				return;
			}

			RangeResult result;
			if (a.rowLimit == 0 || a.byteLimit == 0) {
				a.result.send(result);
			}
			int accumulatedBytes = 0;
			rocksdb::Status s;
			if (a.rowLimit >= 0) {
				double iterCreationBeginTime = a.getHistograms ? timer_monotonic() : 0;
				ReadIterator readIter = readIterPool->getIterator();
				if (a.getHistograms) {
					readRangeNewIteratorHistogram->sampleSeconds(timer_monotonic() - iterCreationBeginTime);
				}
				auto cursor = readIter.iter;
				cursor->Seek(toSlice(a.keys.begin));
				while (cursor->Valid() && toStringRef(cursor->key()) < a.keys.end) {
					KeyValueRef kv(toStringRef(cursor->key()), toStringRef(cursor->value()));
					accumulatedBytes += sizeof(KeyValueRef) + kv.expectedSize();
					result.push_back_deep(result.arena(), kv);
					// Calling `cursor->Next()` is potentially expensive, so short-circut here just in case.
					if (result.size() >= a.rowLimit || accumulatedBytes >= a.byteLimit) {
						break;
					}
					if (timer_monotonic() - a.startTime > readRangeTimeout) {
						TraceEvent(SevWarn, "RocksDBError")
						    .detail("Error", "Read range request timedout")
						    .detail("Method", "ReadRangeAction")
						    .detail("Timeout value", readRangeTimeout);
						a.result.sendError(transaction_too_old());
						return;
					}
					cursor->Next();
				}
				s = cursor->status();
				readIterPool->returnIterator(readIter);
			} else {
				double iterCreationBeginTime = a.getHistograms ? timer_monotonic() : 0;
				ReadIterator readIter = readIterPool->getIterator();
				if (a.getHistograms) {
					readRangeNewIteratorHistogram->sampleSeconds(timer_monotonic() - iterCreationBeginTime);
				}
				auto cursor = readIter.iter;
				cursor->SeekForPrev(toSlice(a.keys.end));
				if (cursor->Valid() && toStringRef(cursor->key()) == a.keys.end) {
					cursor->Prev();
				}
				while (cursor->Valid() && toStringRef(cursor->key()) >= a.keys.begin) {
					KeyValueRef kv(toStringRef(cursor->key()), toStringRef(cursor->value()));
					accumulatedBytes += sizeof(KeyValueRef) + kv.expectedSize();
					result.push_back_deep(result.arena(), kv);
					// Calling `cursor->Prev()` is potentially expensive, so short-circut here just in case.
					if (result.size() >= -a.rowLimit || accumulatedBytes >= a.byteLimit) {
						break;
					}
					if (timer_monotonic() - a.startTime > readRangeTimeout) {
						TraceEvent(SevWarn, "RocksDBError")
						    .detail("Error", "Read range request timedout")
						    .detail("Method", "ReadRangeAction")
						    .detail("Timeout value", readRangeTimeout);
						a.result.sendError(transaction_too_old());
						return;
					}
					cursor->Prev();
				}
				s = cursor->status();
				readIterPool->returnIterator(readIter);
			}

			if (!s.ok()) {
				logRocksDBError(s, "ReadRange");
				a.result.sendError(statusToError(s));
				return;
			}
			result.more =
			    (result.size() == a.rowLimit) || (result.size() == -a.rowLimit) || (accumulatedBytes >= a.byteLimit);
			if (result.more) {
				result.readThrough = result[result.size() - 1].key;
			}
			a.result.send(result);
			if (a.getHistograms) {
				double currTime = timer_monotonic();
				readRangeActionHistogram->sampleSeconds(currTime - readBeginTime);
				readRangeLatencyHistogram->sampleSeconds(currTime - a.startTime);
			}
		}
	};

	DB db = nullptr;
	std::string path;
	rocksdb::ColumnFamilyHandle* defaultFdbCF = nullptr;
	UID id;
	Reference<IThreadPool> writeThread;
	Reference<IThreadPool> readThreads;
	Promise<Void> errorPromise;
	Promise<Void> closePromise;
	Future<Void> openFuture;
	std::unique_ptr<rocksdb::WriteBatch> writeBatch;
	Optional<Future<Void>> metrics;
	FlowLock readSemaphore;
	int numReadWaiters;
	FlowLock fetchSemaphore;
	int numFetchWaiters;
	std::shared_ptr<ReadIteratorPool> readIterPool;

	struct Counters {
		CounterCollection cc;
		Counter immediateThrottle;
		Counter failedToAcquire;

		Counters()
		  : cc("RocksDBThrottle"), immediateThrottle("ImmediateThrottle", cc), failedToAcquire("failedToAcquire", cc) {}
	};

	Counters counters;

	explicit RocksDBKeyValueStore(const std::string& path, UID id)
	  : path(path), id(id), readIterPool(new ReadIteratorPool(db, defaultFdbCF, path)),
	    readSemaphore(SERVER_KNOBS->ROCKSDB_READ_QUEUE_SOFT_MAX),
	    fetchSemaphore(SERVER_KNOBS->ROCKSDB_FETCH_QUEUE_SOFT_MAX),
	    numReadWaiters(SERVER_KNOBS->ROCKSDB_READ_QUEUE_HARD_MAX - SERVER_KNOBS->ROCKSDB_READ_QUEUE_SOFT_MAX),
	    numFetchWaiters(SERVER_KNOBS->ROCKSDB_FETCH_QUEUE_HARD_MAX - SERVER_KNOBS->ROCKSDB_FETCH_QUEUE_SOFT_MAX) {
		// In simluation, run the reader/writer threads as Coro threads (i.e. in the network thread. The storage engine
		// is still multi-threaded as background compaction threads are still present. Reads/writes to disk will also
		// block the network thread in a way that would be unacceptable in production but is a necessary evil here. When
		// performing the reads in background threads in simulation, the event loop thinks there is no work to do and
		// advances time faster than 1 sec/sec. By the time the blocking read actually finishes, simulation has advanced
		// time by more than 5 seconds, so every read fails with a transaction_too_old error. Doing blocking IO on the
		// main thread solves this issue. There are almost certainly better fixes, but my goal was to get a less
		// invasive change merged first and work on a more realistic version if/when we think that would provide
		// substantially more confidence in the correctness.
		// TODO: Adapt the simulation framework to not advance time quickly when background reads/writes are occurring.
		if (g_network->isSimulated()) {
			writeThread = CoroThreadPool::createThreadPool();
			readThreads = CoroThreadPool::createThreadPool();
		} else {
			writeThread = createGenericThreadPool();
			readThreads = createGenericThreadPool();
		}
		writeThread->addThread(new Writer(db, defaultFdbCF, id, readIterPool), "fdb-rocksdb-wr");
		TraceEvent("RocksDBReadThreads").detail("KnobRocksDBReadParallelism", SERVER_KNOBS->ROCKSDB_READ_PARALLELISM);
		for (unsigned i = 0; i < SERVER_KNOBS->ROCKSDB_READ_PARALLELISM; ++i) {
			readThreads->addThread(new Reader(db, defaultFdbCF, readIterPool), "fdb-rocksdb-re");
		}
	}

	Future<Void> getError() const override { return errorPromise.getFuture(); }

	ACTOR static void doClose(RocksDBKeyValueStore* self, bool deleteOnClose) {
		// The metrics future retains a reference to the DB, so stop it before we delete it.
		self->metrics.reset();

		wait(self->readThreads->stop());
		self->readIterPool.reset();
		auto a = new Writer::CloseAction(self->path, deleteOnClose);
		auto f = a->done.getFuture();
		self->writeThread->post(a);
		wait(f);
		wait(self->writeThread->stop());
		if (self->closePromise.canBeSet())
			self->closePromise.send(Void());
		if (self->errorPromise.canBeSet())
			self->errorPromise.send(Never());
		delete self;
	}

	Future<Void> onClosed() const override { return closePromise.getFuture(); }

	void dispose() override { doClose(this, true); }

	void close() override { doClose(this, false); }

	KeyValueStoreType getType() const override { return KeyValueStoreType(KeyValueStoreType::SSD_ROCKSDB_V1); }

	Future<Void> init() override {
		if (openFuture.isValid()) {
			return openFuture;
		}
		auto a = std::make_unique<Writer::OpenAction>(path, metrics, &readSemaphore, &fetchSemaphore);
		openFuture = a->done.getFuture();
		writeThread->post(a.release());
		return openFuture;
	}

	void set(KeyValueRef kv, const Arena*) override {
		if (writeBatch == nullptr) {
			writeBatch.reset(new rocksdb::WriteBatch());
		}
		ASSERT(defaultFdbCF != nullptr);
		writeBatch->Put(defaultFdbCF, toSlice(kv.key), toSlice(kv.value));
	}

	void clear(KeyRangeRef keyRange, const Arena*) override {
		if (writeBatch == nullptr) {
			writeBatch.reset(new rocksdb::WriteBatch());
		}

		ASSERT(defaultFdbCF != nullptr);

		if (keyRange.singleKeyRange()) {
			writeBatch->Delete(defaultFdbCF, toSlice(keyRange.begin));
		} else {
			writeBatch->DeleteRange(defaultFdbCF, toSlice(keyRange.begin), toSlice(keyRange.end));
		}
	}

	Future<Void> commit(bool) override {
		// If there is nothing to write, don't write.
		if (writeBatch == nullptr) {
			return Void();
		}
		auto a = new Writer::CommitAction();
		a->batchToCommit = std::move(writeBatch);
		auto res = a->done.getFuture();
		writeThread->post(a);
		return res;
	}

	void checkWaiters(const FlowLock& semaphore, int maxWaiters) {
		if (semaphore.waiters() > maxWaiters) {
			++counters.immediateThrottle;
			throw server_overloaded();
		}
	}

	// We don't throttle eager reads and reads to the FF keyspace because FDB struggles when those reads fail.
	// Thus far, they have been low enough volume to not cause an issue.
	static bool shouldThrottle(IKeyValueStore::ReadType type, KeyRef key) {
		return type != IKeyValueStore::ReadType::EAGER && !(key.startsWith(systemKeys.begin));
	}

	ACTOR template <class Action>
	static Future<Optional<Value>> read(Action* action, FlowLock* semaphore, IThreadPool* pool, Counter* counter) {
		state std::unique_ptr<Action> a(action);
		state Optional<Void> slot = wait(timeout(semaphore->take(), SERVER_KNOBS->ROCKSDB_READ_QUEUE_WAIT));
		if (!slot.present()) {
			++(*counter);
			throw server_overloaded();
		}

		state FlowLock::Releaser release(*semaphore);

		auto fut = a->result.getFuture();
		pool->post(a.release());
		Optional<Value> result = wait(fut);

		return result;
	}

	Future<Optional<Value>> readValue(KeyRef key, IKeyValueStore::ReadType type, Optional<UID> debugID) override {
		if (!shouldThrottle(type, key)) {
			auto a = new Reader::ReadValueAction(key, debugID);
			auto res = a->result.getFuture();
			readThreads->post(a);
			return res;
		}

		auto& semaphore = (type == IKeyValueStore::ReadType::FETCH) ? fetchSemaphore : readSemaphore;
		int maxWaiters = (type == IKeyValueStore::ReadType::FETCH) ? numFetchWaiters : numReadWaiters;

		checkWaiters(semaphore, maxWaiters);
		auto a = std::make_unique<Reader::ReadValueAction>(key, debugID);
		return read(a.release(), &semaphore, readThreads.getPtr(), &counters.failedToAcquire);
	}

	Future<Optional<Value>> readValuePrefix(KeyRef key,
	                                        int maxLength,
	                                        IKeyValueStore::ReadType type,
	                                        Optional<UID> debugID) override {
		if (!shouldThrottle(type, key)) {
			auto a = new Reader::ReadValuePrefixAction(key, maxLength, debugID);
			auto res = a->result.getFuture();
			readThreads->post(a);
			return res;
		}

		auto& semaphore = (type == IKeyValueStore::ReadType::FETCH) ? fetchSemaphore : readSemaphore;
		int maxWaiters = (type == IKeyValueStore::ReadType::FETCH) ? numFetchWaiters : numReadWaiters;

		checkWaiters(semaphore, maxWaiters);
		auto a = std::make_unique<Reader::ReadValuePrefixAction>(key, maxLength, debugID);
		return read(a.release(), &semaphore, readThreads.getPtr(), &counters.failedToAcquire);
	}

	ACTOR static Future<Standalone<RangeResultRef>> read(Reader::ReadRangeAction* action,
	                                                     FlowLock* semaphore,
	                                                     IThreadPool* pool,
	                                                     Counter* counter) {
		state std::unique_ptr<Reader::ReadRangeAction> a(action);
		state Optional<Void> slot = wait(timeout(semaphore->take(), SERVER_KNOBS->ROCKSDB_READ_QUEUE_WAIT));
		if (!slot.present()) {
			++(*counter);
			throw server_overloaded();
		}

		state FlowLock::Releaser release(*semaphore);

		auto fut = a->result.getFuture();
		pool->post(a.release());
		Standalone<RangeResultRef> result = wait(fut);

		return result;
	}

	Future<RangeResult> readRange(KeyRangeRef keys,
	                              int rowLimit,
	                              int byteLimit,
	                              IKeyValueStore::ReadType type) override {
		if (!shouldThrottle(type, keys.begin)) {
			auto a = new Reader::ReadRangeAction(keys, rowLimit, byteLimit);
			auto res = a->result.getFuture();
			readThreads->post(a);
			return res;
		}

		auto& semaphore = (type == IKeyValueStore::ReadType::FETCH) ? fetchSemaphore : readSemaphore;
		int maxWaiters = (type == IKeyValueStore::ReadType::FETCH) ? numFetchWaiters : numReadWaiters;

		checkWaiters(semaphore, maxWaiters);
		auto a = std::make_unique<Reader::ReadRangeAction>(keys, rowLimit, byteLimit);
		return read(a.release(), &semaphore, readThreads.getPtr(), &counters.failedToAcquire);
	}

	StorageBytes getStorageBytes() const override {
		uint64_t live = 0;
		ASSERT(db->GetIntProperty(rocksdb::DB::Properties::kLiveSstFilesSize, &live));

		int64_t free;
		int64_t total;
		g_network->getDiskBytes(path, free, total);

		return StorageBytes(free, total, live, free);
	}

	Future<CheckpointMetaData> checkpoint(const GetCheckpointRequest& request,
	                                      const std::string& checkpointDir) override {
		auto a = new Writer::CheckpointAction(request, checkpointDir);

		auto res = a->reply.getFuture();
		writeThread->post(a);
		return res;
	}

	Future<Void> restore(const CheckpointMetaData& checkpoint) override {
		auto a = new Writer::RestoreAction(path, checkpoint);
		auto res = a->done.getFuture();
		writeThread->post(a);
		return res;
	}
};

} // namespace

#endif // SSD_ROCKSDB_EXPERIMENTAL

IKeyValueStore* keyValueStoreRocksDB(std::string const& path,
                                     UID logID,
                                     KeyValueStoreType storeType,
                                     bool checkChecksums,
                                     bool checkIntegrity) {
#ifdef SSD_ROCKSDB_EXPERIMENTAL
	return new RocksDBKeyValueStore(path, logID);
#else
	TraceEvent(SevError, "RocksDBEngineInitFailure").detail("Reason", "Built without RocksDB");
	ASSERT(false);
	return nullptr;
#endif // SSD_ROCKSDB_EXPERIMENTAL
}

#ifdef SSD_ROCKSDB_EXPERIMENTAL
#include "flow/UnitTest.h"

namespace {

TEST_CASE("noSim/fdbserver/KeyValueStoreRocksDB/RocksDBBasic") {
	state const std::string rocksDBTestDir = "rocksdb-kvstore-basic-test-db";
	platform::eraseDirectoryRecursive(rocksDBTestDir);

	state IKeyValueStore* kvStore = new RocksDBKeyValueStore(rocksDBTestDir, deterministicRandom()->randomUniqueID());
	wait(kvStore->init());

	state StringRef foo = "foo"_sr;
	state StringRef bar = "ibar"_sr;
	kvStore->set({ foo, foo });
	kvStore->set({ keyAfter(foo), keyAfter(foo) });
	kvStore->set({ bar, bar });
	kvStore->set({ keyAfter(bar), keyAfter(bar) });
	wait(kvStore->commit(false));

	{
		Optional<Value> val = wait(kvStore->readValue(foo));
		ASSERT(foo == val.get());
	}

	// Test single key deletion.
	kvStore->clear(singleKeyRange(foo));
	wait(kvStore->commit(false));

	{
		Optional<Value> val = wait(kvStore->readValue(foo));
		ASSERT(!val.present());
	}

	{
		Optional<Value> val = wait(kvStore->readValue(keyAfter(foo)));
		ASSERT(keyAfter(foo) == val.get());
	}

	// Test range deletion.
	kvStore->clear(KeyRangeRef(keyAfter(foo), keyAfter(bar)));
	wait(kvStore->commit(false));

	{
		Optional<Value> val = wait(kvStore->readValue(bar));
		ASSERT(!val.present());
	}

	{
		Optional<Value> val = wait(kvStore->readValue(keyAfter(bar)));
		ASSERT(keyAfter(bar) == val.get());
	}

	Future<Void> closed = kvStore->onClosed();
	kvStore->close();
	wait(closed);

	platform::eraseDirectoryRecursive(rocksDBTestDir);
	return Void();
}

TEST_CASE("noSim/fdbserver/KeyValueStoreRocksDB/RocksDBReopen") {
	state const std::string rocksDBTestDir = "rocksdb-kvstore-reopen-test-db";
	platform::eraseDirectoryRecursive(rocksDBTestDir);

	state IKeyValueStore* kvStore = new RocksDBKeyValueStore(rocksDBTestDir, deterministicRandom()->randomUniqueID());
	wait(kvStore->init());

	kvStore->set({ LiteralStringRef("foo"), LiteralStringRef("bar") });
	wait(kvStore->commit(false));

	Optional<Value> val = wait(kvStore->readValue(LiteralStringRef("foo")));
	ASSERT(Optional<Value>(LiteralStringRef("bar")) == val);

	Future<Void> closed = kvStore->onClosed();
	kvStore->close();
	wait(closed);

	kvStore = new RocksDBKeyValueStore(rocksDBTestDir, deterministicRandom()->randomUniqueID());
	wait(kvStore->init());
	// Confirm that `init()` is idempotent.
	wait(kvStore->init());

	Optional<Value> val = wait(kvStore->readValue(LiteralStringRef("foo")));
	ASSERT(Optional<Value>(LiteralStringRef("bar")) == val);

	Future<Void> closed = kvStore->onClosed();
	kvStore->close();
	wait(closed);

	platform::eraseDirectoryRecursive(rocksDBTestDir);
	return Void();
}

TEST_CASE("noSim/fdbserver/KeyValueStoreRocksDB/CheckpointRestoreColumnFamily") {
	state std::string cwd = platform::getWorkingDirectory() + "/";
	state std::string rocksDBTestDir = "rocksdb-kvstore-br-test-db";
	platform::eraseDirectoryRecursive(rocksDBTestDir);

	state IKeyValueStore* kvStore = new RocksDBKeyValueStore(rocksDBTestDir, deterministicRandom()->randomUniqueID());
	wait(kvStore->init());

	kvStore->set({ LiteralStringRef("foo"), LiteralStringRef("bar") });
	wait(kvStore->commit(false));

	Optional<Value> val = wait(kvStore->readValue(LiteralStringRef("foo")));
	ASSERT(Optional<Value>(LiteralStringRef("bar")) == val);

	state rocksdb::DB* db = ((RocksDBKeyValueStore*)kvStore)->db;
	ASSERT(db != nullptr);

	platform::eraseDirectoryRecursive("checkpoint");
	state std::string checkpointDir = cwd + "checkpoint";

	GetCheckpointRequest request(latestVersion, allKeys, RocksDBColumnFamily, true);
	CheckpointMetaData metaData = wait(kvStore->checkpoint(request, checkpointDir));

	state std::string rocksDBRestoreDir = "rocksdb-kvstore-br-restore-db";
	platform::eraseDirectoryRecursive(rocksDBRestoreDir);

	state IKeyValueStore* kvStoreCopy =
	    new RocksDBKeyValueStore(rocksDBRestoreDir, deterministicRandom()->randomUniqueID());

	wait(kvStoreCopy->restore(metaData));

	Optional<Value> val = wait(kvStoreCopy->readValue(LiteralStringRef("foo")));
	ASSERT(Optional<Value>(LiteralStringRef("bar")) == val);

	return Void();
}

TEST_CASE("noSim/fdbserver/KeyValueStoreRocksDB/CheckpointRestoreSSTFile") {
	state std::string cwd = platform::getWorkingDirectory() + "/";
	state std::string rocksDBTestDir = "rocksdb-kvstore-brsst-test-db";
	platform::eraseDirectoryRecursive(rocksDBTestDir);
	state IKeyValueStore* kvStore = new RocksDBKeyValueStore(rocksDBTestDir, deterministicRandom()->randomUniqueID());
	wait(kvStore->init());

	kvStore->set({ LiteralStringRef("foo"), LiteralStringRef("bar") });
	wait(kvStore->commit(false));
	Optional<Value> val = wait(kvStore->readValue(LiteralStringRef("foo")));
	ASSERT(Optional<Value>(LiteralStringRef("bar")) == val);

	state DB db = ((RocksDBKeyValueStore*)kvStore)->db;
	ASSERT(db != nullptr);
	platform::eraseDirectoryRecursive("checkpoint");
	std::string checkpointDir = cwd + "checkpoint";
	rocksdb::Checkpoint* checkpoint;
	rocksdb::Status s = rocksdb::Checkpoint::Create(db, &checkpoint);
	ASSERT(s.ok());
	uint64_t checkpointSeq = -1;
	s = checkpoint->CreateCheckpoint(checkpointDir, /*log_size_for_flush=*/0, &checkpointSeq);
	std::cout << "Created Checkpoint" << std::endl;

	state ICheckpointReader* rocksReader =
	    checkpointReaderRocksDB(checkpointDir, deterministicRandom()->randomUniqueID());
	wait(rocksReader->init(allKeys));

	std::cout << "Init Checkpoint Done" << std::endl;

	state std::vector<KeyValue> kvs;
	try {
		loop {
			std::cout << "Getting next Batch" << std::endl;
			RangeResult res = wait(rocksReader->next(1024, 1024));
			if (res.size() == 0)
				break;
			std::cout << "Got new Batch" << std::endl;
			for (int i = 0; i < res.size(); ++i) {
				std::cout << res[i].key.toString() << ": " << res[i].value.toString() << std::endl;
				kvs.push_back(res[i]);
			}
		}
	} catch (Error& e) {
		std::cout << e.code() << std::endl;
	}

	state int i = 0;
	for (; i < kvs.size(); ++i) {
		Optional<Value> val = wait(kvStore->readValue(kvs[i].key));
		ASSERT(val.present());
		ASSERT(kvs[i].value == val.get());
	}

	return Void();
}

} // namespace

#endif // SSD_ROCKSDB_EXPERIMENTAL
