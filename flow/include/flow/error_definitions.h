/*
 * error_definitions.h
 *
 * This source file is part of the FoundationDB open source project
 *
 * Copyright 2013-2022 Apple Inc. and the FoundationDB project authors
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

#ifdef ERROR

// SOMEDAY: Split this into flow, fdbclient, fdbserver error headers?

// Error codes defined here are primarily for programmatic use, not debugging: a separate
// error should be defined if and only if there is a sensible situation in which code could
// catch and react specifically to that error.  So for example there is only one
// internal_error code even though there are a huge number of internal errors; extra
// information is logged in the trace file.

// 1xxx Normal failure (plausibly these should not even be "errors", but they are failures of
//   the way operations are currently defined)
// clang-format off
ERROR( success, 0, "Success" )
ERROR( end_of_stream, 1, "End of stream" )
ERROR( operation_failed, 1000, "Operation failed")
ERROR( wrong_shard_server, 1001, "Shard is not available from this server")
ERROR( operation_obsolete, 1002, "Operation result no longer necessary")
ERROR( cold_cache_server, 1003, "Cache server is not warm for this range")
ERROR( timed_out, 1004, "Operation timed out" )
ERROR( coordinated_state_conflict, 1005, "Conflict occurred while changing coordination information" )
ERROR( all_alternatives_failed, 1006, "All alternatives failed" )
ERROR( transaction_too_old, 1007, "Transaction is too old to perform reads or be committed" )
ERROR( no_more_servers, 1008, "Not enough physical servers available" )
ERROR( future_version, 1009, "Request for future version" )
ERROR( movekeys_conflict, 1010, "Conflicting attempts to change data distribution" )
ERROR( tlog_stopped, 1011, "TLog stopped" )
ERROR( server_request_queue_full, 1012, "Server request queue is full" )
ERROR( not_committed, 1020, "Transaction not committed due to conflict with another transaction" )
ERROR( commit_unknown_result, 1021, "Transaction may or may not have committed" )
ERROR( commit_unknown_result_fatal, 1022, "Idempotency id for transaction may have expired, so the commit status of the transaction cannot be determined" )
ERROR( transaction_cancelled, 1025, "Operation aborted because the transaction was cancelled" )
ERROR( connection_failed, 1026, "Network connection failed" )
ERROR( coordinators_changed, 1027, "Coordination servers have changed" )
ERROR( new_coordinators_timed_out, 1028, "New coordination servers did not respond in a timely way" )
ERROR( watch_cancelled, 1029, "Watch cancelled because storage server watch limit exceeded" )
ERROR( request_maybe_delivered, 1030, "Request may or may not have been delivered" )
ERROR( transaction_timed_out, 1031, "Operation aborted because the transaction timed out" )
ERROR( too_many_watches, 1032, "Too many watches currently set" )
ERROR( locality_information_unavailable, 1033, "Locality information not available" )
ERROR( watches_disabled, 1034, "Watches cannot be set if read your writes is disabled" )
ERROR( default_error_or, 1035, "Default error for an ErrorOr object" )
ERROR( accessed_unreadable, 1036, "Read or wrote an unreadable key" )
ERROR( process_behind, 1037, "Storage process does not have recent mutations" )
ERROR( database_locked, 1038, "Database is locked" )
ERROR( cluster_version_changed, 1039, "The protocol version of the cluster has changed" )
ERROR( external_client_already_loaded, 1040, "External client has already been loaded" )
ERROR( lookup_failed, 1041, "DNS lookup failed" )
ERROR( commit_proxy_memory_limit_exceeded, 1042, "CommitProxy commit memory limit exceeded" )
ERROR( shutdown_in_progress, 1043, "Operation no longer supported due to shutdown" )
ERROR( serialization_failed, 1044, "Failed to deserialize an object" )
ERROR( connection_unreferenced, 1048, "No peer references for connection" )
ERROR( connection_idle, 1049, "Connection closed after idle timeout" )
ERROR( disk_adapter_reset, 1050, "The disk queue adpater reset" )
ERROR( batch_transaction_throttled, 1051, "Batch GRV request rate limit exceeded")
ERROR( dd_cancelled, 1052, "Data distribution components cancelled")
ERROR( dd_not_found, 1053, "Data distributor not found")
ERROR( wrong_connection_file, 1054, "Connection file mismatch")
ERROR( version_already_compacted, 1055, "The requested changes have been compacted away")
ERROR( local_config_changed, 1056, "Local configuration file has changed. Restart and apply these changes" )
ERROR( failed_to_reach_quorum, 1057, "Failed to reach quorum from configuration database nodes. Retry sending these requests" )
ERROR( unsupported_format_version, 1058, "Format version not supported" )
ERROR( unknown_change_feed, 1059, "Change feed not found" )
ERROR( change_feed_not_registered, 1060, "Change feed not registered" )
ERROR( granule_assignment_conflict, 1061, "Conflicting attempts to assign blob granules" )
ERROR( change_feed_cancelled, 1062, "Change feed was cancelled" )
ERROR( blob_granule_file_load_error, 1063, "Error loading a blob file during granule materialization" )
ERROR( blob_granule_transaction_too_old, 1064, "Read version is older than blob granule history supports" )
ERROR( blob_manager_replaced, 1065, "This blob manager has been replaced." )
ERROR( change_feed_popped, 1066, "Tried to read a version older than what has been popped from the change feed" )
ERROR( remote_kvs_cancelled, 1067, "The remote key-value store is cancelled" )
ERROR( page_header_wrong_page_id, 1068, "Page header does not match location on disk" )
ERROR( page_header_checksum_failed, 1069, "Page header checksum failed" )
ERROR( page_header_version_not_supported, 1070, "Page header version is not supported" )
ERROR( page_encoding_not_supported, 1071, "Page encoding type is not supported or not valid" )
ERROR( page_decoding_failed, 1072, "Page content decoding failed" )
ERROR( unexpected_encoding_type, 1073, "Page content decoding failed" )
ERROR( encryption_key_not_found, 1074, "Encryption key not found" )
ERROR( data_move_cancelled, 1075, "Data move was cancelled" )
ERROR( data_move_dest_team_not_found, 1076, "Dest team was not found for data move" )
ERROR( blob_worker_full, 1077, "Blob worker cannot take on more granule assignments" )
ERROR( grv_proxy_memory_limit_exceeded, 1078, "GetReadVersion proxy memory limit exceeded" )
ERROR( blob_granule_request_failed, 1079, "BlobGranule request failed" )
ERROR( storage_too_many_feed_streams, 1080, "Too many feed streams to a single storage server" )

ERROR( broken_promise, 1100, "Broken promise" )
ERROR( operation_cancelled, 1101, "Asynchronous operation cancelled" )
ERROR( future_released, 1102, "Future has been released" )
ERROR( connection_leaked, 1103, "Connection object leaked" )
ERROR( never_reply, 1104, "Never reply to the request" )

ERROR( recruitment_failed, 1200, "Recruitment of a server failed" )   // Be careful, catching this will delete the data of a storage server or tlog permanently
ERROR( move_to_removed_server, 1201, "Attempt to move keys to a storage server that was removed" )
ERROR( worker_removed, 1202, "Normal worker shut down" )   // Be careful, catching this will delete the data of a storage server or tlog permanently
ERROR( cluster_recovery_failed, 1203, "Cluster recovery failed")
ERROR( master_max_versions_in_flight, 1204, "Master hit maximum number of versions in flight" )
ERROR( tlog_failed, 1205, "Cluster recovery terminating because a TLog failed" )   // similar to tlog_stopped, but the tlog has actually died
ERROR( worker_recovery_failed, 1206, "Recovery of a worker process failed" )
ERROR( please_reboot, 1207, "Reboot of server process requested" )
ERROR( please_reboot_delete, 1208, "Reboot of server process requested, with deletion of state" )
ERROR( commit_proxy_failed, 1209, "Master terminating because a CommitProxy failed" )
ERROR( resolver_failed, 1210, "Cluster recovery terminating because a Resolver failed" )
ERROR( server_overloaded, 1211, "Server is under too much load and cannot respond" )
ERROR( backup_worker_failed, 1212, "Cluster recovery terminating because a backup worker failed")
ERROR( tag_throttled, 1213, "Transaction tag is being throttled" )
ERROR( grv_proxy_failed, 1214, "Cluster recovery terminating because a GRVProxy failed" )
ERROR( dd_tracker_cancelled, 1215, "The data distribution tracker has been cancelled" )
ERROR( failed_to_progress, 1216, "Process has failed to make sufficient progress" )
ERROR( invalid_cluster_id, 1217, "Attempted to join cluster with a different cluster ID" )
ERROR( restart_cluster_controller, 1218, "Restart cluster controller process" )
ERROR( please_reboot_kv_store, 1219, "Need to reboot the storage engine")
ERROR( incompatible_software_version, 1220, "Current software does not support database format" )
ERROR( audit_storage_failed, 1221, "Validate storage consistency operation failed" )
ERROR( audit_storage_exceeded_request_limit, 1222, "Exceeded the max number of allowed concurrent audit storage requests" )
ERROR( proxy_tag_throttled, 1223, "Exceeded maximum proxy tag throttling duration" )
ERROR( key_value_store_deadline_exceeded, 1224, "Exceeded maximum time allowed to read or write.")
ERROR( storage_quota_exceeded, 1225, "Exceeded the maximum storage quota allocated to the tenant.")
ERROR( version_indexer_failed, 1226, "Master terminating because version indexer failed");

// 15xx Platform errors
ERROR( platform_error, 1500, "Platform error" )
ERROR( large_alloc_failed, 1501, "Large block allocation failed" )
ERROR( performance_counter_error, 1502, "QueryPerformanceCounter error" )

ERROR( io_error, 1510, "Disk i/o operation failed" )
ERROR( file_not_found, 1511, "File not found" )
ERROR( bind_failed, 1512, "Unable to bind to network" )
ERROR( file_not_readable, 1513, "File could not be read" )
ERROR( file_not_writable, 1514, "File could not be written" )
ERROR( no_cluster_file_found, 1515, "No cluster file found in current directory or default location" )
ERROR( file_too_large, 1516, "File too large to be read" )
ERROR( non_sequential_op, 1517, "Non sequential file operation not allowed" )
ERROR( http_bad_response, 1518, "HTTP response was badly formed" )
ERROR( http_not_accepted, 1519, "HTTP request not accepted" )
ERROR( checksum_failed, 1520, "A data checksum failed" )
ERROR( io_timeout, 1521, "A disk IO operation failed to complete in a timely manner" )
ERROR( file_corrupt, 1522, "A structurally corrupt data file was detected" )
ERROR( http_request_failed, 1523, "HTTP response code not received or indicated failure" )
ERROR( http_auth_failed, 1524, "HTTP request failed due to bad credentials" )
ERROR( http_bad_request_id, 1525, "HTTP response contained an unexpected X-Request-ID header" )
ERROR( rest_invalid_uri, 1526, "Invalid REST URI")
ERROR( rest_invalid_rest_client_knob, 1527, "Invalid RESTClient knob")
ERROR( rest_connectpool_key_not_found, 1528, "ConnectKey not found in connection pool")
ERROR( lock_file_failure, 1529, "Unable to lock the file")


// 2xxx Attempt (presumably by a _client_) to do something illegal.  If an error is known to
// be internally caused, it should be 41xx
ERROR( client_invalid_operation, 2000, "Invalid API call" )
ERROR( commit_read_incomplete, 2002, "Commit with incomplete read" )
ERROR( test_specification_invalid, 2003, "Invalid test specification" )
ERROR( key_outside_legal_range, 2004, "Key outside legal range" )
ERROR( inverted_range, 2005, "Range begin key larger than end key" )
ERROR( invalid_option_value, 2006, "Option set with an invalid value" )
ERROR( invalid_option, 2007, "Option not valid in this context" )
ERROR( network_not_setup, 2008, "Action not possible before the network is configured" )
ERROR( network_already_setup, 2009, "Network can be configured only once" )
ERROR( read_version_already_set, 2010, "Transaction already has a read version set" )
ERROR( version_invalid, 2011, "Version not valid" )
ERROR( range_limits_invalid, 2012, "Range limits not valid" )
ERROR( invalid_database_name, 2013, "Database name must be 'DB'" )
ERROR( attribute_not_found, 2014, "Attribute not found" )
ERROR( future_not_set, 2015, "Future not ready" )
ERROR( future_not_error, 2016, "Future not an error" )
ERROR( used_during_commit, 2017, "Operation issued while a commit was outstanding" )
ERROR( invalid_mutation_type, 2018, "Unrecognized atomic mutation type" )
ERROR( attribute_too_large, 2019, "Attribute too large for type int" )
ERROR( transaction_invalid_version, 2020, "Transaction does not have a valid commit version" )
ERROR( no_commit_version, 2021, "Transaction is read-only and therefore does not have a commit version" )
ERROR( environment_variable_network_option_failed, 2022, "Environment variable network option could not be set" )
ERROR( transaction_read_only, 2023, "Attempted to commit a transaction specified as read-only" )
ERROR( invalid_cache_eviction_policy, 2024, "Invalid cache eviction policy, only random and lru are supported" )
ERROR( network_cannot_be_restarted, 2025, "Network can only be started once" )
ERROR( blocked_from_network_thread, 2026, "Detected a deadlock in a callback called from the network thread" )
ERROR( invalid_config_db_range_read, 2027, "Invalid configuration database range read" )
ERROR( invalid_config_db_key, 2028, "Invalid configuration database key provided" )
ERROR( invalid_config_path, 2029, "Invalid configuration path" )
ERROR( mapper_bad_index, 2030, "The index in K[] or V[] is not a valid number or out of range" )
ERROR( mapper_no_such_key, 2031, "A mapped key is not set in database" )
ERROR( mapper_bad_range_decriptor, 2032, "\"{...}\" must be the last element of the mapper tuple" )
ERROR( quick_get_key_values_has_more, 2033, "One of the mapped range queries is too large" )
ERROR( quick_get_value_miss, 2034, "Found a mapped key that is not served in the same SS" )
ERROR( quick_get_key_values_miss, 2035, "Found a mapped range that is not served in the same SS" )
ERROR( blob_granule_no_ryw, 2036, "Blob Granule Read Transactions must be specified as ryw-disabled" )
ERROR( blob_granule_not_materialized, 2037, "Blob Granule Read was not materialized" )
ERROR( get_mapped_key_values_has_more, 2038, "getMappedRange does not support continuation for now" )
ERROR( get_mapped_range_reads_your_writes, 2039, "getMappedRange tries to read data that were previously written in the transaction" )
ERROR( checkpoint_not_found, 2040, "Checkpoint not found" )
ERROR( key_not_tuple, 2041, "The key cannot be parsed as a tuple" );
ERROR( value_not_tuple, 2042, "The value cannot be parsed as a tuple" );
ERROR( mapper_not_tuple, 2043, "The mapper cannot be parsed as a tuple" );
ERROR( invalid_checkpoint_format, 2044, "Invalid checkpoint format" )
ERROR( invalid_throttle_quota_value, 2045, "Invalid quota value. Note that reserved_throughput cannot exceed total_throughput" )

ERROR( incompatible_protocol_version, 2100, "Incompatible protocol version" )
ERROR( transaction_too_large, 2101, "Transaction exceeds byte limit" )
ERROR( key_too_large, 2102, "Key length exceeds limit" )
ERROR( value_too_large, 2103, "Value length exceeds limit" )
ERROR( connection_string_invalid, 2104, "Connection string invalid" )
ERROR( address_in_use, 2105, "Local address in use" )
ERROR( invalid_local_address, 2106, "Invalid local address" )
ERROR( tls_error, 2107, "TLS error" )
ERROR( unsupported_operation, 2108, "Operation is not supported" )
ERROR( too_many_tags, 2109, "Too many tags set on transaction" )
ERROR( tag_too_long, 2110, "Tag set on transaction is too long" )
ERROR( too_many_tag_throttles, 2111, "Too many tag throttles have been created" )
ERROR( special_keys_cross_module_read, 2112, "Special key space range read crosses modules. Refer to the `special_key_space_relaxed' transaction option for more details." )
ERROR( special_keys_no_module_found, 2113, "Special key space range read does not intersect a module. Refer to the `special_key_space_relaxed' transaction option for more details." )
ERROR( special_keys_write_disabled, 2114, "Special Key space is not allowed to write by default. Refer to the `special_key_space_enable_writes` transaction option for more details." )
ERROR( special_keys_no_write_module_found, 2115, "Special key space key or keyrange in set or clear does not intersect a module" )
ERROR( special_keys_cross_module_clear, 2116, "Special key space clear crosses modules" )
ERROR( special_keys_api_failure, 2117, "Api call through special keys failed. For more information, call get on special key 0xff0xff/error_message to get a json string of the error message." )
ERROR( client_lib_invalid_metadata, 2118, "Invalid client library metadata." )
ERROR( client_lib_already_exists, 2119, "Client library with same identifier already exists on the cluster." )
ERROR( client_lib_not_found, 2120, "Client library for the given identifier not found." )
ERROR( client_lib_not_available, 2121, "Client library exists, but is not available for download." )
ERROR( client_lib_invalid_binary, 2122, "Invalid client library binary." )
ERROR( no_external_client_provided, 2123, "No external client library provided." )
ERROR( all_external_clients_failed, 2124, "All external clients have failed." )

ERROR( tenant_name_required, 2130, "Tenant name must be specified to access data in the cluster" )
ERROR( tenant_not_found, 2131, "Tenant does not exist" )
ERROR( tenant_already_exists, 2132, "A tenant with the given name already exists" )
ERROR( tenant_not_empty, 2133, "Cannot delete a non-empty tenant" )
ERROR( invalid_tenant_name, 2134, "Tenant name cannot begin with \\xff" )
ERROR( tenant_prefix_allocator_conflict, 2135, "The database already has keys stored at the prefix allocated for the tenant" )
ERROR( tenants_disabled, 2136, "Tenants have been disabled in the cluster" )
ERROR( unknown_tenant, 2137, "Tenant is not available from this server" )
ERROR( illegal_tenant_access, 2138, "Illegal tenant access" )
ERROR( invalid_tenant_group_name, 2139, "Tenant group name cannot begin with \\xff" )
ERROR( invalid_tenant_configuration, 2140, "Tenant configuration is invalid" )
ERROR( cluster_no_capacity, 2141, "Cluster does not have capacity to perform the specified operation" )
ERROR( tenant_removed, 2142, "The tenant was removed" )
ERROR( invalid_tenant_state, 2143, "Operation cannot be applied to tenant in its current state" )

ERROR( invalid_cluster_name, 2160, "Data cluster name cannot begin with \\xff" )
ERROR( invalid_metacluster_operation, 2161, "Metacluster operation performed on non-metacluster" )
ERROR( cluster_already_exists, 2162, "A data cluster with the given name already exists" )
ERROR( cluster_not_found, 2163, "Data cluster does not exist" )
ERROR( cluster_not_empty, 2164, "Cluster must be empty" )
ERROR( cluster_already_registered, 2165, "Data cluster is already registered with a metacluster" )
ERROR( metacluster_no_capacity, 2166, "Metacluster does not have capacity to create new tenants" )
ERROR( management_cluster_invalid_access, 2167, "Standard transactions cannot be run against the management cluster" )
ERROR( tenant_creation_permanently_failed, 2168, "The tenant creation did not complete in a timely manner and has permanently failed" )
ERROR( cluster_removed, 2169, "The cluster is being removed from the metacluster" )

// 2200 - errors from bindings and official APIs
ERROR( api_version_unset, 2200, "API version is not set" )
ERROR( api_version_already_set, 2201, "API version may be set only once" )
ERROR( api_version_invalid, 2202, "API version not valid" )
ERROR( api_version_not_supported, 2203, "API version not supported" )
ERROR( api_function_missing, 2204, "Failed to load a required FDB API function." )
ERROR( exact_mode_without_limits, 2210, "EXACT streaming mode requires limits, but none were given" )

ERROR( invalid_tuple_data_type, 2250, "Unrecognized data type in packed tuple")
ERROR( invalid_tuple_index, 2251, "Tuple does not have element at specified index")
ERROR( key_not_in_subspace, 2252, "Cannot unpack key that is not in subspace" )
ERROR( manual_prefixes_not_enabled, 2253, "Cannot specify a prefix unless manual prefixes are enabled" )
ERROR( prefix_in_partition, 2254, "Cannot specify a prefix in a partition" )
ERROR( cannot_open_root_directory, 2255, "Root directory cannot be opened" )
ERROR( directory_already_exists, 2256, "Directory already exists" )
ERROR( directory_does_not_exist, 2257, "Directory does not exist" )
ERROR( parent_directory_does_not_exist, 2258, "Directory's parent does not exist" )
ERROR( mismatched_layer, 2259, "Directory has already been created with a different layer string" )
ERROR( invalid_directory_layer_metadata, 2260, "Invalid directory layer metadata" )
ERROR( cannot_move_directory_between_partitions, 2261, "Directory cannot be moved between partitions" )
ERROR( cannot_use_partition_as_subspace, 2262, "Directory partition cannot be used as subspace" )
ERROR( incompatible_directory_version, 2263, "Directory layer was created with an incompatible version" )
ERROR( directory_prefix_not_empty, 2264, "Database has keys stored at the prefix chosen by the automatic prefix allocator" )
ERROR( directory_prefix_in_use, 2265, "Directory layer already has a conflicting prefix" )
ERROR( invalid_destination_directory, 2266, "Target directory is invalid" )
ERROR( cannot_modify_root_directory, 2267, "Root directory cannot be modified" )
ERROR( invalid_uuid_size, 2268, "UUID is not sixteen bytes");
ERROR( invalid_versionstamp_size, 2269, "Versionstamp is not exactly twelve bytes");

// 2300 - backup and restore errors
ERROR( backup_error, 2300, "Backup error")
ERROR( restore_error, 2301, "Restore error")
ERROR( backup_duplicate, 2311, "Backup duplicate request")
ERROR( backup_unneeded, 2312, "Backup unneeded request")
ERROR( backup_bad_block_size, 2313, "Backup file block size too small")
ERROR( backup_invalid_url, 2314, "Backup Container URL invalid")
ERROR( backup_invalid_info, 2315, "Backup Container info invalid")
ERROR( backup_cannot_expire, 2316, "Cannot expire requested data from backup without violating minimum restorability")
ERROR( backup_auth_missing, 2317, "Cannot find authentication details (such as a password or secret key) for the specified Backup Container URL")
ERROR( backup_auth_unreadable, 2318, "Cannot read or parse one or more sources of authentication information for Backup Container URLs")
ERROR( backup_does_not_exist, 2319, "Backup does not exist")
ERROR( backup_not_filterable_with_key_ranges, 2320, "Backup before 6.3 cannot be filtered with key ranges")
ERROR( backup_not_overlapped_with_keys_filter, 2321, "Backup key ranges doesn't overlap with key ranges filter")
ERROR( restore_invalid_version, 2361, "Invalid restore version")
ERROR( restore_corrupted_data, 2362, "Corrupted backup data")
ERROR( restore_missing_data, 2363, "Missing backup data")
ERROR( restore_duplicate_tag, 2364, "Restore duplicate request")
ERROR( restore_unknown_tag, 2365, "Restore tag does not exist")
ERROR( restore_unknown_file_type, 2366, "Unknown backup/restore file type")
ERROR( restore_unsupported_file_version, 2367, "Unsupported backup file version")
ERROR( restore_bad_read, 2368, "Unexpected number of bytes read")
ERROR( restore_corrupted_data_padding, 2369, "Backup file has unexpected padding bytes")
ERROR( restore_destination_not_empty, 2370, "Attempted to restore into a non-empty destination database")
ERROR( restore_duplicate_uid, 2371, "Attempted to restore using a UID that had been used for an aborted restore")
ERROR( task_invalid_version, 2381, "Invalid task version")
ERROR( task_interrupted, 2382, "Task execution stopped due to timeout, abort, or completion by another worker")
ERROR( invalid_encryption_key_file, 2383, "The provided encryption key file has invalid contents" )

ERROR( key_not_found, 2400, "Expected key is missing")
ERROR( json_malformed, 2401, "JSON string was malformed")
ERROR( json_eof_expected, 2402, "JSON string did not terminate where expected")

// 2500 - disk snapshot based backup errors
ERROR( snap_disable_tlog_pop_failed,  2500, "Failed to disable tlog pops" )
ERROR( snap_storage_failed,  2501, "Failed to snapshot storage nodes" )
ERROR( snap_tlog_failed,  2502, "Failed to snapshot TLog nodes" )
ERROR( snap_coord_failed,  2503, "Failed to snapshot coordinator nodes" )
ERROR( snap_enable_tlog_pop_failed,  2504, "Failed to enable tlog pops" )
ERROR( snap_path_not_whitelisted, 2505, "Snapshot create binary path not whitelisted" )
ERROR( snap_not_fully_recovered_unsupported, 2506, "Unsupported when the cluster is not fully recovered" )
ERROR( snap_log_anti_quorum_unsupported, 2507, "Unsupported when log anti quorum is configured" )
ERROR( snap_with_recovery_unsupported, 2508, "Cluster recovery during snapshot operation not supported" )
ERROR( snap_invalid_uid_string, 2509, "The given uid string is not a 32-length hex string" )

// 27XX - Encryption operations errors
ERROR( encrypt_ops_error, 2700, "Encryption operation error" )
ERROR( encrypt_header_metadata_mismatch, 2701, "Encryption header metadata mismatch" )
ERROR( encrypt_key_not_found, 2702, "Expected encryption key is missing" )
ERROR( encrypt_key_ttl_expired, 2703, "Expected encryption key TTL has expired" )
ERROR( encrypt_header_authtoken_mismatch, 2704, "Encryption header authentication token mismatch" )
ERROR( encrypt_update_cipher, 2705, "Attempt to update encryption cipher key" )
ERROR( encrypt_invalid_id, 2706, "Invalid encryption cipher details" )
ERROR( encrypt_keys_fetch_failed, 2707, "Encryption keys fetch from external KMS failed" )
ERROR( encrypt_invalid_kms_config, 2708, "Invalid encryption/kms configuration: discovery-url, validation-token, endpoint etc." )
ERROR( encrypt_unsupported, 2709, "Encryption not supported" )

// 4xxx Internal errors (those that should be generated only by bugs) are decimal 4xxx
ERROR( unknown_error, 4000, "An unknown error occurred" )  // C++ exception not of type Error
ERROR( internal_error, 4100, "An internal error occurred" )
ERROR( not_implemented, 4200, "Not implemented yet" )

// 6xxx Authorization and authentication error codes
ERROR( permission_denied, 6000, "Client tried to access unauthorized data" )
ERROR( unauthorized_attempt, 6001, "A untrusted client tried to send a message to a private endpoint" )
ERROR( digital_signature_ops_error, 6002, "Digital signature operation error" )
ERROR( authorization_token_verify_failed, 6003, "Failed to verify authorization token" )
ERROR( pkey_decode_error, 6004, "Failed to decode public/private key" )
ERROR( pkey_encode_error, 6005, "Failed to encode public/private key" )
// clang-format on

#undef ERROR
#endif
