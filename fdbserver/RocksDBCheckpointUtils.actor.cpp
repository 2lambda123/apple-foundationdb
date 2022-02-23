#include "fdbserver/RocksDBCheckpointUtils.actor.h"

#ifdef SSD_ROCKSDB_EXPERIMENTAL

#include <rocksdb/db.h>
#include <rocksdb/env.h>
#include <rocksdb/options.h>
#include <rocksdb/rate_limiter.h>
#include <rocksdb/slice.h>
#include <rocksdb/slice_transform.h>
#include <rocksdb/statistics.h>
#include <rocksdb/table.h>
#include <rocksdb/types.h>
#include <rocksdb/version.h>

#endif // SSD_ROCKSDB_EXPERIMENTAL

#include "fdbclient/FDBTypes.h"
#include "fdbclient/NativeAPI.actor.h"
#include "fdbclient/StorageCheckpoint.h"
#include "fdbserver/CoroFlow.h"
#include "fdbserver/Knobs.h"
#include "flow/IThreadPool.h"
#include "flow/ThreadHelper.actor.h"
#include "flow/Trace.h"
#include "flow/flow.h"

#include "flow/actorcompiler.h" // has to be last include
#ifdef SSD_ROCKSDB_EXPERIMENTAL

// Enforcing rocksdb version to be 6.22.1 or greater.
static_assert(ROCKSDB_MAJOR >= 6, "Unsupported rocksdb version. Update the rocksdb to 6.22.1 version");
static_assert(ROCKSDB_MAJOR == 6 ? ROCKSDB_MINOR >= 22 : true,
              "Unsupported rocksdb version. Update the rocksdb to 6.22.1 version");
static_assert((ROCKSDB_MAJOR == 6 && ROCKSDB_MINOR == 22) ? ROCKSDB_PATCH >= 1 : true,
              "Unsupported rocksdb version. Update the rocksdb to 6.22.1 version");

#endif // SSD_ROCKSDB_EXPERIMENTAL

namespace {
#ifdef SSD_ROCKSDB_EXPERIMENTAL

using DB = rocksdb::DB*;
using CF = rocksdb::ColumnFamilyHandle*;

rocksdb::Slice toSlice(StringRef s) {
	return rocksdb::Slice(reinterpret_cast<const char*>(s.begin()), s.size());
}

StringRef toStringRef(rocksdb::Slice s) {
	return StringRef(reinterpret_cast<const uint8_t*>(s.data()), s.size());
}

rocksdb::ColumnFamilyOptions getCFOptions() {
	rocksdb::ColumnFamilyOptions options;
	return options;
}

rocksdb::Options getOptions() {
	rocksdb::Options options({}, getCFOptions());

	// options.avoid_unnecessary_blocking_io = true;
	options.create_if_missing = false;
	options.db_log_dir = SERVER_KNOBS->LOG_DIRECTORY;
	return options;
}

// Set some useful defaults desired for all reads.
rocksdb::ReadOptions getReadOptions() {
	rocksdb::ReadOptions options;
	options.background_purge_on_iterator_cleanup = true;
	return options;
}

void logRocksDBError(const rocksdb::Status& status, const std::string& method) {
	auto level = status.IsTimedOut() ? SevWarn : SevError;
	TraceEvent e(level, "RocksDBCheckpointReaderError");
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

class RocksDBCheckpointReader : public ICheckpointReader {
private:
	struct Reader : IThreadPoolReceiver {
		explicit Reader(DB& db) : db(db), cf(nullptr) {
			if (g_network->isSimulated()) {
				// In simulation, increasing the read operation timeouts to 5 minutes, as some of the tests have
				// very high load and single read thread cannot process all the load within the timeouts.
				readRangeTimeout = 5 * 60;
			} else {
				readRangeTimeout = SERVER_KNOBS->ROCKSDB_READ_RANGE_TIMEOUT;
			}
		}

		~Reader() override {
			// if (db) {
			// 	delete db;
			// }
		}

		void init() override {}

		struct OpenAction : TypedAction<Reader, OpenAction> {
			OpenAction(std::string path, KeyRange range) : path(std::move(path)), range(range) {}

			double getTimeEstimate() const override { return SERVER_KNOBS->COMMIT_TIME_ESTIMATE; }

			const std::string path;
			const KeyRange range;
			ThreadReturnPromise<Void> done;
		};

		void action(OpenAction& a) {
			ASSERT(cf == nullptr);

			std::cout << "Open RocksDB Checkpoint." << std::endl;
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

			std::vector<rocksdb::ColumnFamilyHandle*> handles;
			status = rocksdb::DB::OpenForReadOnly(options, a.path, descriptors, &handles, &db);

			std::cout << "Open RocksDB Checkpoint Status." << status.ToString() << std::endl;
			if (!status.ok()) {
				logRocksDBError(status, "OpenForReadOnly");
				a.done.sendError(statusToError(status));
				return;
			}

			for (rocksdb::ColumnFamilyHandle* handle : handles) {
				if (handle->GetName() == SERVER_KNOBS->DEFAULT_FDB_ROCKSDB_COLUMN_FAMILY) {
					cf = handle;
					break;
				}
			}

			TraceEvent(SevInfo, "RocksDBCheckpointReader")
			    .detail("Path", a.path)
			    .detail("Method", "OpenForReadOnly")
			    .detail("ColumnFamily", cf->GetName());

			ASSERT(db != nullptr && cf != nullptr);

			std::cout << "Init Iterator." << std::endl;

			begin = toSlice(a.range.begin);
			end = toSlice(a.range.end);

			rocksdb::ReadOptions readOptions = getReadOptions();
			readOptions.iterate_upper_bound = &end;
			cursor = std::unique_ptr<rocksdb::Iterator>(db->NewIterator(readOptions, cf));
			cursor->Seek(begin);

			a.done.send(Void());
		}

		struct CloseAction : TypedAction<Reader, CloseAction> {
			CloseAction(std::string path, bool deleteOnClose) : path(path), deleteOnClose(deleteOnClose) {}
			double getTimeEstimate() const override { return SERVER_KNOBS->COMMIT_TIME_ESTIMATE; }

			std::string path;
			bool deleteOnClose;
			ThreadReturnPromise<Void> done;
		};

		void action(CloseAction& a) {
			if (db == nullptr) {
				a.done.send(Void());
				return;
			}

			rocksdb::Status s = db->Close();
			if (!s.ok()) {
				logRocksDBError(s, "Close");
			}

			if (a.deleteOnClose) {
				std::set<std::string> columnFamilies{ "default" };
				columnFamilies.insert(SERVER_KNOBS->DEFAULT_FDB_ROCKSDB_COLUMN_FAMILY);
				std::vector<rocksdb::ColumnFamilyDescriptor> descriptors;
				for (const std::string& name : columnFamilies) {
					descriptors.push_back(rocksdb::ColumnFamilyDescriptor{ name, getCFOptions() });
				}
				s = rocksdb::DestroyDB(a.path, getOptions(), descriptors);
				if (!s.ok()) {
					logRocksDBError(s, "Destroy");
				} else {
					TraceEvent("RocksDBCheckpointReader").detail("Path", a.path).detail("Method", "Destroy");
				}
			}

			std::cout << "RocksDBCheckpointReader close acton done" << std::endl;
			TraceEvent("RocksDBCheckpointReader").detail("Path", a.path).detail("Method", "Close");
			a.done.send(Void());
		}

		struct ReadRangeAction : TypedAction<Reader, ReadRangeAction>, FastAllocated<ReadRangeAction> {
			ReadRangeAction(int rowLimit, int byteLimit)
			  : rowLimit(rowLimit), byteLimit(byteLimit), startTime(timer_monotonic()) {}

			double getTimeEstimate() const override { return SERVER_KNOBS->READ_RANGE_TIME_ESTIMATE; }

			const int rowLimit, byteLimit;
			const double startTime;
			ThreadReturnPromise<RangeResult> result;
		};

		void action(ReadRangeAction& a) {
			const double readBeginTime = timer_monotonic();

			if (readBeginTime - a.startTime > readRangeTimeout) {
				TraceEvent(SevWarn, "RocksDBCheckpointReaderError")
				    .detail("Error", "Read range request timedout")
				    .detail("Method", "ReadRangeAction")
				    .detail("Timeout value", readRangeTimeout);
				a.result.sendError(transaction_too_old());
				return;
			}

			std::cout << "Reading batch" << std::endl;

			RangeResult result;
			if (a.rowLimit == 0 || a.byteLimit == 0) {
				a.result.send(result);
				return;
			}

			ASSERT(a.rowLimit > 0);

			int accumulatedBytes = 0;
			rocksdb::Status s;
			while (cursor->Valid()) {
				KeyValueRef kv(toStringRef(cursor->key()), toStringRef(cursor->value()));
				std::cout << "Getting key " << cursor->key().ToString() << std::endl;
				accumulatedBytes += sizeof(KeyValueRef) + kv.expectedSize();
				result.push_back_deep(result.arena(), kv);
				// Calling `cursor->Next()` is potentially expensive, so short-circut here just in case.
				if (result.size() >= a.rowLimit || accumulatedBytes >= a.byteLimit) {
					break;
				}
				if (timer_monotonic() - a.startTime > readRangeTimeout) {
					TraceEvent(SevWarn, "RocksDBCheckpointReaderError")
					    .detail("Error", "Read range request timedout")
					    .detail("Method", "ReadRangeAction")
					    .detail("Timeout value", readRangeTimeout);
					a.result.sendError(transaction_too_old());
					delete (cursor.release());
					return;
				}
				cursor->Next();
			}

			s = cursor->status();

			if (!s.ok()) {
				logRocksDBError(s, "ReadRange");
				a.result.sendError(statusToError(s));
				return;
			}

			std::cout << "Read Done." << cursor->status().ToString() << std::endl;
			// throw end_of_stream();

			if (result.empty()) {
				delete (cursor.release());
				a.result.sendError(end_of_stream());
			} else {
				a.result.send(result);
			}
		}

		DB& db;
		CF cf;
		rocksdb::Slice begin;
		rocksdb::Slice end;
		double readRangeTimeout;
		std::unique_ptr<rocksdb::Iterator> cursor;
	};

public:
	RocksDBCheckpointReader(const CheckpointMetaData& checkpoint, UID logID) : id(logID) {
		RocksDBCheckpoint rocksCheckpoint = getRocksCheckpoint(checkpoint);
		this->path = rocksCheckpoint.checkpointDir;
		if (g_network->isSimulated()) {
			readThreads = CoroThreadPool::createThreadPool();
		} else {
			readThreads = createGenericThreadPool();
		}
		readThreads->addThread(new Reader(db), "fdb-rocksdb-checkpoint-reader");
	}

	Future<Void> init(StringRef token) override { throw not_implemented(); }

	Future<Void> init(KeyRangeRef range) override {
		if (openFuture.isValid()) {
			return openFuture;
		}
		auto a = std::make_unique<Reader::OpenAction>(this->path, range);
		openFuture = a->done.getFuture();
		readThreads->post(a.release());
		return openFuture;
	}

	Future<RangeResult> nextKeyValues(const int rowLimit, const int byteLimit) override {
		auto a = std::make_unique<Reader::ReadRangeAction>(rowLimit, byteLimit);
		auto res = a->result.getFuture();
		readThreads->post(a.release());
		return res;
	}

	Future<Standalone<StringRef>> nextChunk(const int byteLimit) { throw not_implemented(); }

	Future<Void> close() { return doClose(this); }

private:
	ACTOR static Future<Void> doClose(RocksDBCheckpointReader* self) {
		if (self == nullptr)
			return Void();

		auto a = new Reader::CloseAction(self->path, false);
		auto f = a->done.getFuture();
		self->readThreads->post(a);
		wait(f);

		std::cout << "Closed Action." << std::endl;

		if (self != nullptr) {
			wait(self->readThreads->stop());
		}

		std::cout << "threads stopped." << std::endl;

		if (self != nullptr) {
			delete self;
		}

		return Void();
	}

	DB db = nullptr;
	std::string path;
	const UID id;
	Reference<IThreadPool> readThreads;
	Future<Void> openFuture;
};

#endif // SSD_ROCKSDB_EXPERIMENTAL

class RocksDBCFCheckpointReader : public ICheckpointReader {
public:
	RocksDBCFCheckpointReader(const CheckpointMetaData& checkpoint, UID logID)
	  : checkpoint_(checkpoint), id_(logID), file_(Reference<IAsyncFile>()), offset_(0) {}

	Future<Void> init(StringRef token) override;

	Future<Void> init(KeyRangeRef range) override { throw not_implemented(); }

	Future<RangeResult> nextKeyValues(const int rowLimit, const int byteLimit) override { throw not_implemented(); }

	// Returns the next chunk of serialized checkpoint.
	Future<Standalone<StringRef>> nextChunk(const int byteLimit) override;

	Future<Void> close() override;

private:
	ACTOR static Future<Void> doInit(RocksDBCFCheckpointReader* self) {
		ASSERT_NE(self, nullptr);
		try {
			state Reference<IAsyncFile> _file = wait(IAsyncFileSystem::filesystem()->open(
			    self->path_, IAsyncFile::OPEN_READONLY | IAsyncFile::OPEN_UNCACHED | IAsyncFile::OPEN_NO_AIO, 0));
			self->file_ = _file;
			TraceEvent("RocksDBCheckpointReaderOpenFile").detail("File", self->path_);
		} catch (Error& e) {
			TraceEvent(SevWarnAlways, "ServerGetCheckpointFileFailure")
			    .detail("File", self->path_)
			    .error(e, /*includeCancel=*/true);
			throw e;
		}

		return Void();
	}

	ACTOR Future<Standalone<StringRef>> getNextChunk(RocksDBCFCheckpointReader* self, int byteLimit) {
		state int transactionSize = std::min(64 * 1024, byteLimit); // Block size read from disk.
		state Standalone<StringRef> buf = makeAlignedString(_PAGE_SIZE, transactionSize);
		int bytesRead = wait(self->file_->read(mutateString(buf), transactionSize, self->offset_));
		if (bytesRead == 0) {
			throw end_of_stream();
		}

		self->offset_ += bytesRead;
		return buf.substr(0, bytesRead);
	}

	ACTOR static Future<Void> doClose(RocksDBCFCheckpointReader* self) {
		wait(delay(0, TaskPriority::FetchKeys));
		delete self;
		return Void();
	}

	CheckpointMetaData checkpoint_;
	UID id_;
	Reference<IAsyncFile> file_;
	int offset_;
	std::string path_;
};

#ifdef SSD_ROCKSDB_EXPERIMENTAL
ACTOR Future<Void> fetchCheckpointRange(Database cx,
                                        std::shared_ptr<CheckpointMetaData> metaData,
                                        KeyRange range,
                                        std::string localFile,
                                        std::shared_ptr<rocksdb::SstFileWriter> writer,
                                        std::function<Future<Void>(const CheckpointMetaData&)> cFun,
                                        int maxRetries = 3) {
	RocksDBCheckpoint rcp = getRocksCheckpoint(*metaData);
	TraceEvent("FetchCheckpointRange")
	    .detail("InitialState", metaData->toString())
	    .detail("RocksCheckpoint", rcp.toString());

	for (const auto& [shard, file] : rcp.fetchedFiles) {
		ASSERT(!shard.intersects(range));
	}

	state UID ssID = metaData->ssID;
	state Transaction tr(cx);
	state StorageServerInterface ssi;
	loop {
		try {
			Optional<Value> ss = wait(tr.get(serverListKeyFor(ssID)));
			if (!ss.present()) {
				throw checkpoint_not_found();
			}
			ssi = decodeServerListValue(ss.get());
			break;
		} catch (Error& e) {
			wait(tr.onError(e));
		}
	}

	std::cout << "FetchRocksCheckpointKeyValues found ss: " << ssi.toString() << std::endl;
	ASSERT(ssi.id() == ssID);

	state int attempt = 0;
	state int64_t totalBytes = 0;
	state rocksdb::Status status;
	state Optional<Error> error;
	loop {
		totalBytes = 0;
		++attempt;
		try {
			TraceEvent("FetchCheckpointRangeBegin")
			    .detail("CheckpointID", metaData->checkpointID)
			    .detail("Range", range.toString())
			    .detail("TargetStorageServerUID", ssID)
			    .detail("LocalFile", localFile)
			    .detail("Attempt", attempt)
			    .log();
			// status = writer.Finish();
			// if (!status.ok()) {
			// 	std::cout << "SstFileWriter close failure: " << status.ToString() << std::endl;
			// 	break;
			// }

			wait(IAsyncFileSystem::filesystem()->deleteFile(localFile, true));
			status = writer->Open(localFile);
			if (!status.ok()) {
				Error e = statusToError(status);
				std::cout << "SstFileWriter open failure: " << status.ToString() << std::endl;
				TraceEvent("FetchCheckpointRangeOpenFileError")
				    .detail("LocalFile", localFile)
				    .detail("Status", status.ToString());
				throw e;
			}
			// const int64_t flags = IAsyncFile::OPEN_ATOMIC_WRITE_AND_CREATE | IAsyncFile::OPEN_READWRITE |
			//                       IAsyncFile::OPEN_CREATE | IAsyncFile::OPEN_UNCACHED | IAsyncFile::OPEN_NO_AIO;
			// state Reference<IAsyncFile> asyncFile = wait(IAsyncFileSystem::filesystem()->open(localFile, flags,
			// 0666));

			state ReplyPromiseStream<FetchCheckpointKeyValuesStreamReply> stream =
			    ssi.fetchCheckpointKeyValues.getReplyStream(
			        FetchCheckpointKeyValuesRequest(metaData->checkpointID, range));
			std::cout << "FetchRocksCheckpointKeyValues stream." << std::endl;
			TraceEvent("FetchCheckpointKeyValuesReceivingData")
			    .detail("CheckpointID", metaData->checkpointID)
			    .detail("Range", range.toString())
			    .detail("TargetStorageServerUID", ssID.toString())
			    .detail("LocalFile", localFile)
			    .detail("Attempt", attempt)
			    .log();

			loop {
				FetchCheckpointKeyValuesStreamReply rep = waitNext(stream.getFuture());
				// wait(asyncFile->write(rep.data.begin(), rep.size, offset));
				// wait(asyncFile->flush());
				//  += rep.data.size();
				for (int i = 0; i < rep.data.size(); ++i) {
					std::cout << "Writing key: " << rep.data[i].key.toString()
					          << ", value: " << rep.data[i].value.toString() << std::endl;
					status = writer->Put(toSlice(rep.data[i].key), toSlice(rep.data[i].value));
					if (!status.ok()) {
						Error e = statusToError(status);
						std::cout << "SstFileWriter put failure: " << status.ToString() << std::endl;
						TraceEvent("FetchCheckpointRangeWriteError")
						    .detail("LocalFile", localFile)
						    .detail("Key", rep.data[i].key.toString())
						    .detail("Value", rep.data[i].value.toString())
						    .detail("Status", status.ToString());
						throw e;
					}
					totalBytes += rep.data[i].expectedSize();
				}
			}
		} catch (Error& e) {
			Error err = e;
			status = writer->Finish();
			if (!status.ok()) {
				err = statusToError(status);
			}
			if (err.code() != error_code_end_of_stream) {
				TraceEvent("FetchCheckpointFileError")
				    .detail("CheckpointID", metaData->checkpointID)
				    .detail("Range", range.toString())
				    .detail("TargetStorageServerUID", ssID.toString())
				    .detail("LocalFile", localFile)
				    .detail("Attempt", attempt)
				    .error(err, true);
				if (attempt >= maxRetries) {
					error = err;
					break;
				}
			} else {
				RocksDBCheckpoint rcp = getRocksCheckpoint(*metaData);
				rcp.fetchedFiles.emplace_back(range, localFile);
				metaData->serializedCheckpoint = ObjectWriter::toValue(rcp, IncludeVersion());
				// TODO: This won't work since it is not transactional.
				// if (cFun) {
				// 	wait(cFun(*metaData));
				// }
				TraceEvent("FetchCheckpointRangeBegin")
				    .detail("CheckpointID", metaData->checkpointID)
				    .detail("Range", range.toString())
				    .detail("TargetStorageServerUID", ssID.toString())
				    .detail("LocalFile", localFile)
				    .detail("Attempt", attempt)
				    .detail("TotalBytes", totalBytes);
				break;
			}
		}
	}

	if (error.present()) {
		throw error.get();
	}

	return Void();
}

Future<Void> RocksDBCFCheckpointReader::init(StringRef token) {
	ASSERT_EQ(this->checkpoint_.getFormat(), RocksDBColumnFamily);
	const std::string name = token.toString();
	this->offset_ = 0;
	this->path_.clear();
	const RocksDBColumnFamilyCheckpoint rocksCF = getRocksCF(this->checkpoint_);
	for (const auto& sstFile : rocksCF.sstFiles) {
		if (sstFile.name == name) {
			this->path_ = sstFile.db_path + sstFile.name;
			break;
		}
	}

	if (this->path_.empty()) {
		TraceEvent("RocksDBCheckpointReaderInitFileNotFound").detail("File", this->path_);
		return checkpoint_not_found();
	}

	return doInit(this);
}

Future<Standalone<StringRef>> RocksDBCFCheckpointReader::nextChunk(const int byteLimit) {
	return getNextChunk(this, byteLimit);
}

Future<Void> RocksDBCFCheckpointReader::close() {
	return doClose(this);
}

// Fetch a single sst file from storage server. If the file is fetch successfully, it will be recorded via cFun.
ACTOR Future<Void> fetchCheckpointFile(Database cx,
                                       std::shared_ptr<CheckpointMetaData> metaData,
                                       int idx,
                                       std::string dir,
                                       std::function<Future<Void>(const CheckpointMetaData&)> cFun,
                                       int maxRetries = 3) {
	state RocksDBColumnFamilyCheckpoint rocksCF;
	ObjectReader reader(metaData->serializedCheckpoint.begin(), IncludeVersion());
	reader.deserialize(rocksCF);

	// Skip fetched file.
	if (rocksCF.sstFiles[idx].fetched && rocksCF.sstFiles[idx].db_path == dir) {
		return Void();
	}

	state std::string remoteFile = rocksCF.sstFiles[idx].name;
	state std::string localFile = dir + rocksCF.sstFiles[idx].name;
	state UID ssID = metaData->ssID;

	state Transaction tr(cx);
	state StorageServerInterface ssi;
	loop {
		try {
			Optional<Value> ss = wait(tr.get(serverListKeyFor(ssID)));
			if (!ss.present()) {
				throw checkpoint_not_found();
			}
			ssi = decodeServerListValue(ss.get());
			break;
		} catch (Error& e) {
			wait(tr.onError(e));
		}
	}

	state int attempt = 0;
	loop {
		try {
			++attempt;
			TraceEvent("FetchCheckpointFileBegin")
			    .detail("RemoteFile", remoteFile)
			    .detail("TargetUID", ssID.toString())
			    .detail("StorageServer", ssi.id().toString())
			    .detail("LocalFile", localFile)
			    .detail("Attempt", attempt);

			wait(IAsyncFileSystem::filesystem()->deleteFile(localFile, true));
			const int64_t flags = IAsyncFile::OPEN_ATOMIC_WRITE_AND_CREATE | IAsyncFile::OPEN_READWRITE |
			                      IAsyncFile::OPEN_CREATE | IAsyncFile::OPEN_UNCACHED | IAsyncFile::OPEN_NO_AIO;
			state int64_t offset = 0;
			state Reference<IAsyncFile> asyncFile = wait(IAsyncFileSystem::filesystem()->open(localFile, flags, 0666));

			state ReplyPromiseStream<FetchCheckpointReply> stream =
			    ssi.fetchCheckpoint.getReplyStream(FetchCheckpointRequest(metaData->checkpointID, remoteFile));
			TraceEvent("FetchCheckpointFileReceivingData")
			    .detail("RemoteFile", remoteFile)
			    .detail("TargetUID", ssID.toString())
			    .detail("StorageServer", ssi.id().toString())
			    .detail("LocalFile", localFile)
			    .detail("Attempt", attempt);
			loop {
				state FetchCheckpointReply rep = waitNext(stream.getFuture());
				wait(asyncFile->write(rep.data.begin(), rep.data.size(), offset));
				wait(asyncFile->flush());
				offset += rep.data.size();
			}
		} catch (Error& e) {
			if (e.code() != error_code_end_of_stream) {
				TraceEvent("FetchCheckpointFileError")
				    .detail("RemoteFile", remoteFile)
				    .detail("StorageServer", ssi.toString())
				    .detail("LocalFile", localFile)
				    .detail("Attempt", attempt)
				    .error(e, true);
				if (attempt >= maxRetries) {
					throw e;
				}
			} else {
				wait(asyncFile->sync());
				int64_t fileSize = wait(asyncFile->size());
				TraceEvent("FetchCheckpointFileEnd")
				    .detail("RemoteFile", remoteFile)
				    .detail("StorageServer", ssi.toString())
				    .detail("LocalFile", localFile)
				    .detail("Attempt", attempt)
				    .detail("DataSize", offset)
				    .detail("FileSize", fileSize);
				rocksCF.sstFiles[idx].db_path = dir;
				rocksCF.sstFiles[idx].fetched = true;
				metaData->serializedCheckpoint = ObjectWriter::toValue(rocksCF, IncludeVersion());
				if (cFun) {
					wait(cFun(*metaData));
				}
				return Void();
			}
		}
	}
}

#endif // SSD_ROCKSDB_EXPERIMENTAL
} // namespace

#ifdef SSD_ROCKSDB_EXPERIMENTAL
ACTOR Future<CheckpointMetaData> fetchRocksDBCheckpoint(Database cx,
                                                        CheckpointMetaData initialState,
                                                        std::string dir,
                                                        std::function<Future<Void>(const CheckpointMetaData&)> cFun) {
	TraceEvent("FetchRocksCheckpointBegin")
	    .detail("InitialState", initialState.toString())
	    .detail("CheckpointDir", dir);

	state std::shared_ptr<CheckpointMetaData> metaData = std::make_shared<CheckpointMetaData>(initialState);

	if (metaData->format == RocksDBColumnFamily) {
		state RocksDBColumnFamilyCheckpoint rocksCF = getRocksCF(initialState);
		TraceEvent("RocksDBCheckpointMetaData").detail("RocksCF", rocksCF.toString());

		state int i = 0;
		state std::vector<Future<Void>> fs;
		for (; i < rocksCF.sstFiles.size(); ++i) {
			fs.push_back(fetchCheckpointFile(cx, metaData, i, dir, cFun));
			TraceEvent("GetCheckpointFetchingFile")
			    .detail("FileName", rocksCF.sstFiles[i].name)
			    .detail("Server", metaData->ssID.toString());
		}
		wait(waitForAll(fs));
	} else if (metaData->format == RocksDB) {
		// RocksDBCheckpoint rcp = getRocksCheckpoint(*metaData);
		std::string localFile = dir + "/" + metaData->checkpointID.toString() + ".sst";
		std::shared_ptr<rocksdb::SstFileWriter> writer =
		    std::make_shared<rocksdb::SstFileWriter>(rocksdb::EnvOptions(), rocksdb::Options());
		wait(fetchCheckpointRange(cx, metaData, metaData->range, localFile, writer, cFun));
	}

	return *metaData;
}
#else
ACTOR Future<CheckpointMetaData> fetchRocksDBCheckpoint(Database cx,
                                                        CheckpointMetaData initialState,
                                                        std::string dir,
                                                        std::function<Future<Void>(const CheckpointMetaData&)> cFun) {
	wait(delay(0));
	return initialState;
}
#endif // SSD_ROCKSDB_EXPERIMENTAL

#ifdef SSD_ROCKSDB_EXPERIMENTAL
ACTOR Future<Void> deleteRocksCFCheckpoint(CheckpointMetaData checkpoint) {
	state CheckpointFormat format = checkpoint.getFormat();
	state std::unordered_set<std::string> dirs;
	if (format == RocksDBColumnFamily) {
		RocksDBColumnFamilyCheckpoint rocksCF = getRocksCF(checkpoint);
		TraceEvent("DeleteRocksColumnFamilyCheckpoint", checkpoint.checkpointID)
		    .detail("CheckpointID", checkpoint.checkpointID)
		    .detail("RocksCF", rocksCF.toString());

		for (const LiveFileMetaData& file : rocksCF.sstFiles) {
			dirs.insert(file.db_path);
		}
	} else if (format == RocksDB) {
		RocksDBCheckpoint rocksCheckpoint = getRocksCheckpoint(checkpoint);
		TraceEvent("DeleteRocksCheckpoint", checkpoint.checkpointID)
		    .detail("CheckpointID", checkpoint.checkpointID)
		    .detail("RocksCheckpoint", rocksCheckpoint.toString());
		dirs.insert(rocksCheckpoint.checkpointDir);
	} else {
		ASSERT(false);
	}

	state std::unordered_set<std::string>::iterator it = dirs.begin();
	for (; it != dirs.end(); ++it) {
		const std::string dir = *it;
		platform::eraseDirectoryRecursive(dir);
		TraceEvent("DeleteCheckpointRemovedDir", checkpoint.checkpointID)
		    .detail("CheckpointID", checkpoint.checkpointID)
		    .detail("Dir", dir);
		wait(delay(0, TaskPriority::FetchKeys));
	}

	return Void();
}
#else
ACTOR Future<Void> deleteRocksCFCheckpoint(CheckpointMetaData checkpoint) {
	wait(delay(0));
	return Void();
}
#endif // SSD_ROCKSDB_EXPERIMENTAL

ICheckpointReader* newRocksDBCheckpointReader(const CheckpointMetaData& checkpoint, UID logID) {
#ifdef SSD_ROCKSDB_EXPERIMENTAL
	const CheckpointFormat format = checkpoint.getFormat();
	if (format == RocksDBColumnFamily) {
		return new RocksDBCFCheckpointReader(checkpoint, logID);
	} else if (format == RocksDB) {
		return new RocksDBCheckpointReader(checkpoint, logID);
	}
#endif // SSD_ROCKSDB_EXPERIMENTAL
	return nullptr;
}

RocksDBColumnFamilyCheckpoint getRocksCF(const CheckpointMetaData& checkpoint) {
	RocksDBColumnFamilyCheckpoint rocksCF;
	ObjectReader reader(checkpoint.serializedCheckpoint.begin(), IncludeVersion());
	reader.deserialize(rocksCF);
	return rocksCF;
}

RocksDBCheckpoint getRocksCheckpoint(const CheckpointMetaData& checkpoint) {
	RocksDBCheckpoint rocksCheckpoint;
	ObjectReader reader(checkpoint.serializedCheckpoint.begin(), IncludeVersion());
	reader.deserialize(rocksCheckpoint);
	return rocksCheckpoint;
}