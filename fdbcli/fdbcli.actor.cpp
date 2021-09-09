/*
 * fdbcli.actor.cpp
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

#include "boost/lexical_cast.hpp"
#include "fdbclient/NativeAPI.actor.h"
#include "fdbclient/FDBTypes.h"
#include "fdbclient/IClientApi.h"
#include "fdbclient/MultiVersionTransaction.h"
#include "fdbclient/Status.h"
#include "fdbclient/KeyBackedTypes.h"
#include "fdbclient/StatusClient.h"
#include "fdbclient/DatabaseContext.h"
#include "fdbclient/GlobalConfig.actor.h"
#include "fdbclient/IKnobCollection.h"
#include "fdbclient/NativeAPI.actor.h"
#include "fdbclient/ClusterInterface.h"
#include "fdbclient/ManagementAPI.actor.h"
#include "fdbclient/Schemas.h"
#include "fdbclient/CoordinationInterface.h"
#include "fdbclient/FDBOptions.g.h"
#include "fdbclient/TagThrottle.actor.h"
#include "fdbclient/Tuple.h"

#include "fdbclient/ThreadSafeTransaction.h"
#include "flow/DeterministicRandom.h"
#include "flow/FastRef.h"
#include "flow/Platform.h"

#include "flow/TLSConfig.actor.h"
#include "flow/ThreadHelper.actor.h"
#include "flow/SimpleOpt.h"

#include "fdbcli/FlowLineNoise.h"
#include "fdbcli/fdbcli.actor.h"

#include <cinttypes>
#include <type_traits>
#include <signal.h>

#ifdef __unixish__
#include <stdio.h>
#include "fdbcli/linenoise/linenoise.h"
#endif

#include "fdbclient/versions.h"
#include "fdbclient/BuildFlags.h"

#include "flow/actorcompiler.h" // This must be the last #include.

#define FDB_API_VERSION 710
/*
 * While we could just use the MultiVersionApi instance directly, this #define allows us to swap in any other IClientApi
 * instance (e.g. from ThreadSafeApi)
 */
#define API ((IClientApi*)MultiVersionApi::api)

extern const char* getSourceVersion();

std::vector<std::string> validOptions;

enum {
	OPT_CONNFILE,
	OPT_DATABASE,
	OPT_HELP,
	OPT_TRACE,
	OPT_TRACE_DIR,
	OPT_TIMEOUT,
	OPT_EXEC,
	OPT_NO_STATUS,
	OPT_NO_HINTS,
	OPT_STATUS_FROM_JSON,
	OPT_VERSION,
	OPT_BUILD_FLAGS,
	OPT_TRACE_FORMAT,
	OPT_KNOB,
	OPT_DEBUG_TLS
};

CSimpleOpt::SOption g_rgOptions[] = { { OPT_CONNFILE, "-C", SO_REQ_SEP },
	                                  { OPT_CONNFILE, "--cluster_file", SO_REQ_SEP },
	                                  { OPT_DATABASE, "-d", SO_REQ_SEP },
	                                  { OPT_TRACE, "--log", SO_NONE },
	                                  { OPT_TRACE_DIR, "--log-dir", SO_REQ_SEP },
	                                  { OPT_TIMEOUT, "--timeout", SO_REQ_SEP },
	                                  { OPT_EXEC, "--exec", SO_REQ_SEP },
	                                  { OPT_NO_STATUS, "--no-status", SO_NONE },
	                                  { OPT_NO_HINTS, "--no-hints", SO_NONE },
	                                  { OPT_HELP, "-?", SO_NONE },
	                                  { OPT_HELP, "-h", SO_NONE },
	                                  { OPT_HELP, "--help", SO_NONE },
	                                  { OPT_STATUS_FROM_JSON, "--status-from-json", SO_REQ_SEP },
	                                  { OPT_VERSION, "--version", SO_NONE },
	                                  { OPT_VERSION, "-v", SO_NONE },
	                                  { OPT_BUILD_FLAGS, "--build_flags", SO_NONE },
	                                  { OPT_TRACE_FORMAT, "--trace_format", SO_REQ_SEP },
	                                  { OPT_KNOB, "--knob_", SO_REQ_SEP },
	                                  { OPT_DEBUG_TLS, "--debug-tls", SO_NONE },

#ifndef TLS_DISABLED
	                                  TLS_OPTION_FLAGS
#endif

	                                      SO_END_OF_OPTIONS };

void printAtCol(const char* text, int col) {
	const char* iter = text;
	const char* start = text;
	const char* space = nullptr;

	do {
		iter++;
		if (*iter == '\n' || *iter == ' ' || *iter == '\0')
			space = iter;
		if (*iter == '\n' || *iter == '\0' || (iter - start == col)) {
			if (!space)
				space = iter;
			printf("%.*s\n", (int)(space - start), start);
			start = space;
			if (*start == ' ' || *start == '\n')
				start++;
			space = nullptr;
		}
	} while (*iter);
}

class FdbOptions {
public:
	// Prints an error and throws invalid_option or invalid_option_value if the option could not be set
	void setOption(Reference<ITransaction> tr,
	               StringRef optionStr,
	               bool enabled,
	               Optional<StringRef> arg,
	               bool intrans) {
		auto transactionItr = transactionOptions.legalOptions.find(optionStr.toString());
		if (transactionItr != transactionOptions.legalOptions.end())
			setTransactionOption(tr, transactionItr->second, enabled, arg, intrans);
		else {
			fprintf(stderr,
			        "ERROR: invalid option '%s'. Try `help options' for a list of available options.\n",
			        optionStr.toString().c_str());
			throw invalid_option();
		}
	}

	// Applies all enabled transaction options to the given transaction
	void apply(Reference<ITransaction> tr) {
		for (const auto& [name, value] : transactionOptions.options) {
			tr->setOption(name, value.castTo<StringRef>());
		}
	}

	// Returns true if any options have been set
	bool hasAnyOptionsEnabled() const { return !transactionOptions.options.empty(); }

	// Prints a list of enabled options, along with their parameters (if any)
	void print() const {
		bool found = false;
		found = found || transactionOptions.print();

		if (!found)
			printf("There are no options enabled\n");
	}

	// Returns a vector of the names of all documented options
	std::vector<std::string> getValidOptions() const { return transactionOptions.getValidOptions(); }

	// Prints the help string obtained by invoking `help options'
	void printHelpString() const { transactionOptions.printHelpString(); }

private:
	// Sets a transaction option. If intrans == true, then this option is also applied to the passed in transaction.
	void setTransactionOption(Reference<ITransaction> tr,
	                          FDBTransactionOptions::Option option,
	                          bool enabled,
	                          Optional<StringRef> arg,
	                          bool intrans) {
		if (enabled && arg.present() != FDBTransactionOptions::optionInfo.getMustExist(option).hasParameter) {
			fprintf(stderr, "ERROR: option %s a parameter\n", arg.present() ? "did not expect" : "expected");
			throw invalid_option_value();
		}

		if (intrans) {
			tr->setOption(option, arg);
		}

		transactionOptions.setOption(option, enabled, arg.castTo<StringRef>());
	}

	// A group of enabled options (of type T::Option) as well as a legal options map from string to T::Option
	template <class T>
	struct OptionGroup {
		std::map<typename T::Option, Optional<Standalone<StringRef>>> options;
		std::map<std::string, typename T::Option> legalOptions;

		OptionGroup<T>() {}
		OptionGroup<T>(OptionGroup<T>& base)
		  : options(base.options.begin(), base.options.end()), legalOptions(base.legalOptions) {}

		// Enable or disable an option. Returns true if option value changed
		bool setOption(typename T::Option option, bool enabled, Optional<StringRef> arg) {
			auto optionItr = options.find(option);
			if (enabled && (optionItr == options.end() ||
			                Optional<Standalone<StringRef>>(optionItr->second).castTo<StringRef>() != arg)) {
				options[option] = arg.castTo<Standalone<StringRef>>();
				return true;
			} else if (!enabled && optionItr != options.end()) {
				options.erase(optionItr);
				return true;
			}

			return false;
		}

		// Prints a list of all enabled options in this group
		bool print() const {
			bool found = false;

			for (auto itr = legalOptions.begin(); itr != legalOptions.end(); ++itr) {
				auto optionItr = options.find(itr->second);
				if (optionItr != options.end()) {
					if (optionItr->second.present())
						printf("%s: `%s'\n", itr->first.c_str(), formatStringRef(optionItr->second.get()).c_str());
					else
						printf("%s\n", itr->first.c_str());

					found = true;
				}
			}

			return found;
		}

		// Returns true if the specified option is documented
		bool isDocumented(typename T::Option option) const {
			FDBOptionInfo info = T::optionInfo.getMustExist(option);

			std::string deprecatedStr = "Deprecated";
			return !info.comment.empty() && info.comment.substr(0, deprecatedStr.size()) != deprecatedStr;
		}

		// Returns a vector of the names of all documented options
		std::vector<std::string> getValidOptions() const {
			std::vector<std::string> ret;

			for (auto itr = legalOptions.begin(); itr != legalOptions.end(); ++itr)
				if (isDocumented(itr->second))
					ret.push_back(itr->first);

			return ret;
		}

		// Prints a help string for each option in this group. Any options with no comment
		// are excluded from this help string. Lines are wrapped to 80 characters.
		void printHelpString() const {
			for (auto itr = legalOptions.begin(); itr != legalOptions.end(); ++itr) {
				if (isDocumented(itr->second)) {
					FDBOptionInfo info = T::optionInfo.getMustExist(itr->second);
					std::string helpStr = info.name + " - " + info.comment;
					if (info.hasParameter)
						helpStr += " " + info.parameterComment;
					helpStr += "\n";

					printAtCol(helpStr.c_str(), 80);
				}
			}
		}
	};

	OptionGroup<FDBTransactionOptions> transactionOptions;

public:
	FdbOptions() {
		for (auto itr = FDBTransactionOptions::optionInfo.begin(); itr != FDBTransactionOptions::optionInfo.end();
		     ++itr)
			transactionOptions.legalOptions[itr->second.name] = itr->first;
	}

	FdbOptions(FdbOptions& base) : transactionOptions(base.transactionOptions) {}
};

static std::string formatStringRef(StringRef item, bool fullEscaping = false) {
	std::string ret;

	for (int i = 0; i < item.size(); i++) {
		if (fullEscaping && item[i] == '\\')
			ret += "\\\\";
		else if (fullEscaping && item[i] == '"')
			ret += "\\\"";
		else if (fullEscaping && item[i] == ' ')
			ret += format("\\x%02x", item[i]);
		else if (item[i] >= 32 && item[i] < 127)
			ret += item[i];
		else
			ret += format("\\x%02x", item[i]);
	}

	return ret;
}

static std::vector<std::vector<StringRef>> parseLine(std::string& line, bool& err, bool& partial) {
	err = false;
	partial = false;

	bool quoted = false;
	std::vector<StringRef> buf;
	std::vector<std::vector<StringRef>> ret;

	size_t i = line.find_first_not_of(' ');
	size_t offset = i;

	bool forcetoken = false;

	while (i <= line.length()) {
		switch (line[i]) {
		case ';':
			if (!quoted) {
				if (i > offset || (forcetoken && i == offset))
					buf.push_back(StringRef((uint8_t*)(line.data() + offset), i - offset));
				ret.push_back(std::move(buf));
				offset = i = line.find_first_not_of(' ', i + 1);
				forcetoken = false;
			} else
				i++;
			break;
		case '"':
			quoted = !quoted;
			line.erase(i, 1);
			forcetoken = true;
			break;
		case ' ':
			if (!quoted) {
				if (i > offset || (forcetoken && i == offset))
					buf.push_back(StringRef((uint8_t*)(line.data() + offset), i - offset));
				offset = i = line.find_first_not_of(' ', i);
				forcetoken = false;
			} else
				i++;
			break;
		case '\\':
			if (i + 2 > line.length()) {
				err = true;
				ret.push_back(std::move(buf));
				return ret;
			}
			switch (line[i + 1]) {
				char ent, save;
			case '"':
			case '\\':
			case ' ':
			case ';':
				line.erase(i, 1);
				break;
			case 'x':
				if (i + 4 > line.length()) {
					err = true;
					ret.push_back(std::move(buf));
					return ret;
				}
				char* pEnd;
				save = line[i + 4];
				line[i + 4] = 0;
				ent = char(strtoul(line.data() + i + 2, &pEnd, 16));
				if (*pEnd) {
					err = true;
					ret.push_back(std::move(buf));
					return ret;
				}
				line[i + 4] = save;
				line.replace(i, 4, 1, ent);
				break;
			default:
				err = true;
				ret.push_back(std::move(buf));
				return ret;
			}
		default:
			i++;
		}
	}

	i -= 1;
	if (i > offset || (forcetoken && i == offset))
		buf.push_back(StringRef((uint8_t*)(line.data() + offset), i - offset));

	ret.push_back(std::move(buf));

	if (quoted)
		partial = true;

	return ret;
}

static void printProgramUsage(const char* name) {
	printf("FoundationDB CLI " FDB_VT_PACKAGE_NAME " (v" FDB_VT_VERSION ")\n"
	       "usage: %s [OPTIONS]\n"
	       "\n",
	       name);
	printf("  -C CONNFILE    The path of a file containing the connection string for the\n"
	       "                 FoundationDB cluster. The default is first the value of the\n"
	       "                 FDB_CLUSTER_FILE environment variable, then `./fdb.cluster',\n"
	       "                 then `%s'.\n",
	       platform::getDefaultClusterFilePath().c_str());
	printf("  --log          Enables trace file logging for the CLI session.\n"
	       "  --log-dir PATH Specifes the output directory for trace files. If\n"
	       "                 unspecified, defaults to the current directory. Has\n"
	       "                 no effect unless --log is specified.\n"
	       "  --trace_format FORMAT\n"
	       "                 Select the format of the log files. xml (the default) and json\n"
	       "                 are supported. Has no effect unless --log is specified.\n"
	       "  --exec CMDS    Immediately executes the semicolon separated CLI commands\n"
	       "                 and then exits.\n"
	       "  --no-status    Disables the initial status check done when starting\n"
	       "                 the CLI.\n"
#ifndef TLS_DISABLED
	       TLS_HELP
#endif
	       "  --knob_KNOBNAME KNOBVALUE\n"
	       "                 Changes a knob option. KNOBNAME should be lowercase.\n"
	       "  --debug-tls    Prints the TLS configuration and certificate chain, then exits.\n"
	       "                 Useful in reporting and diagnosing TLS issues.\n"
	       "  --build_flags  Print build information and exit.\n"
	       "  -v, --version  Print FoundationDB CLI version information and exit.\n"
	       "  -h, --help     Display this help and exit.\n");
}

#define ESCAPINGK "\n\nFor information on escaping keys, type `help escaping'."
#define ESCAPINGKV "\n\nFor information on escaping keys and values, type `help escaping'."

using namespace fdb_cli;
std::map<std::string, CommandHelp>& helpMap = CommandFactory::commands();
std::set<std::string>& hiddenCommands = CommandFactory::hiddenCommands();

void initHelp() {
	helpMap["begin"] =
	    CommandHelp("begin",
	                "begin a new transaction",
	                "By default, the fdbcli operates in autocommit mode. All operations are performed in their own "
	                "transaction, and are automatically committed for you. By explicitly beginning a transaction, "
	                "successive operations are all performed as part of a single transaction.\n\nTo commit the "
	                "transaction, use the commit command. To discard the transaction, use the reset command.");
	helpMap["commit"] = CommandHelp("commit",
	                                "commit the current transaction",
	                                "Any sets or clears executed after the start of the current transaction will be "
	                                "committed to the database. On success, the committed version number is displayed. "
	                                "If commit fails, the error is displayed and the transaction must be retried.");
	helpMap["clear"] = CommandHelp(
	    "clear <KEY>",
	    "clear a key from the database",
	    "Clear succeeds even if the specified key is not present, but may fail because of conflicts." ESCAPINGK);
	helpMap["clearrange"] = CommandHelp(
	    "clearrange <BEGINKEY> <ENDKEY>",
	    "clear a range of keys from the database",
	    "All keys between BEGINKEY (inclusive) and ENDKEY (exclusive) are cleared from the database. This command will "
	    "succeed even if the specified range is empty, but may fail because of conflicts." ESCAPINGK);
	helpMap["configure"] = CommandHelp(
	    "configure [new|tss]"
	    "<single|double|triple|three_data_hall|three_datacenter|ssd|memory|memory-radixtree-beta|proxies=<PROXIES>|"
	    "commit_proxies=<COMMIT_PROXIES>|grv_proxies=<GRV_PROXIES>|logs=<LOGS>|resolvers=<RESOLVERS>>*|"
	    "count=<TSS_COUNT>|perpetual_storage_wiggle=<WIGGLE_SPEED>",
	    "change the database configuration",
	    "The `new' option, if present, initializes a new database with the given configuration rather than changing "
	    "the configuration of an existing one. When used, both a redundancy mode and a storage engine must be "
	    "specified.\n\ntss: when enabled, configures the testing storage server for the cluster instead."
	    "When used with new to set up tss for the first time, it requires both a count and a storage engine."
	    "To disable the testing storage server, run \"configure tss count=0\"\n\n"
	    "Redundancy mode:\n  single - one copy of the data.  Not fault tolerant.\n  double - two copies "
	    "of data (survive one failure).\n  triple - three copies of data (survive two failures).\n  three_data_hall - "
	    "See the Admin Guide.\n  three_datacenter - See the Admin Guide.\n\nStorage engine:\n  ssd - B-Tree storage "
	    "engine optimized for solid state disks.\n  memory - Durable in-memory storage engine for small "
	    "datasets.\n\nproxies=<PROXIES>: Sets the desired number of proxies in the cluster. The proxy role is being "
	    "deprecated and split into GRV proxy and Commit proxy, now prefer configure 'grv_proxies' and 'commit_proxies' "
	    "separately. Generally we should follow that 'commit_proxies' is three times of 'grv_proxies' and "
	    "'grv_proxies' "
	    "should be not more than 4. If 'proxies' is specified, it will be converted to 'grv_proxies' and "
	    "'commit_proxies'. "
	    "Must be at least 2 (1 GRV proxy, 1 Commit proxy), or set to -1 which restores the number of proxies to the "
	    "default value.\n\ncommit_proxies=<COMMIT_PROXIES>: Sets the desired number of commit proxies in the cluster. "
	    "Must be at least 1, or set to -1 which restores the number of commit proxies to the default "
	    "value.\n\ngrv_proxies=<GRV_PROXIES>: Sets the desired number of GRV proxies in the cluster. Must be at least "
	    "1, or set to -1 which restores the number of GRV proxies to the default value.\n\nlogs=<LOGS>: Sets the "
	    "desired number of log servers in the cluster. Must be at least 1, or set to -1 which restores the number of "
	    "logs to the default value.\n\nresolvers=<RESOLVERS>: Sets the desired number of resolvers in the cluster. "
	    "Must be at least 1, or set to -1 which restores the number of resolvers to the default value.\n\n"
	    "perpetual_storage_wiggle=<WIGGLE_SPEED>: Set the value speed (a.k.a., the number of processes that the Data "
	    "Distributor should wiggle at a time). Currently, only 0 and 1 are supported. The value 0 means to disable the "
	    "perpetual storage wiggle.\n\n"
	    "See the FoundationDB Administration Guide for more information.");
	helpMap["fileconfigure"] = CommandHelp(
	    "fileconfigure [new] <FILENAME>",
	    "change the database configuration from a file",
	    "The `new' option, if present, initializes a new database with the given configuration rather than changing "
	    "the configuration of an existing one. Load a JSON document from the provided file, and change the database "
	    "configuration to match the contents of the JSON document. The format should be the same as the value of the "
	    "\"configuration\" entry in status JSON without \"excluded_servers\" or \"coordinators_count\".");
	helpMap["exit"] = CommandHelp("exit", "exit the CLI", "");
	helpMap["quit"] = CommandHelp();
	helpMap["waitconnected"] = CommandHelp();
	helpMap["waitopen"] = CommandHelp();
	helpMap["sleep"] = CommandHelp("sleep <SECONDS>", "sleep for a period of time", "");
	helpMap["get"] =
	    CommandHelp("get <KEY>",
	                "fetch the value for a given key",
	                "Displays the value of KEY in the database, or `not found' if KEY is not present." ESCAPINGK);
	helpMap["getrange"] =
	    CommandHelp("getrange <BEGINKEY> [ENDKEY] [LIMIT]",
	                "fetch key/value pairs in a range of keys",
	                "Displays up to LIMIT keys and values for keys between BEGINKEY (inclusive) and ENDKEY "
	                "(exclusive). If ENDKEY is omitted, then the range will include all keys starting with BEGINKEY. "
	                "LIMIT defaults to 25 if omitted." ESCAPINGK);
	helpMap["getrangekeys"] = CommandHelp(
	    "getrangekeys <BEGINKEY> [ENDKEY] [LIMIT]",
	    "fetch keys in a range of keys",
	    "Displays up to LIMIT keys for keys between BEGINKEY (inclusive) and ENDKEY (exclusive). If ENDKEY is omitted, "
	    "then the range will include all keys starting with BEGINKEY. LIMIT defaults to 25 if omitted." ESCAPINGK);
	helpMap["getversion"] =
	    CommandHelp("getversion",
	                "Fetch the current read version",
	                "Displays the current read version of the database or currently running transaction.");
	helpMap["reset"] =
	    CommandHelp("reset",
	                "reset the current transaction",
	                "Any sets or clears executed after the start of the active transaction will be discarded.");
	helpMap["rollback"] = CommandHelp("rollback",
	                                  "rolls back the current transaction",
	                                  "The active transaction will be discarded, including any sets or clears executed "
	                                  "since the transaction was started.");
	helpMap["set"] = CommandHelp("set <KEY> <VALUE>",
	                             "set a value for a given key",
	                             "If KEY is not already present in the database, it will be created." ESCAPINGKV);
	helpMap["option"] = CommandHelp(
	    "option <STATE> <OPTION> <ARG>",
	    "enables or disables an option",
	    "If STATE is `on', then the option OPTION will be enabled with optional parameter ARG, if required. If STATE "
	    "is `off', then OPTION will be disabled.\n\nIf there is no active transaction, then the option will be applied "
	    "to all operations as well as all subsequently created transactions (using `begin').\n\nIf there is an active "
	    "transaction (one created with `begin'), then enabled options apply only to that transaction. Options cannot "
	    "be disabled on an active transaction.\n\nCalling `option' with no parameters prints a list of all enabled "
	    "options.\n\nFor information about specific options that can be set, type `help options'.");
	helpMap["help"] = CommandHelp("help [<topic>]", "get help about a topic or command", "");
	helpMap["writemode"] = CommandHelp("writemode <on|off>",
	                                   "enables or disables sets and clears",
	                                   "Setting or clearing keys from the CLI is not recommended.");
}

void printVersion() {
	printf("FoundationDB CLI " FDB_VT_PACKAGE_NAME " (v" FDB_VT_VERSION ")\n");
	printf("source version %s\n", getSourceVersion());
	printf("protocol %" PRIx64 "\n", currentProtocolVersion.version());
}

void printBuildInformation() {
	printf("%s", jsonBuildInformation().c_str());
}

void printHelpOverview() {
	printf("\nList of commands:\n\n");
	for (const auto& [command, help] : helpMap) {
		if (help.short_desc.size())
			printf(" %s:\n      %s\n", command.c_str(), help.short_desc.c_str());
	}
	printf("\nFor information on a specific command, type `help <command>'.");
	printf("\nFor information on escaping keys and values, type `help escaping'.");
	printf("\nFor information on available options, type `help options'.\n\n");
}

void printHelp(StringRef command) {
	auto i = helpMap.find(command.toString());
	if (i != helpMap.end() && i->second.short_desc.size()) {
		printf("\n%s\n\n", i->second.usage.c_str());
		auto cstr = i->second.short_desc.c_str();
		printf("%c%s.\n", toupper(cstr[0]), cstr + 1);
		if (!i->second.long_desc.empty()) {
			printf("\n");
			printAtCol(i->second.long_desc.c_str(), 80);
		}
		printf("\n");
	} else
		printf("I don't know anything about `%s'\n", formatStringRef(command).c_str());
}

int printStatusFromJSON(std::string const& jsonFileName) {
	try {
		json_spirit::mValue value;
		json_spirit::read_string(readFileBytes(jsonFileName, 10000000), value);

		printStatus(value.get_obj(), StatusClient::DETAILED, false, true);

		return 0;
	} catch (std::exception& e) {
		printf("Exception printing status: %s\n", e.what());
		return 1;
	} catch (Error& e) {
		printf("Error printing status: %d %s\n", e.code(), e.what());
		return 2;
	} catch (...) {
		printf("Unknown exception printing status.\n");
		return 3;
	}
}

ACTOR Future<Void> timeWarning(double when, const char* msg) {
	wait(delay(when));
	fputs(msg, stderr);

	return Void();
}

ACTOR Future<Void> checkStatus(Future<Void> f,
                               Reference<IDatabase> db,
                               Database localDb,
                               bool displayDatabaseAvailable = true) {
	wait(f);
	state Reference<ITransaction> tr = db->createTransaction();
	state StatusObject s;
	if (!tr->isValid()) {
		StatusObject _s = wait(StatusClient::statusFetcher(localDb));
		s = _s;
	} else {
		state ThreadFuture<Optional<Value>> statusValueF = tr->get(LiteralStringRef("\xff\xff/status/json"));
		Optional<Value> statusValue = wait(safeThreadFutureToFuture(statusValueF));
		if (!statusValue.present()) {
			fprintf(stderr, "ERROR: Failed to get status json from the cluster\n");
			return Void();
		}
		json_spirit::mValue mv;
		json_spirit::read_string(statusValue.get().toString(), mv);
		s = StatusObject(mv.get_obj());
	}
	printf("\n");
	printStatus(s, StatusClient::MINIMAL, displayDatabaseAvailable);
	printf("\n");
	return Void();
}

ACTOR template <class T>
Future<T> makeInterruptable(Future<T> f) {
	Future<Void> interrupt = LineNoise::onKeyboardInterrupt();
	choose {
		when(T t = wait(f)) { return t; }
		when(wait(interrupt)) {
			f.cancel();
			throw operation_cancelled();
		}
	}
}

ACTOR Future<Void> commitTransaction(Reference<ITransaction> tr) {
	wait(makeInterruptable(safeThreadFutureToFuture(tr->commit())));
	auto ver = tr->getCommittedVersion();
	if (ver != invalidVersion)
		printf("Committed (%" PRId64 ")\n", ver);
	else
		printf("Nothing to commit\n");
	return Void();
}

ACTOR Future<bool> configure(Database db,
                             std::vector<StringRef> tokens,
                             Reference<ClusterConnectionFile> ccf,
                             LineNoise* linenoise,
                             Future<Void> warn) {
	state ConfigurationResult result;
	state int startToken = 1;
	state bool force = false;
	if (tokens.size() < 2)
		result = ConfigurationResult::NO_OPTIONS_PROVIDED;
	else {
		if (tokens[startToken] == LiteralStringRef("FORCE")) {
			force = true;
			startToken = 2;
		}

		state Optional<ConfigureAutoResult> conf;
		if (tokens[startToken] == LiteralStringRef("auto")) {
			StatusObject s = wait(makeInterruptable(StatusClient::statusFetcher(db)));
			if (warn.isValid())
				warn.cancel();

			conf = parseConfig(s);

			if (!conf.get().isValid()) {
				printf("Unable to provide advice for the current configuration.\n");
				return true;
			}

			bool noChanges = conf.get().old_replication == conf.get().auto_replication &&
			                 conf.get().old_logs == conf.get().auto_logs &&
			                 conf.get().old_commit_proxies == conf.get().auto_commit_proxies &&
			                 conf.get().old_grv_proxies == conf.get().auto_grv_proxies &&
			                 conf.get().old_resolvers == conf.get().auto_resolvers &&
			                 conf.get().old_processes_with_transaction == conf.get().auto_processes_with_transaction &&
			                 conf.get().old_machines_with_transaction == conf.get().auto_machines_with_transaction;

			bool noDesiredChanges = noChanges && conf.get().old_logs == conf.get().desired_logs &&
			                        conf.get().old_commit_proxies == conf.get().desired_commit_proxies &&
			                        conf.get().old_grv_proxies == conf.get().desired_grv_proxies &&
			                        conf.get().old_resolvers == conf.get().desired_resolvers;

			std::string outputString;

			outputString += "\nYour cluster has:\n\n";
			outputString += format("  processes %d\n", conf.get().processes);
			outputString += format("  machines  %d\n", conf.get().machines);

			if (noDesiredChanges)
				outputString += "\nConfigure recommends keeping your current configuration:\n\n";
			else if (noChanges)
				outputString +=
				    "\nConfigure cannot modify the configuration because some parameters have been set manually:\n\n";
			else
				outputString += "\nConfigure recommends the following changes:\n\n";
			outputString += " ------------------------------------------------------------------- \n";
			outputString += "| parameter                   | old              | new              |\n";
			outputString += " ------------------------------------------------------------------- \n";
			outputString += format("| replication                 | %16s | %16s |\n",
			                       conf.get().old_replication.c_str(),
			                       conf.get().auto_replication.c_str());
			outputString +=
			    format("| logs                        | %16d | %16d |", conf.get().old_logs, conf.get().auto_logs);
			outputString += conf.get().auto_logs != conf.get().desired_logs
			                    ? format(" (manually set; would be %d)\n", conf.get().desired_logs)
			                    : "\n";
			outputString += format("| commit_proxies              | %16d | %16d |",
			                       conf.get().old_commit_proxies,
			                       conf.get().auto_commit_proxies);
			outputString += conf.get().auto_commit_proxies != conf.get().desired_commit_proxies
			                    ? format(" (manually set; would be %d)\n", conf.get().desired_commit_proxies)
			                    : "\n";
			outputString += format("| grv_proxies                 | %16d | %16d |",
			                       conf.get().old_grv_proxies,
			                       conf.get().auto_grv_proxies);
			outputString += conf.get().auto_grv_proxies != conf.get().desired_grv_proxies
			                    ? format(" (manually set; would be %d)\n", conf.get().desired_grv_proxies)
			                    : "\n";
			outputString += format(
			    "| resolvers                   | %16d | %16d |", conf.get().old_resolvers, conf.get().auto_resolvers);
			outputString += conf.get().auto_resolvers != conf.get().desired_resolvers
			                    ? format(" (manually set; would be %d)\n", conf.get().desired_resolvers)
			                    : "\n";
			outputString += format("| transaction-class processes | %16d | %16d |\n",
			                       conf.get().old_processes_with_transaction,
			                       conf.get().auto_processes_with_transaction);
			outputString += format("| transaction-class machines  | %16d | %16d |\n",
			                       conf.get().old_machines_with_transaction,
			                       conf.get().auto_machines_with_transaction);
			outputString += " ------------------------------------------------------------------- \n\n";

			std::printf("%s", outputString.c_str());

			if (noChanges)
				return false;

			// TODO: disable completion
			Optional<std::string> line = wait(linenoise->read("Would you like to make these changes? [y/n]> "));

			if (!line.present() || (line.get() != "y" && line.get() != "Y")) {
				return false;
			}
		}

		ConfigurationResult r = wait(makeInterruptable(
		    changeConfig(db, std::vector<StringRef>(tokens.begin() + startToken, tokens.end()), conf, force)));
		result = r;
	}

	// Real errors get thrown from makeInterruptable and printed by the catch block in cli(), but
	// there are various results specific to changeConfig() that we need to report:
	bool ret;
	switch (result) {
	case ConfigurationResult::NO_OPTIONS_PROVIDED:
	case ConfigurationResult::CONFLICTING_OPTIONS:
	case ConfigurationResult::UNKNOWN_OPTION:
	case ConfigurationResult::INCOMPLETE_CONFIGURATION:
		printUsage(LiteralStringRef("configure"));
		ret = true;
		break;
	case ConfigurationResult::INVALID_CONFIGURATION:
		fprintf(stderr, "ERROR: These changes would make the configuration invalid\n");
		ret = true;
		break;
	case ConfigurationResult::DATABASE_ALREADY_CREATED:
		fprintf(stderr, "ERROR: Database already exists! To change configuration, don't say `new'\n");
		ret = true;
		break;
	case ConfigurationResult::DATABASE_CREATED:
		printf("Database created\n");
		ret = false;
		break;
	case ConfigurationResult::DATABASE_UNAVAILABLE:
		fprintf(stderr, "ERROR: The database is unavailable\n");
		fprintf(stderr, "Type `configure FORCE <TOKEN...>' to configure without this check\n");
		ret = true;
		break;
	case ConfigurationResult::STORAGE_IN_UNKNOWN_DCID:
		fprintf(stderr, "ERROR: All storage servers must be in one of the known regions\n");
		fprintf(stderr, "Type `configure FORCE <TOKEN...>' to configure without this check\n");
		ret = true;
		break;
	case ConfigurationResult::REGION_NOT_FULLY_REPLICATED:
		fprintf(stderr,
		        "ERROR: When usable_regions > 1, all regions with priority >= 0 must be fully replicated "
		        "before changing the configuration\n");
		fprintf(stderr, "Type `configure FORCE <TOKEN...>' to configure without this check\n");
		ret = true;
		break;
	case ConfigurationResult::MULTIPLE_ACTIVE_REGIONS:
		fprintf(stderr, "ERROR: When changing usable_regions, only one region can have priority >= 0\n");
		fprintf(stderr, "Type `configure FORCE <TOKEN...>' to configure without this check\n");
		ret = true;
		break;
	case ConfigurationResult::REGIONS_CHANGED:
		fprintf(stderr,
		        "ERROR: The region configuration cannot be changed while simultaneously changing usable_regions\n");
		fprintf(stderr, "Type `configure FORCE <TOKEN...>' to configure without this check\n");
		ret = true;
		break;
	case ConfigurationResult::NOT_ENOUGH_WORKERS:
		fprintf(stderr, "ERROR: Not enough processes exist to support the specified configuration\n");
		fprintf(stderr, "Type `configure FORCE <TOKEN...>' to configure without this check\n");
		ret = true;
		break;
	case ConfigurationResult::REGION_REPLICATION_MISMATCH:
		fprintf(stderr, "ERROR: `three_datacenter' replication is incompatible with region configuration\n");
		fprintf(stderr, "Type `configure FORCE <TOKEN...>' to configure without this check\n");
		ret = true;
		break;
	case ConfigurationResult::DCID_MISSING:
		fprintf(stderr, "ERROR: `No storage servers in one of the specified regions\n");
		fprintf(stderr, "Type `configure FORCE <TOKEN...>' to configure without this check\n");
		ret = true;
		break;
	case ConfigurationResult::SUCCESS:
		printf("Configuration changed\n");
		ret = false;
		break;
	case ConfigurationResult::LOCKED_NOT_NEW:
		fprintf(stderr, "ERROR: `only new databases can be configured as locked`\n");
		ret = true;
		break;
	default:
		ASSERT(false);
		ret = true;
	};
	return ret;
}

ACTOR Future<bool> fileConfigure(Database db, std::string filePath, bool isNewDatabase, bool force) {
	std::string contents(readFileBytes(filePath, 100000));
	json_spirit::mValue config;
	if (!json_spirit::read_string(contents, config)) {
		fprintf(stderr, "ERROR: Invalid JSON\n");
		return true;
	}
	if (config.type() != json_spirit::obj_type) {
		fprintf(stderr, "ERROR: Configuration file must contain a JSON object\n");
		return true;
	}
	StatusObject configJSON = config.get_obj();

	json_spirit::mValue schema;
	if (!json_spirit::read_string(JSONSchemas::clusterConfigurationSchema.toString(), schema)) {
		ASSERT(false);
	}

	std::string errorStr;
	if (!schemaMatch(schema.get_obj(), configJSON, errorStr)) {
		printf("%s", errorStr.c_str());
		return true;
	}

	std::string configString;
	if (isNewDatabase) {
		configString = "new";
	}

	for (const auto& [name, value] : configJSON) {
		if (!configString.empty()) {
			configString += " ";
		}
		if (value.type() == json_spirit::int_type) {
			configString += name + ":=" + format("%d", value.get_int());
		} else if (value.type() == json_spirit::str_type) {
			configString += value.get_str();
		} else if (value.type() == json_spirit::array_type) {
			configString +=
			    name + "=" +
			    json_spirit::write_string(json_spirit::mValue(value.get_array()), json_spirit::Output_options::none);
		} else {
			printUsage(LiteralStringRef("fileconfigure"));
			return true;
		}
	}
	ConfigurationResult result = wait(makeInterruptable(changeConfig(db, configString, force)));
	// Real errors get thrown from makeInterruptable and printed by the catch block in cli(), but
	// there are various results specific to changeConfig() that we need to report:
	bool ret;
	switch (result) {
	case ConfigurationResult::NO_OPTIONS_PROVIDED:
		fprintf(stderr, "ERROR: No options provided\n");
		ret = true;
		break;
	case ConfigurationResult::CONFLICTING_OPTIONS:
		fprintf(stderr, "ERROR: Conflicting options\n");
		ret = true;
		break;
	case ConfigurationResult::UNKNOWN_OPTION:
		fprintf(stderr, "ERROR: Unknown option\n"); // This should not be possible because of schema match
		ret = true;
		break;
	case ConfigurationResult::INCOMPLETE_CONFIGURATION:
		fprintf(stderr,
		        "ERROR: Must specify both a replication level and a storage engine when creating a new database\n");
		ret = true;
		break;
	case ConfigurationResult::INVALID_CONFIGURATION:
		fprintf(stderr, "ERROR: These changes would make the configuration invalid\n");
		ret = true;
		break;
	case ConfigurationResult::DATABASE_ALREADY_CREATED:
		fprintf(stderr, "ERROR: Database already exists! To change configuration, don't say `new'\n");
		ret = true;
		break;
	case ConfigurationResult::DATABASE_CREATED:
		printf("Database created\n");
		ret = false;
		break;
	case ConfigurationResult::DATABASE_UNAVAILABLE:
		fprintf(stderr, "ERROR: The database is unavailable\n");
		printf("Type `fileconfigure FORCE <FILENAME>' to configure without this check\n");
		ret = true;
		break;
	case ConfigurationResult::STORAGE_IN_UNKNOWN_DCID:
		fprintf(stderr, "ERROR: All storage servers must be in one of the known regions\n");
		printf("Type `fileconfigure FORCE <FILENAME>' to configure without this check\n");
		ret = true;
		break;
	case ConfigurationResult::REGION_NOT_FULLY_REPLICATED:
		fprintf(stderr,
		        "ERROR: When usable_regions > 1, All regions with priority >= 0 must be fully replicated "
		        "before changing the configuration\n");
		printf("Type `fileconfigure FORCE <FILENAME>' to configure without this check\n");
		ret = true;
		break;
	case ConfigurationResult::MULTIPLE_ACTIVE_REGIONS:
		fprintf(stderr, "ERROR: When changing usable_regions, only one region can have priority >= 0\n");
		printf("Type `fileconfigure FORCE <FILENAME>' to configure without this check\n");
		ret = true;
		break;
	case ConfigurationResult::REGIONS_CHANGED:
		fprintf(stderr,
		        "ERROR: The region configuration cannot be changed while simultaneously changing usable_regions\n");
		printf("Type `fileconfigure FORCE <FILENAME>' to configure without this check\n");
		ret = true;
		break;
	case ConfigurationResult::NOT_ENOUGH_WORKERS:
		fprintf(stderr, "ERROR: Not enough processes exist to support the specified configuration\n");
		printf("Type `fileconfigure FORCE <FILENAME>' to configure without this check\n");
		ret = true;
		break;
	case ConfigurationResult::REGION_REPLICATION_MISMATCH:
		fprintf(stderr, "ERROR: `three_datacenter' replication is incompatible with region configuration\n");
		printf("Type `fileconfigure FORCE <TOKEN...>' to configure without this check\n");
		ret = true;
		break;
	case ConfigurationResult::DCID_MISSING:
		fprintf(stderr, "ERROR: `No storage servers in one of the specified regions\n");
		printf("Type `fileconfigure FORCE <TOKEN...>' to configure without this check\n");
		ret = true;
		break;
	case ConfigurationResult::SUCCESS:
		printf("Configuration changed\n");
		ret = false;
		break;
	default:
		ASSERT(false);
		ret = true;
	};
	return ret;
}

ACTOR Future<bool> createSnapshot(Database db, std::vector<StringRef> tokens) {
	state Standalone<StringRef> snapCmd;
	state UID snapUID = deterministicRandom()->randomUniqueID();
	for (int i = 1; i < tokens.size(); i++) {
		snapCmd = snapCmd.withSuffix(tokens[i]);
		if (i != tokens.size() - 1) {
			snapCmd = snapCmd.withSuffix(LiteralStringRef(" "));
		}
	}
	try {
		wait(makeInterruptable(mgmtSnapCreate(db, snapCmd, snapUID)));
		printf("Snapshot command succeeded with UID %s\n", snapUID.toString().c_str());
	} catch (Error& e) {
		fprintf(stderr,
		        "Snapshot command failed %d (%s)."
		        " Please cleanup any instance level snapshots created with UID %s.\n",
		        e.code(),
		        e.what(),
		        snapUID.toString().c_str());
		return true;
	}
	return false;
}

// TODO: Update the function to get rid of the Database after refactoring
Reference<ITransaction> getTransaction(Reference<IDatabase> db,
                                       Reference<ITransaction>& tr,
                                       FdbOptions* options,
                                       bool intrans) {
	// Update "tr" to point to a brand new transaction object when it's not initialized or "intrans" flag is "false",
	// which indicates we need a new transaction object
	if (!tr || !intrans) {
		tr = db->createTransaction();
		options->apply(tr);
	}

	return tr;
}

std::string newCompletion(const char* base, const char* name) {
	return format("%s%s ", base, name);
}

void compGenerator(const char* text, bool help, std::vector<std::string>& lc) {
	std::map<std::string, CommandHelp>::const_iterator iter;
	int len = strlen(text);

	const char* helpExtra[] = { "escaping", "options", nullptr };

	const char** he = helpExtra;

	for (auto iter = helpMap.begin(); iter != helpMap.end(); ++iter) {
		const char* name = (*iter).first.c_str();
		if (!strncmp(name, text, len)) {
			lc.push_back(newCompletion(help ? "help " : "", name));
		}
	}

	if (help) {
		while (*he) {
			const char* name = *he;
			he++;
			if (!strncmp(name, text, len))
				lc.push_back(newCompletion("help ", name));
		}
	}
}

void cmdGenerator(const char* text, std::vector<std::string>& lc) {
	compGenerator(text, false, lc);
}

void helpGenerator(const char* text, std::vector<std::string>& lc) {
	compGenerator(text, true, lc);
}

void optionGenerator(const char* text, const char* line, std::vector<std::string>& lc) {
	int len = strlen(text);

	for (auto iter = validOptions.begin(); iter != validOptions.end(); ++iter) {
		const char* name = (*iter).c_str();
		if (!strncmp(name, text, len)) {
			lc.push_back(newCompletion(line, name));
		}
	}
}

void arrayGenerator(const char* text, const char* line, const char** options, std::vector<std::string>& lc) {
	const char** iter = options;
	int len = strlen(text);

	while (*iter) {
		const char* name = *iter;
		iter++;
		if (!strncmp(name, text, len)) {
			lc.push_back(newCompletion(line, name));
		}
	}
}

void onOffGenerator(const char* text, const char* line, std::vector<std::string>& lc) {
	const char* opts[] = { "on", "off", nullptr };
	arrayGenerator(text, line, opts, lc);
}

void configureGenerator(const char* text, const char* line, std::vector<std::string>& lc) {
	const char* opts[] = { "new",
		                   "single",
		                   "double",
		                   "triple",
		                   "three_data_hall",
		                   "three_datacenter",
		                   "ssd",
		                   "ssd-1",
		                   "ssd-2",
		                   "memory",
		                   "memory-1",
		                   "memory-2",
		                   "memory-radixtree-beta",
		                   "commit_proxies=",
		                   "grv_proxies=",
		                   "logs=",
		                   "resolvers=",
		                   "perpetual_storage_wiggle=",
		                   nullptr };
	arrayGenerator(text, line, opts, lc);
}

void statusGenerator(const char* text, const char* line, std::vector<std::string>& lc) {
	const char* opts[] = { "minimal", "details", "json", nullptr };
	arrayGenerator(text, line, opts, lc);
}

void killGenerator(const char* text, const char* line, std::vector<std::string>& lc) {
	const char* opts[] = { "all", "list", nullptr };
	arrayGenerator(text, line, opts, lc);
}

void throttleGenerator(const char* text,
                       const char* line,
                       std::vector<std::string>& lc,
                       std::vector<StringRef> const& tokens) {
	if (tokens.size() == 1) {
		const char* opts[] = { "on tag", "off", "enable auto", "disable auto", "list", nullptr };
		arrayGenerator(text, line, opts, lc);
	} else if (tokens.size() >= 2 && tokencmp(tokens[1], "on")) {
		if (tokens.size() == 2) {
			const char* opts[] = { "tag", nullptr };
			arrayGenerator(text, line, opts, lc);
		} else if (tokens.size() == 6) {
			const char* opts[] = { "default", "immediate", "batch", nullptr };
			arrayGenerator(text, line, opts, lc);
		}
	} else if (tokens.size() >= 2 && tokencmp(tokens[1], "off") && !tokencmp(tokens[tokens.size() - 1], "tag")) {
		const char* opts[] = { "all", "auto", "manual", "tag", "default", "immediate", "batch", nullptr };
		arrayGenerator(text, line, opts, lc);
	} else if (tokens.size() == 2 && (tokencmp(tokens[1], "enable") || tokencmp(tokens[1], "disable"))) {
		const char* opts[] = { "auto", nullptr };
		arrayGenerator(text, line, opts, lc);
	} else if (tokens.size() >= 2 && tokencmp(tokens[1], "list")) {
		if (tokens.size() == 2) {
			const char* opts[] = { "throttled", "recommended", "all", nullptr };
			arrayGenerator(text, line, opts, lc);
		} else if (tokens.size() == 3) {
			const char* opts[] = { "LIMITS", nullptr };
			arrayGenerator(text, line, opts, lc);
		}
	}
}

void fdbcliCompCmd(std::string const& text, std::vector<std::string>& lc) {
	bool err, partial;
	std::string whole_line = text;
	auto parsed = parseLine(whole_line, err, partial);
	if (err || partial) // If there was an error, or we are partially through a quoted sequence
		return;

	auto tokens = parsed.back();
	int count = tokens.size();

	// for(int i = 0; i < count; i++) {
	// 	printf("Token (%d): `%s'\n", i, tokens[i].toString().c_str());
	// }

	std::string ntext = "";
	std::string base_input = text;

	// If there is a token and the input does not end in a space
	if (count && text.size() > 0 && text[text.size() - 1] != ' ') {
		count--; // Ignore the last token for purposes of later code
		ntext = tokens.back().toString();
		base_input = whole_line.substr(0, whole_line.rfind(ntext));
	}

	// printf("final text (%d tokens): `%s' & `%s'\n", count, base_input.c_str(), ntext.c_str());

	if (!count) {
		cmdGenerator(ntext.c_str(), lc);
		return;
	}

	if (tokencmp(tokens[0], "help") && count == 1) {
		helpGenerator(ntext.c_str(), lc);
		return;
	}

	if (tokencmp(tokens[0], "option")) {
		if (count == 1)
			onOffGenerator(ntext.c_str(), base_input.c_str(), lc);
		if (count == 2)
			optionGenerator(ntext.c_str(), base_input.c_str(), lc);
	}

	if (tokencmp(tokens[0], "writemode") && count == 1) {
		onOffGenerator(ntext.c_str(), base_input.c_str(), lc);
	}

	if (tokencmp(tokens[0], "configure")) {
		configureGenerator(ntext.c_str(), base_input.c_str(), lc);
	}

	if (tokencmp(tokens[0], "status") && count == 1) {
		statusGenerator(ntext.c_str(), base_input.c_str(), lc);
	}

	if (tokencmp(tokens[0], "kill") && count == 1) {
		killGenerator(ntext.c_str(), base_input.c_str(), lc);
	}

	if (tokencmp(tokens[0], "throttle")) {
		throttleGenerator(ntext.c_str(), base_input.c_str(), lc, tokens);
	}
}

std::vector<const char*> throttleHintGenerator(std::vector<StringRef> const& tokens, bool inArgument) {
	if (tokens.size() == 1) {
		return { "<on|off|enable auto|disable auto|list>", "[ARGS]" };
	} else if (tokencmp(tokens[1], "on")) {
		std::vector<const char*> opts = { "tag", "<TAG>", "[RATE]", "[DURATION]", "[default|immediate|batch]" };
		if (tokens.size() == 2) {
			return opts;
		} else if (((tokens.size() == 3 && inArgument) || tokencmp(tokens[2], "tag")) && tokens.size() < 7) {
			return std::vector<const char*>(opts.begin() + tokens.size() - 2, opts.end());
		}
	} else if (tokencmp(tokens[1], "off")) {
		if (tokencmp(tokens[tokens.size() - 1], "tag")) {
			return { "<TAG>" };
		} else {
			bool hasType = false;
			bool hasTag = false;
			bool hasPriority = false;
			for (int i = 2; i < tokens.size(); ++i) {
				if (tokencmp(tokens[i], "all") || tokencmp(tokens[i], "auto") || tokencmp(tokens[i], "manual")) {
					hasType = true;
				} else if (tokencmp(tokens[i], "default") || tokencmp(tokens[i], "immediate") ||
				           tokencmp(tokens[i], "batch")) {
					hasPriority = true;
				} else if (tokencmp(tokens[i], "tag")) {
					hasTag = true;
					++i;
				} else {
					return {};
				}
			}

			std::vector<const char*> options;
			if (!hasType) {
				options.push_back("[all|auto|manual]");
			}
			if (!hasTag) {
				options.push_back("[tag <TAG>]");
			}
			if (!hasPriority) {
				options.push_back("[default|immediate|batch]");
			}

			return options;
		}
	} else if ((tokencmp(tokens[1], "enable") || tokencmp(tokens[1], "disable")) && tokens.size() == 2) {
		return { "auto" };
	} else if (tokens.size() >= 2 && tokencmp(tokens[1], "list")) {
		if (tokens.size() == 2) {
			return { "[throttled|recommended|all]", "[LIMITS]" };
		} else if (tokens.size() == 3 && (tokencmp(tokens[2], "throttled") || tokencmp(tokens[2], "recommended") ||
		                                  tokencmp(tokens[2], "all"))) {
			return { "[LIMITS]" };
		}
	} else if (tokens.size() == 2 && inArgument) {
		return { "[ARGS]" };
	}

	return std::vector<const char*>();
}

void LogCommand(std::string line, UID randomID, std::string errMsg) {
	printf("%s\n", errMsg.c_str());
	TraceEvent(SevInfo, "CLICommandLog", randomID).detail("Command", line).detail("Error", errMsg);
}

struct CLIOptions {
	std::string program_name;
	int exit_code = -1;

	std::string commandLine;

	std::string clusterFile;
	bool trace = false;
	std::string traceDir;
	std::string traceFormat;
	int exit_timeout = 0;
	Optional<std::string> exec;
	bool initialStatusCheck = true;
	bool cliHints = true;
	bool debugTLS = false;
	std::string tlsCertPath;
	std::string tlsKeyPath;
	std::string tlsVerifyPeers;
	std::string tlsCAPath;
	std::string tlsPassword;

	std::vector<std::pair<std::string, std::string>> knobs;

	CLIOptions(int argc, char* argv[]) {
		program_name = argv[0];
		for (int a = 0; a < argc; a++) {
			if (a)
				commandLine += ' ';
			commandLine += argv[a];
		}

		CSimpleOpt args(argc, argv, g_rgOptions);

		while (args.Next()) {
			int ec = processArg(args);
			if (ec != -1) {
				exit_code = ec;
				return;
			}
		}
		if (exit_timeout && !exec.present()) {
			fprintf(stderr, "ERROR: --timeout may only be specified with --exec\n");
			exit_code = FDB_EXIT_ERROR;
			return;
		}

		auto& g_knobs = IKnobCollection::getMutableGlobalKnobCollection();
		for (const auto& [knobName, knobValueString] : knobs) {
			try {
				auto knobValue = g_knobs.parseKnobValue(knobName, knobValueString);
				g_knobs.setKnob(knobName, knobValue);
			} catch (Error& e) {
				if (e.code() == error_code_invalid_option_value) {
					fprintf(stderr,
					        "WARNING: Invalid value '%s' for knob option '%s'\n",
					        knobValueString.c_str(),
					        knobName.c_str());
					TraceEvent(SevWarnAlways, "InvalidKnobValue")
					    .detail("Knob", printable(knobName))
					    .detail("Value", printable(knobValueString));
				} else {
					fprintf(stderr, "ERROR: Failed to set knob option '%s': %s\n", knobName.c_str(), e.what());
					TraceEvent(SevError, "FailedToSetKnob")
					    .detail("Knob", printable(knobName))
					    .detail("Value", printable(knobValueString))
					    .error(e);
					exit_code = FDB_EXIT_ERROR;
				}
			}
		}

		// Reinitialize knobs in order to update knobs that are dependent on explicitly set knobs
		g_knobs.initialize(Randomize::False, IsSimulated::False);
	}

	int processArg(CSimpleOpt& args) {
		if (args.LastError() != SO_SUCCESS) {
			printProgramUsage(program_name.c_str());
			return 1;
		}

		switch (args.OptionId()) {
		case OPT_CONNFILE:
			clusterFile = args.OptionArg();
			break;
		case OPT_TRACE:
			trace = true;
			break;
		case OPT_TRACE_DIR:
			traceDir = args.OptionArg();
			break;
		case OPT_TIMEOUT: {
			char* endptr;
			exit_timeout = strtoul((char*)args.OptionArg(), &endptr, 10);
			if (*endptr != '\0') {
				fprintf(stderr, "ERROR: invalid timeout %s\n", args.OptionArg());
				return 1;
			}
			break;
		}
		case OPT_EXEC:
			exec = args.OptionArg();
			break;
		case OPT_NO_STATUS:
			initialStatusCheck = false;
			break;
		case OPT_NO_HINTS:
			cliHints = false;

#ifndef TLS_DISABLED
		// TLS Options
		case TLSConfig::OPT_TLS_PLUGIN:
			args.OptionArg();
			break;
		case TLSConfig::OPT_TLS_CERTIFICATES:
			tlsCertPath = args.OptionArg();
			break;
		case TLSConfig::OPT_TLS_CA_FILE:
			tlsCAPath = args.OptionArg();
			break;
		case TLSConfig::OPT_TLS_KEY:
			tlsKeyPath = args.OptionArg();
			break;
		case TLSConfig::OPT_TLS_PASSWORD:
			tlsPassword = args.OptionArg();
			break;
		case TLSConfig::OPT_TLS_VERIFY_PEERS:
			tlsVerifyPeers = args.OptionArg();
			break;
#endif
		case OPT_HELP:
			printProgramUsage(program_name.c_str());
			return 0;
		case OPT_STATUS_FROM_JSON:
			return printStatusFromJSON(args.OptionArg());
		case OPT_TRACE_FORMAT:
			if (!validateTraceFormat(args.OptionArg())) {
				fprintf(stderr, "WARNING: Unrecognized trace format `%s'\n", args.OptionArg());
			}
			traceFormat = args.OptionArg();
			break;
		case OPT_KNOB: {
			std::string syn = args.OptionSyntax();
			if (!StringRef(syn).startsWith(LiteralStringRef("--knob_"))) {
				fprintf(stderr, "ERROR: unable to parse knob option '%s'\n", syn.c_str());
				return FDB_EXIT_ERROR;
			}
			syn = syn.substr(7);
			knobs.emplace_back(syn, args.OptionArg());
			break;
		}
		case OPT_DEBUG_TLS:
			debugTLS = true;
			break;
		case OPT_VERSION:
			printVersion();
			return FDB_EXIT_SUCCESS;
		case OPT_BUILD_FLAGS:
			printBuildInformation();
			return FDB_EXIT_SUCCESS;
		}
		return -1;
	}
};

ACTOR template <class T>
Future<T> stopNetworkAfter(Future<T> what) {
	try {
		T t = wait(what);
		API->stopNetwork();
		return t;
	} catch (...) {
		API->stopNetwork();
		throw;
	}
}

ACTOR Future<int> cli(CLIOptions opt, LineNoise* plinenoise) {
	state LineNoise& linenoise = *plinenoise;
	state bool intrans = false;

	state Database localDb;
	state Reference<IDatabase> db;
	state Reference<ITransaction> tr;

	state bool writeMode = false;

	state std::string clusterConnectString;
	state std::map<Key, std::pair<Value, ClientLeaderRegInterface>> address_interface;

	state FdbOptions globalOptions;
	state FdbOptions activeOptions;

	state FdbOptions* options = &globalOptions;

	state Reference<ClusterConnectionFile> ccf;

	state std::pair<std::string, bool> resolvedClusterFile =
	    ClusterConnectionFile::lookupClusterFileName(opt.clusterFile);
	try {
		ccf = makeReference<ClusterConnectionFile>(resolvedClusterFile.first);
	} catch (Error& e) {
		fprintf(stderr, "%s\n", ClusterConnectionFile::getErrorString(resolvedClusterFile, e).c_str());
		return 1;
	}

	// Ordinarily, this is done when the network is run. However, network thread should be set before TraceEvents are
	// logged. This thread will eventually run the network, so call it now.
	TraceEvent::setNetworkThread();

	try {
		localDb = Database::createDatabase(ccf, -1, IsInternal::False);
		if (!opt.exec.present()) {
			printf("Using cluster file `%s'.\n", ccf->getFilename().c_str());
		}
		db = API->createDatabase(opt.clusterFile.c_str());
	} catch (Error& e) {
		fprintf(stderr, "ERROR: %s (%d)\n", e.what(), e.code());
		printf("Unable to connect to cluster from `%s'\n", ccf->getFilename().c_str());
		return 1;
	}

	if (opt.trace) {
		TraceEvent("CLIProgramStart")
		    .setMaxEventLength(12000)
		    .detail("SourceVersion", getSourceVersion())
		    .detail("Version", FDB_VT_VERSION)
		    .detail("PackageName", FDB_VT_PACKAGE_NAME)
		    .detailf("ActualTime", "%lld", DEBUG_DETERMINISM ? 0 : time(nullptr))
		    .detail("ClusterFile", ccf->getFilename().c_str())
		    .detail("ConnectionString", ccf->getConnectionString().toString())
		    .setMaxFieldLength(10000)
		    .detail("CommandLine", opt.commandLine)
		    .trackLatest("ProgramStart");
	}

	// used to catch the first cluster_version_changed error when using external clients
	// when using external clients, it will throw cluster_version_changed for the first time establish the connection to
	// the cluster. Thus, we catch it by doing a get version request to establish the connection
	// The 3.0 timeout is a guard to avoid waiting forever when the cli cannot talk to any coordinators
	loop {
		try {
			getTransaction(db, tr, options, intrans);
			tr->setOption(FDBTransactionOptions::LOCK_AWARE);
			wait(delay(3.0) || success(safeThreadFutureToFuture(tr->getReadVersion())));
			break;
		} catch (Error& e) {
			if (e.code() == error_code_cluster_version_changed) {
				wait(safeThreadFutureToFuture(tr->onError(e)));
			} else {
				// unexpected errors
				fprintf(stderr, "ERROR: unexpected error %d while initializing the multiversion database\n", e.code());
				tr->reset();
				break;
			}
		}
	}

	if (!opt.exec.present()) {
		if (opt.initialStatusCheck) {
			Future<Void> checkStatusF = checkStatus(Void(), db, localDb);
			wait(makeInterruptable(success(checkStatusF)));
		} else {
			printf("\n");
		}

		printf("Welcome to the fdbcli. For help, type `help'.\n");
		validOptions = options->getValidOptions();
	}

	state bool is_error = false;

	state Future<Void> warn;
	loop {
		if (warn.isValid())
			warn.cancel();

		state std::string line;

		if (opt.exec.present()) {
			line = opt.exec.get();
		} else {
			Optional<std::string> rawline = wait(linenoise.read("fdb> "));
			if (!rawline.present()) {
				printf("\n");
				return 0;
			}
			line = rawline.get();

			if (!line.size())
				continue;

			// Don't put dangerous commands in the command history
			if (line.find("writemode") == std::string::npos && line.find("expensive_data_check") == std::string::npos &&
			    line.find("unlock") == std::string::npos)
				linenoise.historyAdd(line);
		}

		warn = checkStatus(timeWarning(5.0, "\nWARNING: Long delay (Ctrl-C to interrupt)\n"), db, localDb);

		try {
			state UID randomID = deterministicRandom()->randomUniqueID();
			TraceEvent(SevInfo, "CLICommandLog", randomID).detail("Command", line);

			bool malformed, partial;
			state std::vector<std::vector<StringRef>> parsed = parseLine(line, malformed, partial);
			if (malformed)
				LogCommand(line, randomID, "ERROR: malformed escape sequence");
			if (partial)
				LogCommand(line, randomID, "ERROR: unterminated quote");
			if (malformed || partial) {
				if (parsed.size() > 0) {
					// Denote via a special token that the command was a parse failure.
					auto& last_command = parsed.back();
					last_command.insert(last_command.begin(),
					                    StringRef((const uint8_t*)"parse_error", strlen("parse_error")));
				}
			}

			state bool multi = parsed.size() > 1;
			is_error = false;

			state std::vector<std::vector<StringRef>>::iterator iter;
			for (iter = parsed.begin(); iter != parsed.end(); ++iter) {
				state std::vector<StringRef> tokens = *iter;

				if (is_error) {
					printf("WARNING: the previous command failed, the remaining commands will not be executed.\n");
					break;
				}

				if (!tokens.size())
					continue;

				if (tokencmp(tokens[0], "parse_error")) {
					fprintf(stderr, "ERROR: Command failed to completely parse.\n");
					if (tokens.size() > 1) {
						fprintf(stderr, "ERROR: Not running partial or malformed command:");
						for (auto t = tokens.begin() + 1; t != tokens.end(); ++t)
							printf(" %s", formatStringRef(*t, true).c_str());
						printf("\n");
					}
					is_error = true;
					continue;
				}

				if (multi) {
					printf(">>>");
					for (auto t = tokens.begin(); t != tokens.end(); ++t)
						printf(" %s", formatStringRef(*t, true).c_str());
					printf("\n");
				}

				if (!helpMap.count(tokens[0].toString()) && !hiddenCommands.count(tokens[0].toString())) {
					fprintf(stderr, "ERROR: Unknown command `%s'. Try `help'?\n", formatStringRef(tokens[0]).c_str());
					is_error = true;
					continue;
				}

				if (tokencmp(tokens[0], "exit") || tokencmp(tokens[0], "quit")) {
					return 0;
				}

				if (tokencmp(tokens[0], "help")) {
					if (tokens.size() == 1) {
						printHelpOverview();
					} else if (tokens.size() == 2) {
						if (tokencmp(tokens[1], "escaping"))
							printf("\n"
							       "When parsing commands, fdbcli considers a space to delimit individual tokens.\n"
							       "To include a space in a single token, you may either enclose the token in\n"
							       "quotation marks (\"hello world\"), prefix the space with a backslash\n"
							       "(hello\\ world), or encode the space as a hex byte (hello\\x20world).\n"
							       "\n"
							       "To include a literal quotation mark in a token, precede it with a backslash\n"
							       "(\\\"hello\\ world\\\").\n"
							       "\n"
							       "To express a binary value, encode each byte as a two-digit hex byte, preceded\n"
							       "by \\x (e.g. \\x20 for a space character, or \\x0a\\x00\\x00\\x00 for a\n"
							       "32-bit, little-endian representation of the integer 10).\n"
							       "\n"
							       "All keys and values are displayed by the fdbcli with non-printable characters\n"
							       "and spaces encoded as two-digit hex bytes.\n\n");
						else if (tokencmp(tokens[1], "options")) {
							printf("\n"
							       "The following options are available to be set using the `option' command:\n"
							       "\n");
							options->printHelpString();
						} else if (tokencmp(tokens[1], "help"))
							printHelpOverview();
						else
							printHelp(tokens[1]);
					} else
						printf("Usage: help [topic]\n");
					continue;
				}

				if (tokencmp(tokens[0], "waitconnected")) {
					wait(makeInterruptable(localDb->onConnected()));
					continue;
				}

				if (tokencmp(tokens[0], "waitopen")) {
					wait(success(safeThreadFutureToFuture(getTransaction(db, tr, options, intrans)->getReadVersion())));
					continue;
				}

				if (tokencmp(tokens[0], "sleep")) {
					if (tokens.size() != 2) {
						printUsage(tokens[0]);
						is_error = true;
					} else {
						double v;
						int n = 0;
						if (sscanf(tokens[1].toString().c_str(), "%lf%n", &v, &n) != 1 || n != tokens[1].size()) {
							printUsage(tokens[0]);
							is_error = true;
						} else {
							wait(delay(v));
						}
					}
					continue;
				}

				if (tokencmp(tokens[0], "status")) {
					// Warn at 7 seconds since status will spend as long as 5 seconds trying to read/write from the
					// database
					warn = timeWarning(7.0, "\nWARNING: Long delay (Ctrl-C to interrupt)\n");
					bool _result = wait(makeInterruptable(statusCommandActor(db, localDb, tokens, opt.exec.present())));
					if (!_result)
						is_error = true;
					continue;
				}

				if (tokencmp(tokens[0], "triggerddteaminfolog")) {
					wait(triggerddteaminfologCommandActor(db));
					continue;
				}

				if (tokencmp(tokens[0], "tssq")) {
					bool _result = wait(makeInterruptable(tssqCommandActor(db, tokens)));
					if (!_result)
						is_error = true;
					continue;
				}

				if (tokencmp(tokens[0], "configure")) {
					bool err = wait(configure(localDb, tokens, localDb->getConnectionFile(), &linenoise, warn));
					if (err)
						is_error = true;
					continue;
				}

				if (tokencmp(tokens[0], "fileconfigure")) {
					if (tokens.size() == 2 || (tokens.size() == 3 && (tokens[1] == LiteralStringRef("new") ||
					                                                  tokens[1] == LiteralStringRef("FORCE")))) {
						bool err = wait(fileConfigure(localDb,
						                              tokens.back().toString(),
						                              tokens[1] == LiteralStringRef("new"),
						                              tokens[1] == LiteralStringRef("FORCE")));
						if (err)
							is_error = true;
					} else {
						printUsage(tokens[0]);
						is_error = true;
					}
					continue;
				}

				if (tokencmp(tokens[0], "coordinators")) {
					bool _result = wait(makeInterruptable(coordinatorsCommandActor(db, tokens)));
					if (!_result)
						is_error = true;
					continue;
				}

				if (tokencmp(tokens[0], "exclude")) {
					bool _result = wait(makeInterruptable(excludeCommandActor(db, tokens, warn)));
					if (!_result)
						is_error = true;
					continue;
				}

				if (tokencmp(tokens[0], "include")) {
					bool _result = wait(makeInterruptable(includeCommandActor(db, tokens)));
					if (!_result)
						is_error = true;
					continue;
				}

				if (tokencmp(tokens[0], "snapshot")) {
					bool _result = wait(snapshotCommandActor(db, tokens));
					if (!_result)
						is_error = true;
					continue;
				}

				if (tokencmp(tokens[0], "lock")) {
					bool _result = wait(lockCommandActor(db, tokens));
					if (!_result)
						is_error = true;
					continue;
				}

				if (tokencmp(tokens[0], "unlock")) {
					if ((tokens.size() != 2) || (tokens[1].size() != 32) ||
					    !std::all_of(tokens[1].begin(), tokens[1].end(), &isxdigit)) {
						printUsage(tokens[0]);
						is_error = true;
					} else {
						state std::string passPhrase = deterministicRandom()->randomAlphaNumeric(10);
						warn.cancel(); // don't warn while waiting on user input
						printf("Unlocking the database is a potentially dangerous operation.\n");
						printf("%s\n", passPhrase.c_str());
						fflush(stdout);
						Optional<std::string> input =
						    wait(linenoise.read(format("Repeat the above passphrase if you would like to proceed:")));
						warn =
						    checkStatus(timeWarning(5.0, "\nWARNING: Long delay (Ctrl-C to interrupt)\n"), db, localDb);
						if (input.present() && input.get() == passPhrase) {
							UID unlockUID = UID::fromString(tokens[1].toString());
							bool _result = wait(makeInterruptable(unlockDatabaseActor(db, unlockUID)));
							if (!_result)
								is_error = true;
						} else {
							fprintf(stderr, "ERROR: Incorrect passphrase entered.\n");
							is_error = true;
						}
					}
					continue;
				}

				if (tokencmp(tokens[0], "setclass")) {
					bool _result = wait(makeInterruptable(setClassCommandActor(db, tokens)));
					if (!_result)
						is_error = true;
					continue;
				}

				if (tokencmp(tokens[0], "begin")) {
					if (tokens.size() != 1) {
						printUsage(tokens[0]);
						is_error = true;
					} else if (intrans) {
						fprintf(stderr, "ERROR: Already in transaction\n");
						is_error = true;
					} else {
						activeOptions = FdbOptions(globalOptions);
						options = &activeOptions;
						getTransaction(db, tr, options, false);
						intrans = true;
						printf("Transaction started\n");
					}
					continue;
				}

				if (tokencmp(tokens[0], "commit")) {
					if (tokens.size() != 1) {
						printUsage(tokens[0]);
						is_error = true;
					} else if (!intrans) {
						fprintf(stderr, "ERROR: No active transaction\n");
						is_error = true;
					} else {
						wait(commitTransaction(tr));
						intrans = false;
						options = &globalOptions;
					}

					continue;
				}

				if (tokencmp(tokens[0], "reset")) {
					if (tokens.size() != 1) {
						printUsage(tokens[0]);
						is_error = true;
					} else if (!intrans) {
						fprintf(stderr, "ERROR: No active transaction\n");
						is_error = true;
					} else {
						tr->reset();
						activeOptions = FdbOptions(globalOptions);
						options = &activeOptions;
						options->apply(tr);
						printf("Transaction reset\n");
					}
					continue;
				}

				if (tokencmp(tokens[0], "rollback")) {
					if (tokens.size() != 1) {
						printUsage(tokens[0]);
						is_error = true;
					} else if (!intrans) {
						fprintf(stderr, "ERROR: No active transaction\n");
						is_error = true;
					} else {
						intrans = false;
						options = &globalOptions;
						printf("Transaction rolled back\n");
					}
					continue;
				}

				if (tokencmp(tokens[0], "get")) {
					if (tokens.size() != 2) {
						printUsage(tokens[0]);
						is_error = true;
					} else {
						state ThreadFuture<Optional<Value>> valueF =
						    getTransaction(db, tr, options, intrans)->get(tokens[1]);
						Optional<Standalone<StringRef>> v = wait(makeInterruptable(safeThreadFutureToFuture(valueF)));

						if (v.present())
							printf("`%s' is `%s'\n", printable(tokens[1]).c_str(), printable(v.get()).c_str());
						else
							printf("`%s': not found\n", printable(tokens[1]).c_str());
					}
					continue;
				}

				if (tokencmp(tokens[0], "getversion")) {
					if (tokens.size() != 1) {
						printUsage(tokens[0]);
						is_error = true;
					} else {
						Version v = wait(makeInterruptable(
						    safeThreadFutureToFuture(getTransaction(db, tr, options, intrans)->getReadVersion())));
						printf("%ld\n", v);
					}
					continue;
				}

				if (tokencmp(tokens[0], "advanceversion")) {
					bool _result = wait(makeInterruptable(advanceVersionCommandActor(db, tokens)));
					if (!_result)
						is_error = true;
					continue;
				}

				if (tokencmp(tokens[0], "kill")) {
					getTransaction(db, tr, options, intrans);
					bool _result = wait(makeInterruptable(killCommandActor(db, tr, tokens, &address_interface)));
					if (!_result)
						is_error = true;
					continue;
				}

				if (tokencmp(tokens[0], "suspend")) {
					getTransaction(db, tr, options, intrans);
					bool _result = wait(makeInterruptable(suspendCommandActor(db, tr, tokens, &address_interface)));
					if (!_result)
						is_error = true;
					continue;
				}

				if (tokencmp(tokens[0], "force_recovery_with_data_loss")) {
					bool _result = wait(makeInterruptable(forceRecoveryWithDataLossCommandActor(db, tokens)));
					if (!_result)
						is_error = true;
					continue;
				}

				if (tokencmp(tokens[0], "maintenance")) {
					bool _result = wait(makeInterruptable(maintenanceCommandActor(db, tokens)));
					if (!_result)
						is_error = true;
					continue;
				}

				if (tokencmp(tokens[0], "consistencycheck")) {
					getTransaction(db, tr, options, intrans);
					bool _result = wait(makeInterruptable(consistencyCheckCommandActor(tr, tokens, intrans)));
					if (!_result)
						is_error = true;
					continue;
				}

				if (tokencmp(tokens[0], "profile")) {
					getTransaction(db, tr, options, intrans);
					bool _result = wait(makeInterruptable(profileCommandActor(tr, tokens, intrans)));
					if (!_result)
						is_error = true;
					continue;
				}

				if (tokencmp(tokens[0], "expensive_data_check")) {
					getTransaction(db, tr, options, intrans);
					bool _result =
					    wait(makeInterruptable(expensiveDataCheckCommandActor(db, tr, tokens, &address_interface)));
					if (!_result)
						is_error = true;
					continue;
				}

				if (tokencmp(tokens[0], "getrange") ||
				    tokencmp(tokens[0], "getrangekeys")) { // FIXME: support byte limits, and reverse range reads
					if (tokens.size() < 2 || tokens.size() > 4) {
						printUsage(tokens[0]);
						is_error = true;
					} else {
						state int limit;
						bool valid = true;

						if (tokens.size() == 4) {
							// INT_MAX is 10 digits; rather than
							// worrying about overflow we'll just cap
							// limit at the (already absurd)
							// nearly-a-billion
							if (tokens[3].size() > 9) {
								fprintf(stderr, "ERROR: bad limit\n");
								is_error = true;
								continue;
							}
							limit = 0;
							int place = 1;
							for (int i = tokens[3].size(); i > 0; i--) {
								int val = int(tokens[3][i - 1]) - int('0');
								if (val < 0 || val > 9) {
									valid = false;
									break;
								}
								limit += val * place;
								place *= 10;
							}
							if (!valid) {
								fprintf(stderr, "ERROR: bad limit\n");
								is_error = true;
								continue;
							}
						} else {
							limit = 25;
						}

						Standalone<StringRef> endKey;
						if (tokens.size() >= 3) {
							endKey = tokens[2];
						} else if (tokens[1].size() == 0) {
							endKey = normalKeys.end;
						} else if (tokens[1] == systemKeys.begin) {
							endKey = systemKeys.end;
						} else if (tokens[1] >= allKeys.end) {
							throw key_outside_legal_range();
						} else {
							endKey = strinc(tokens[1]);
						}

						state ThreadFuture<RangeResult> kvsF =
						    getTransaction(db, tr, options, intrans)->getRange(KeyRangeRef(tokens[1], endKey), limit);
						RangeResult kvs = wait(makeInterruptable(safeThreadFutureToFuture(kvsF)));

						printf("\nRange limited to %d keys\n", limit);
						for (auto iter = kvs.begin(); iter < kvs.end(); iter++) {
							if (tokencmp(tokens[0], "getrangekeys"))
								printf("`%s'\n", printable((*iter).key).c_str());
							else
								printf(
								    "`%s' is `%s'\n", printable((*iter).key).c_str(), printable((*iter).value).c_str());
						}
						printf("\n");
					}
					continue;
				}

				if (tokencmp(tokens[0], "writemode")) {
					if (tokens.size() != 2) {
						printUsage(tokens[0]);
						is_error = true;
					} else {
						if (tokencmp(tokens[1], "on")) {
							writeMode = true;
						} else if (tokencmp(tokens[1], "off")) {
							writeMode = false;
						} else {
							printUsage(tokens[0]);
							is_error = true;
						}
					}
					continue;
				}

				if (tokencmp(tokens[0], "set")) {
					if (!writeMode) {
						fprintf(stderr, "ERROR: writemode must be enabled to set or clear keys in the database.\n");
						is_error = true;
						continue;
					}

					if (tokens.size() != 3) {
						printUsage(tokens[0]);
						is_error = true;
					} else {
						getTransaction(db, tr, options, intrans);
						tr->set(tokens[1], tokens[2]);

						if (!intrans) {
							wait(commitTransaction(tr));
						}
					}
					continue;
				}

				if (tokencmp(tokens[0], "clear")) {
					if (!writeMode) {
						fprintf(stderr, "ERROR: writemode must be enabled to set or clear keys in the database.\n");
						is_error = true;
						continue;
					}

					if (tokens.size() != 2) {
						printUsage(tokens[0]);
						is_error = true;
					} else {
						getTransaction(db, tr, options, intrans);
						tr->clear(tokens[1]);

						if (!intrans) {
							wait(commitTransaction(tr));
						}
					}
					continue;
				}

				if (tokencmp(tokens[0], "clearrange")) {
					if (!writeMode) {
						fprintf(stderr, "ERROR: writemode must be enabled to set or clear keys in the database.\n");
						is_error = true;
						continue;
					}

					if (tokens.size() != 3) {
						printUsage(tokens[0]);
						is_error = true;
					} else {
						getTransaction(db, tr, options, intrans);
						tr->clear(KeyRangeRef(tokens[1], tokens[2]));

						if (!intrans) {
							wait(commitTransaction(tr));
						}
					}
					continue;
				}

				if (tokencmp(tokens[0], "datadistribution")) {
					bool _result = wait(makeInterruptable(dataDistributionCommandActor(db, tokens)));
					if (!_result)
						is_error = true;
					continue;
				}

				if (tokencmp(tokens[0], "option")) {
					if (tokens.size() == 2 || tokens.size() > 4) {
						printUsage(tokens[0]);
						is_error = true;
					} else {
						if (tokens.size() == 1) {
							if (options->hasAnyOptionsEnabled()) {
								printf("\nCurrently enabled options:\n\n");
								options->print();
								printf("\n");
							} else
								fprintf(stderr, "There are no options enabled\n");

							continue;
						}
						bool isOn;
						if (tokencmp(tokens[1], "on")) {
							isOn = true;
						} else if (tokencmp(tokens[1], "off")) {
							if (intrans) {
								fprintf(
								    stderr,
								    "ERROR: Cannot turn option off when using a transaction created with `begin'\n");
								is_error = true;
								continue;
							}
							if (tokens.size() > 3) {
								fprintf(stderr, "ERROR: Cannot specify option argument when turning option off\n");
								is_error = true;
								continue;
							}

							isOn = false;
						} else {
							fprintf(stderr,
							        "ERROR: Invalid option state `%s': option must be turned `on' or `off'\n",
							        formatStringRef(tokens[1]).c_str());
							is_error = true;
							continue;
						}

						Optional<StringRef> arg = (tokens.size() > 3) ? tokens[3] : Optional<StringRef>();

						try {
							options->setOption(tr, tokens[2], isOn, arg, intrans);
							printf("Option %s for %s\n",
							       isOn ? "enabled" : "disabled",
							       intrans ? "current transaction" : "all transactions");
						} catch (Error& e) {
							// options->setOption() prints error message
							TraceEvent(SevWarn, "CLISetOptionError").error(e).detail("Option", tokens[2]);
							is_error = true;
						}
					}

					continue;
				}

				if (tokencmp(tokens[0], "throttle")) {
					bool _result = wait(throttleCommandActor(db, tokens));
					if (!_result)
						is_error = true;
					continue;
				}

				if (tokencmp(tokens[0], "cache_range")) {
					bool _result = wait(makeInterruptable(cacheRangeCommandActor(db, tokens)));
					if (!_result)
						is_error = true;
					continue;
				}

				fprintf(stderr, "ERROR: Unknown command `%s'. Try `help'?\n", formatStringRef(tokens[0]).c_str());
				is_error = true;
			}

			TraceEvent(SevInfo, "CLICommandLog", randomID).detail("Command", line).detail("IsError", is_error);

		} catch (Error& e) {
			if (e.code() != error_code_actor_cancelled)
				fprintf(stderr, "ERROR: %s (%d)\n", e.what(), e.code());
			is_error = true;
			if (intrans) {
				printf("Rolling back current transaction\n");
				intrans = false;
				options = &globalOptions;
				options->apply(tr);
			}
		}

		if (opt.exec.present()) {
			return is_error ? 1 : 0;
		}
	}
}

ACTOR Future<int> runCli(CLIOptions opt) {
	state LineNoise linenoise(
	    [](std::string const& line, std::vector<std::string>& completions) { fdbcliCompCmd(line, completions); },
	    [enabled = opt.cliHints](std::string const& line) -> LineNoise::Hint {
		    if (!enabled) {
			    return LineNoise::Hint();
		    }

		    bool error = false;
		    bool partial = false;
		    std::string linecopy = line;
		    std::vector<std::vector<StringRef>> parsed = parseLine(linecopy, error, partial);
		    if (parsed.size() == 0 || parsed.back().size() == 0)
			    return LineNoise::Hint();
		    StringRef command = parsed.back().front();
		    int finishedParameters = parsed.back().size() + error;

		    // As a user is typing an escaped character, e.g. \", after the \ and before the " is typed
		    // the string will be a parse error.  Ignore this parse error to avoid flipping the hint to
		    // {malformed escape sequence} and back to the original hint for the span of one character
		    // being entered.
		    if (error && line.back() != '\\')
			    return LineNoise::Hint(std::string(" {malformed escape sequence}"), 90, false);

		    bool inArgument = *(line.end() - 1) != ' ';
		    std::string hintLine = inArgument ? " " : "";
		    if (tokencmp(command, "throttle")) {
			    std::vector<const char*> hintItems = throttleHintGenerator(parsed.back(), inArgument);
			    if (hintItems.empty()) {
				    return LineNoise::Hint();
			    }
			    for (auto item : hintItems) {
				    hintLine = hintLine + item + " ";
			    }
		    } else {
			    auto iter = helpMap.find(command.toString());
			    if (iter != helpMap.end()) {
				    std::string helpLine = iter->second.usage;
				    std::vector<std::vector<StringRef>> parsedHelp = parseLine(helpLine, error, partial);
				    for (int i = finishedParameters; i < parsedHelp.back().size(); i++) {
					    hintLine = hintLine + parsedHelp.back()[i].toString() + " ";
				    }
			    } else {
				    return LineNoise::Hint();
			    }
		    }

		    return LineNoise::Hint(hintLine, 90, false);
	    },
	    1000,
	    false);

	state std::string historyFilename;
	try {
		historyFilename = joinPath(getUserHomeDirectory(), ".fdbcli_history");
		linenoise.historyLoad(historyFilename);
	} catch (Error& e) {
		TraceEvent(SevWarnAlways, "ErrorLoadingCliHistory")
		    .error(e)
		    .detail("Filename", historyFilename.empty() ? "<unknown>" : historyFilename)
		    .GetLastError();
	}

	state int result = wait(cli(opt, &linenoise));

	if (!historyFilename.empty()) {
		try {
			linenoise.historySave(historyFilename);
		} catch (Error& e) {
			TraceEvent(SevWarnAlways, "ErrorSavingCliHistory")
			    .error(e)
			    .detail("Filename", historyFilename)
			    .GetLastError();
		}
	}

	return result;
}

ACTOR Future<Void> timeExit(double duration) {
	wait(delay(duration));
	fprintf(stderr, "Specified timeout reached -- exiting...\n");
	return Void();
}

int main(int argc, char** argv) {
	platformInit();
	Error::init();
	std::set_new_handler(&platform::outOfMemory);
	uint64_t memLimit = 8LL << 30;
	setMemoryQuota(memLimit);

	registerCrashHandler();

	IKnobCollection::setGlobalKnobCollection(IKnobCollection::Type::CLIENT, Randomize::False, IsSimulated::False);

#ifdef __unixish__
	struct sigaction act;

	// We don't want ctrl-c to quit
	sigemptyset(&act.sa_mask);
	act.sa_flags = 0;
	act.sa_handler = SIG_IGN;
	sigaction(SIGINT, &act, nullptr);
#endif

	CLIOptions opt(argc, argv);
	if (opt.exit_code != -1)
		return opt.exit_code;

	if (opt.trace) {
		if (opt.traceDir.empty())
			setNetworkOption(FDBNetworkOptions::TRACE_ENABLE);
		else
			setNetworkOption(FDBNetworkOptions::TRACE_ENABLE, StringRef(opt.traceDir));

		if (!opt.traceFormat.empty()) {
			setNetworkOption(FDBNetworkOptions::TRACE_FORMAT, StringRef(opt.traceFormat));
		}
		setNetworkOption(FDBNetworkOptions::ENABLE_SLOW_TASK_PROFILING);
	}
	initHelp();

	// deferred TLS options
	if (opt.tlsCertPath.size()) {
		try {
			setNetworkOption(FDBNetworkOptions::TLS_CERT_PATH, opt.tlsCertPath);
		} catch (Error& e) {
			fprintf(stderr, "ERROR: cannot set TLS certificate path to `%s' (%s)\n", opt.tlsCertPath.c_str(), e.what());
			return 1;
		}
	}

	if (opt.tlsCAPath.size()) {
		try {
			setNetworkOption(FDBNetworkOptions::TLS_CA_PATH, opt.tlsCAPath);
		} catch (Error& e) {
			fprintf(stderr, "ERROR: cannot set TLS CA path to `%s' (%s)\n", opt.tlsCAPath.c_str(), e.what());
			return 1;
		}
	}
	if (opt.tlsKeyPath.size()) {
		try {
			if (opt.tlsPassword.size())
				setNetworkOption(FDBNetworkOptions::TLS_PASSWORD, opt.tlsPassword);

			setNetworkOption(FDBNetworkOptions::TLS_KEY_PATH, opt.tlsKeyPath);
		} catch (Error& e) {
			fprintf(stderr, "ERROR: cannot set TLS key path to `%s' (%s)\n", opt.tlsKeyPath.c_str(), e.what());
			return 1;
		}
	}
	if (opt.tlsVerifyPeers.size()) {
		try {
			setNetworkOption(FDBNetworkOptions::TLS_VERIFY_PEERS, opt.tlsVerifyPeers);
		} catch (Error& e) {
			fprintf(
			    stderr, "ERROR: cannot set TLS peer verification to `%s' (%s)\n", opt.tlsVerifyPeers.c_str(), e.what());
			return 1;
		}
	}

	try {
		setNetworkOption(FDBNetworkOptions::DISABLE_CLIENT_STATISTICS_LOGGING);
	} catch (Error& e) {
		fprintf(stderr, "ERROR: cannot disable logging client related information (%s)\n", e.what());
		return 1;
	}

	if (opt.debugTLS) {
#ifndef TLS_DISABLED
		// Backdoor into NativeAPI's tlsConfig, which is where the above network option settings ended up.
		extern TLSConfig tlsConfig;
		printf("TLS Configuration:\n");
		printf("\tCertificate Path: %s\n", tlsConfig.getCertificatePathSync().c_str());
		printf("\tKey Path: %s\n", tlsConfig.getKeyPathSync().c_str());
		printf("\tCA Path: %s\n", tlsConfig.getCAPathSync().c_str());
		try {
			LoadedTLSConfig loaded = tlsConfig.loadSync();
			printf("\tPassword: %s\n", loaded.getPassword().empty() ? "Not configured" : "Exists, but redacted");
			printf("\n");
			loaded.print(stdout);
		} catch (Error& e) {
			fprintf(stderr, "ERROR: %s (%d)\n", e.what(), e.code());
			printf("Use --log and look at the trace logs for more detailed information on the failure.\n");
			return 1;
		}
#else
		printf("This fdbcli was built with TLS disabled.\n");
#endif
		return 0;
	}

	try {
		// Note: refactoring fdbcli, in progress
		API->selectApiVersion(FDB_API_VERSION);
		API->setupNetwork();
		Future<int> cliFuture = runCli(opt);
		Future<Void> timeoutFuture = opt.exit_timeout ? timeExit(opt.exit_timeout) : Never();
		auto f = stopNetworkAfter(success(cliFuture) || timeoutFuture);
		API->runNetwork();

		if (cliFuture.isReady()) {
			return cliFuture.get();
		} else {
			return 1;
		}
	} catch (Error& e) {
		fprintf(stderr, "ERROR: %s (%d)\n", e.what(), e.code());
		return 1;
	}
}
