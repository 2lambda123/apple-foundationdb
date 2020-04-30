#include <rocksdb/env.h>
#include <rocksdb/db.h>
#include "flow/flow.h"
#include "fdbrpc/AsyncFileCached.actor.h"
#include "fdbserver/CoroFlow.h"
#include "fdbserver/IKeyValueStore.h"
#include "flow/actorcompiler.h" // has to be last include

namespace {

class FlowLogger : public rocksdb::Logger, public FastAllocated<FlowLogger> {
	UID id;
	std::string loggerName;
	size_t logSize = 0;
public:
	explicit FlowLogger(UID id, const std::string& loggerName, const rocksdb::InfoLogLevel log_level = rocksdb::InfoLogLevel::INFO_LEVEL)
		: rocksdb::Logger(log_level)
		, id(id)
		, loggerName(loggerName) {}

	rocksdb::Status Close() override { return rocksdb::Status::OK(); }

	void Logv(const char* fmtString, va_list ap) override {
		Logv(rocksdb::InfoLogLevel::INFO_LEVEL, fmtString, ap);
	}

	void Logv(const rocksdb::InfoLogLevel log_level, const char* fmtString, va_list ap) override {
		Severity sev;
		switch (log_level) {
			case rocksdb::InfoLogLevel::DEBUG_LEVEL:
				sev = SevDebug;
				break;
			case rocksdb::InfoLogLevel::INFO_LEVEL:
			case rocksdb::InfoLogLevel::HEADER_LEVEL:
			case rocksdb::InfoLogLevel::NUM_INFO_LOG_LEVELS:
				sev = SevInfo;
				break;
			case rocksdb::InfoLogLevel::WARN_LEVEL:
				sev = SevWarn;
				break;
			case rocksdb::InfoLogLevel::ERROR_LEVEL:
				sev = SevWarnAlways;
				break;
			case rocksdb::InfoLogLevel::FATAL_LEVEL:
				sev = SevError;
				break;
		}
		std::string outStr;
		auto sz = vsformat(outStr, fmtString, ap);
		if (sz < 0) {
			TraceEvent(SevError, "RocksDBLogFormatError", id)
				.detail("Logger", loggerName)
				.detail("FormatString", fmtString);
			return;
		}
		logSize += sz;
		TraceEvent(sev, "RocksDBLogMessage", id)
			.detail("Msg", outStr);
	}

	size_t GetLogFileSize() const override {
		return logSize;
	}
};

rocksdb::Slice toSlice(StringRef s) {
	return rocksdb::Slice(reinterpret_cast<const char*>(s.begin()), s.size());
}

StringRef toStringRef(rocksdb::Slice s) {
	return StringRef(reinterpret_cast<const uint8_t*>(s.data()), s.size());
}

struct RocksDBKeyValueStore : IKeyValueStore {
	using DB = rocksdb::DB*;
	using CF = rocksdb::ColumnFamilyHandle*;

	struct Writer : IThreadPoolReceiver {
		DB& db;
		UID id;
		std::unique_ptr<rocksdb::WriteBatch> writeBatch;
		rocksdb::WriteOptions writeOptions;

		explicit Writer(DB& db, UID id)
			: db(db), id(id), writeBatch(new rocksdb::WriteBatch{})
		{}

		~Writer() {
			if (db) {
				delete db;
			}
		}

		void init() override {}

		Error statusToError(const rocksdb::Status& s) {
			if (s == rocksdb::Status::IOError()) {
				return io_error();
			} else {
				return unknown_error();
			}
		}

		struct OpenAction : TypedAction<Writer, OpenAction> {
			rocksdb::Options options;
			rocksdb::ColumnFamilyOptions cfOptions;
			std::string path;
			ThreadReturnPromise<Void> done;
			
			double getTimeEstimate() {
				return SERVER_KNOBS->COMMIT_TIME_ESTIMATE;
			}
		};
		void action(OpenAction& a) {
			bool exists = directoryExists(a.path);
			a.options.create_if_missing = !exists;
			std::vector<rocksdb::ColumnFamilyDescriptor> defaultCF{ rocksdb::ColumnFamilyDescriptor{"default", rocksdb::ColumnFamilyOptions{}} };
			std::vector<rocksdb::ColumnFamilyHandle*> handle;
			auto status = rocksdb::DB::Open(a.options, a.path, defaultCF, &handle, &db);
			if (!status.ok()) {
				a.done.sendError(statusToError(status));
			} else {
				a.done.send(Void());
			}
		}

		struct SetAction : TypedAction<Writer, SetAction> {
			rocksdb::Slice key;
			rocksdb::Slice value;
			rocksdb::WriteOptions options;
			explicit SetAction(KeyValueRef kv)
				: key(toSlice(kv.key)), value(toSlice(kv.value))
			{}

			double getTimeEstimate() override { return SERVER_KNOBS->SET_TIME_ESTIMATE; }
		};
		void action(SetAction& a) {
			writeBatch->Put(db->DefaultColumnFamily(), a.key, a.value);
		}

		struct ClearAction : TypedAction<Writer, ClearAction> {
			rocksdb::Slice begin, end;
			explicit ClearAction(KeyRangeRef range)
				: begin(toSlice(range.begin)), end(toSlice(range.end))
			{}
			double getTimeEstimate() override { return SERVER_KNOBS->CLEAR_TIME_ESTIMATE; }
		};
		void action(ClearAction& a) {
			writeBatch->DeleteRange(db->DefaultColumnFamily(), a.begin, a.end);
		}

		struct CommitAction : TypedAction<Writer, CommitAction> {
			ThreadReturnPromise<Void> done;
			double getTimeEstimate() override { return SERVER_KNOBS->COMMIT_TIME_ESTIMATE; }
		};
		void action(CommitAction& a) {
			auto s = db->Write(rocksdb::WriteOptions{}, writeBatch.get());
			if (s.ok()) {
				writeBatch.reset(new rocksdb::WriteBatch{});
				s = db->FlushWAL(true);
			}
			if (!s.ok()) {
				a.done.sendError(statusToError(s));
			} else {
				a.done.send(Void());
			}
		}

		struct CloseAction : TypedAction<Writer, CloseAction> {
			ThreadReturnPromise<Void> done;
			double getTimeEstimate() override { return SERVER_KNOBS->COMMIT_TIME_ESTIMATE; }
		};
		void action(CloseAction& a) {
			db->Close();
			a.done.send(Void());
		}
	};

	struct Reader : IThreadPoolReceiver {
		DB& db;
		std::unique_ptr<rocksdb::Iterator> cursor = nullptr;
		rocksdb::ReadOptions readOptions;

		explicit Reader(DB& db)
			: db(db)
		{
			readOptions.total_order_seek = true;
		}

		void init() override {}

		struct ReadValueAction : TypedAction<Reader, ReadValueAction> {
			Key key;
			Optional<UID> debugID;
			ThreadReturnPromise<Optional<Value>> result;
			ReadValueAction(KeyRef key, Optional<UID> debugID)
				: key(key), debugID(debugID)
			{}
			double getTimeEstimate() override { return SERVER_KNOBS->READ_VALUE_TIME_ESTIMATE; }
		};
		void action(ReadValueAction& a) {
			if (a.debugID.present()) {
				g_traceBatch.addEvent("GetValueDebug", a.debugID.get().first(), "Reader.Before");
			}
			rocksdb::PinnableSlice value;
			auto s = db->Get(readOptions, db->DefaultColumnFamily(), toSlice(a.key), &value);
			if (a.debugID.present()) {
				g_traceBatch.addEvent("GetValueDebug", a.debugID.get().first(), "Reader.After");
			}
			if (s.ok()) {
				a.result.send(Value(toStringRef(value)));
			} else {
				a.result.send(Optional<Value>());
			}
		}

		struct ReadValuePrefixAction : TypedAction<Reader, ReadValuePrefixAction> {
			Key key;
			int maxLength;
			Optional<UID> debugID;
			ThreadReturnPromise<Optional<Value>> result;
			ReadValuePrefixAction(Key key, int maxLength, Optional<UID> debugID) : key(key), maxLength(maxLength), debugID(debugID) {};
			virtual double getTimeEstimate() { return SERVER_KNOBS->READ_VALUE_TIME_ESTIMATE; }
		};
		void action(ReadValuePrefixAction& a) {
			rocksdb::PinnableSlice value;
			if (a.debugID.present()) {
				g_traceBatch.addEvent("GetValuePrefixDebug", a.debugID.get().first(),
									  "Reader.Before"); //.detail("TaskID", g_network->getCurrentTask());
			}
			auto s = db->Get(readOptions, db->DefaultColumnFamily(), toSlice(a.key), &value);
			if (a.debugID.present()) {
				g_traceBatch.addEvent("GetValuePrefixDebug", a.debugID.get().first(),
									  "Reader.After"); //.detail("TaskID", g_network->getCurrentTask());
			}
			if (s.ok()) {
				a.result.send(Value(StringRef(reinterpret_cast<const uint8_t*>(value.data()),
											  std::min(value.size(), size_t(a.maxLength)))));
			} else {
				a.result.send(Optional<Value>());
			}
		}

		struct ReadRangeAction : TypedAction<Reader, ReadRangeAction>, FastAllocated<ReadRangeAction> {
			KeyRange keys;
			int rowLimit, byteLimit;
			ThreadReturnPromise<Standalone<VectorRef<KeyValueRef>>> result;
			ReadRangeAction(KeyRange keys, int rowLimit, int byteLimit) : keys(keys), rowLimit(rowLimit), byteLimit(byteLimit) {}
			virtual double getTimeEstimate() { return SERVER_KNOBS->READ_RANGE_TIME_ESTIMATE; }
		};
		void action(ReadRangeAction& a) {
			if (!cursor) {
				cursor.reset(db->NewIterator(readOptions));
			}
			cursor->Seek(toSlice(a.keys.begin));
			Standalone<VectorRef<KeyValueRef>> result;
			int accumulatedBytes = 0;
			while (cursor->Valid() &&
				   toStringRef(cursor->key()) < a.keys.end &&
				   result.size() < a.rowLimit &&
				   accumulatedBytes < a.byteLimit) {
				KeyValueRef kv(toStringRef(cursor->key()), toStringRef(cursor->value()));
				accumulatedBytes += sizeof(KeyValueRef) + kv.expectedSize();
				result.push_back_deep(result.arena(), kv);
				cursor->Next();
			}
			a.result.send(result);
		}
	};

	DB db = nullptr;
	std::string path;
	UID id;
	size_t diskBytesUsed = 0;
	Reference<IThreadPool> writeThread;
	Reference<IThreadPool> readThreads;
	unsigned nReaders = 2;
	Promise<Void> errorPromise;
	Promise<Void> closePromise;

	explicit RocksDBKeyValueStore(const std::string& path, UID id)
		: path(path)
		, id(id)
	{
		writeThread = createGenericThreadPool();
		readThreads = createGenericThreadPool();
		writeThread->addThread(new Writer(db, id));
		for (unsigned i = 0; i < nReaders; ++i) {
			readThreads->addThread(new Reader(db));
		}
	}

	Future<Void> getError() override {
		return errorPromise.getFuture();
	}

	ACTOR static void doClose(RocksDBKeyValueStore* self, bool deleteOnClose) {
		state Promise<Void> closePromise = self->closePromise;
		wait(self->readThreads->stop());
		auto a = new Writer::CloseAction{};
		auto f = a->done.getFuture();
		self->writeThread->post(a);
		wait(f);
		wait(self->writeThread->stop());
		delete self;
		// TODO: delete data on close
		closePromise.send(Void());
	}

	Future<Void> onClosed() override {
		return closePromise.getFuture();
	}

	void dispose() override {
		doClose(this, true);
	}

	void close() override {
		doClose(this, false);
	}

	KeyValueStoreType getType() override {
		return KeyValueStoreType(KeyValueStoreType::SSD_ROCKSDB_V1);
	}

	Future<Void> init() override {
		std::unique_ptr<Writer::OpenAction> a(new Writer::OpenAction());
		a->path = path;
		auto res = a->done.getFuture();
		writeThread->post(a.release());
		return res;
	}

	void set(KeyValueRef kv, const Arena*) override {
		writeThread->post(new Writer::SetAction(kv));
	}

	void clear(KeyRangeRef keyRange, const Arena*) override {
		writeThread->post(new Writer::ClearAction(keyRange));
	}

	Future<Void> commit(bool) override {
		auto a = new Writer::CommitAction();
		auto res = a->done.getFuture();
		writeThread->post(a);
		return res;
	}

	Future<Optional<Value>> readValue(KeyRef key, Optional<UID> debugID) override {
		auto a = new Reader::ReadValueAction(key, debugID);
		auto res = a->result.getFuture();
		readThreads->post(a);
		return res;
	}

	Future<Optional<Value>> readValuePrefix(KeyRef key, int maxLength, Optional<UID> debugID) override {
		auto a = new Reader::ReadValuePrefixAction(key, maxLength, debugID);
		auto res = a->result.getFuture();
		readThreads->post(a);
		return res;
	}

	Future<Standalone<VectorRef<KeyValueRef>>> readRange(KeyRangeRef keys, int rowLimit, int byteLimit) override {
		auto a = new Reader::ReadRangeAction(keys, rowLimit, byteLimit);
		auto res = a->result.getFuture();
		readThreads->post(a);
		return res;
	}

	StorageBytes getStorageBytes() override {
		int64_t free;
		int64_t total;

		g_network->getDiskBytes(path, free, total);

		return StorageBytes(free, total, diskBytesUsed, free);
	}
};

} // namespace

IKeyValueStore* keyValueStoreRocksDB(std::string const& path, UID logID, KeyValueStoreType storeType, bool checkChecksums, bool checkIntegrity) {
	return new RocksDBKeyValueStore(path, logID);
}
