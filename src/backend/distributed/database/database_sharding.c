/*-------------------------------------------------------------------------
 *
 * database_sharding.c
 *
 * This file contains module-level definitions.
 *
 * Copyright (c) 2023, Microsoft, Inc.
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"
#include "fmgr.h"
#include "miscadmin.h"

#include "citus_version.h"
#include "pg_version_compat.h"

#include "access/genam.h"
#include "commands/dbcommands.h"
#include "distributed/connection_management.h"
#include "distributed/database/database_sharding.h"
#include "distributed/deparser.h"
#include "distributed/deparse_shard_query.h"
#include "distributed/listutils.h"
#include "distributed/metadata_sync.h"
#include "distributed/pooler/pgbouncer_manager.h"
#include "distributed/remote_commands.h"
#include "distributed/shared_library_init.h"
#include "distributed/worker_transaction.h"
#include "executor/spi.h"
#include "nodes/makefuncs.h"
#include "nodes/parsenodes.h"
#include "postmaster/postmaster.h"
#include "tcop/utility.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"


static void ExecuteCommandInControlDatabase(char *command);
static void AllowConnectionsOnlyOnNodeGroup(Oid databaseOid, Oid nodeGroupId);
static void InsertDatabaseShardAssignment(Oid databaseOid, int nodeGroupId);
static void InsertDatabaseShardAssignmentLocally(Oid databaseOid, int nodeGroupId);
static void InsertDatabaseShardAssignmentOnOtherNodes(Oid databaseOid, int nodeGroupId);
static void DeleteDatabaseShardByDatabaseId(Oid databaseOid);
static void DeleteDatabaseShardByDatabaseIdOnOtherNodes(Oid databaseOid);
static DatabaseShard * TupleToDatabaseShard(HeapTuple heapTuple,
											TupleDesc tupleDescriptor);
static char * InsertDatabaseShardAssignmentCommand(Oid databaseOid, int nodeGroupId);
static char * DeleteDatabaseShardByDatabaseIdCommand(Oid databaseOid);


PG_FUNCTION_INFO_V1(database_shard_assign);
PG_FUNCTION_INFO_V1(citus_internal_add_database_shard);
PG_FUNCTION_INFO_V1(citus_internal_delete_database_shard);


/* citus.enable_database_sharding setting */
bool EnableDatabaseSharding = false;

/* citus.database_sharding_pgbouncer_file setting */
char *DatabaseShardingPgBouncerFile = "";


/*
 * PreProcessUtilityInDatabaseShard handles DDL commands that occur within a
 * database shard and require global coordination:
 * - CREATE/ALTER/DROP DATABASE
 * - CREATE/ALTER/DROP ROLE/USER/GROUP
 */
void
PreProcessUtilityInDatabaseShard(Node *parseTree, const char *queryString,
								 ProcessUtilityContext context,
								 bool *runPreviousUtilityHook)
{
	if (!EnableDatabaseSharding || context != PROCESS_UTILITY_TOPLEVEL)
	{
		return;
	}

	if (EnableCreateDatabasePropagation)
	{
		if (IsA(parseTree, CreatedbStmt))
		{
			char *command = DeparseCreatedbStmt(parseTree);
			ExecuteCommandInControlDatabase(command);

			/* command is fully delegated to control database */
			*runPreviousUtilityHook = false;
		}
		else if (IsA(parseTree, DropdbStmt))
		{
			char *command = DeparseDropdbStmt(parseTree);
			ExecuteCommandInControlDatabase(command);

			/* command is fully delegated to control database */
			*runPreviousUtilityHook = false;
		}
	}
}


/*
 * PostProcessUtilityInDatabaseShard is currently a noop.
 */
void
PostProcessUtilityInDatabaseShard(Node *parseTree, const char *queryString,
								  ProcessUtilityContext context)
{
	if (!EnableDatabaseSharding || context != PROCESS_UTILITY_TOPLEVEL)
	{
		return;
	}
}


/*
 * ExecuteCommandInControlDatabase connects to localhost to execute a command
 * in the main Citus database.
 */
static void
ExecuteCommandInControlDatabase(char *command)
{
	int connectionFlag = FORCE_NEW_CONNECTION;

	MultiConnection *connection =
		GetNodeUserDatabaseConnection(connectionFlag, LocalHostName, PostPortNumber,
									  NULL, CitusMainDatabase);

	ExecuteCriticalRemoteCommand(connection,
								 "SET application_name TO 'citus_database_shard'");
	ExecuteCriticalRemoteCommand(connection, command);
	CloseConnection(connection);
}


/*
 * database_shard_assign assigns an existing database to a node.
 */
Datum
database_shard_assign(PG_FUNCTION_ARGS)
{
	CheckCitusVersion(ERROR);

	char *databaseName = text_to_cstring(PG_GETARG_TEXT_P(0));

	bool missingOk = false;
	Oid databaseOid = get_database_oid(databaseName, missingOk);

	if (!pg_database_ownercheck(databaseOid, GetUserId()))
	{
		ereport(ERROR, (errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
						errmsg("permission denied to assign database \"%s\" "
							   "to a shard",
							   databaseName)));
	}

	if (GetDatabaseShardByOid(databaseOid) != NULL)
	{
		ereport(ERROR, (errmsg("database is already assigned to a shard")));
	}

	AssignDatabaseToShard(databaseOid);

	PG_RETURN_VOID();
}


/*
 * AssignDatabaseToShard finds a suitable node for the given
 * database and assigns it.
 */
void
AssignDatabaseToShard(Oid databaseOid)
{
	int nodeGroupId = GetLocalGroupId();

	List *workerNodes = TargetWorkerSetNodeList(ALL_SHARD_NODES, RowShareLock);
	if (list_length(workerNodes) > 0)
	{
		/* TODO: actually look for available space */
		int workerNodeIndex = databaseOid % list_length(workerNodes);
		WorkerNode *workerNode = list_nth(workerNodes, workerNodeIndex);
		nodeGroupId = workerNode->groupId;
	}

	InsertDatabaseShardAssignment(databaseOid, nodeGroupId);
	AllowConnectionsOnlyOnNodeGroup(databaseOid, nodeGroupId);

	ReconfigurePgBouncersOnCommit = true;
}


/*
 * AllowConnectionsOnlyOnNodeGroup sets the ALLOW_CONNECTIONS properties on
 * the database to false, except on nodeGroupId.
 */
static void
AllowConnectionsOnlyOnNodeGroup(Oid databaseOid, Oid nodeGroupId)
{
	StringInfo command = makeStringInfo();
	char *databaseName = get_database_name(databaseOid);

	List *workerNodes = TargetWorkerSetNodeList(ALL_SHARD_NODES, RowShareLock);
	WorkerNode *workerNode = NULL;

	foreach_ptr(workerNode, workerNodes)
	{
		resetStringInfo(command);

		if (workerNode->groupId == nodeGroupId)
		{
			appendStringInfo(command, "GRANT CONNECT ON DATABASE %s TO public",
							 quote_identifier(databaseName));
		}
		else
		{
			appendStringInfo(command, "REVOKE CONNECT ON DATABASE %s FROM public",
							 quote_identifier(databaseName));
		}

		if (workerNode->groupId == GetLocalGroupId())
		{
			ExecuteQueryViaSPI(command->data, SPI_OK_UTILITY);
		}
		else
		{
			SendCommandToWorker(workerNode->workerName, workerNode->workerPort,
								command->data);
		}
	}
}


/*
 * InsertDatabaseShardAssignment inserts a record into the local
 * citus_catalog.database_sharding table.
 */
static void
InsertDatabaseShardAssignment(Oid databaseOid, int nodeGroupId)
{
	InsertDatabaseShardAssignmentLocally(databaseOid, nodeGroupId);

	if (EnableMetadataSync)
	{
		InsertDatabaseShardAssignmentOnOtherNodes(databaseOid, nodeGroupId);
	}
}


/*
 * InsertDatabaseShardAssignmentLocally inserts a record into the local
 * citus_catalog.database_sharding table.
 */
static void
InsertDatabaseShardAssignmentLocally(Oid databaseOid, int nodeGroupId)
{
	Datum values[Natts_database_shard];
	bool isNulls[Natts_database_shard];

	/* form new shard tuple */
	memset(values, 0, sizeof(values));
	memset(isNulls, false, sizeof(isNulls));

	values[Anum_database_shard_database_id - 1] = ObjectIdGetDatum(databaseOid);
	values[Anum_database_shard_node_group_id - 1] = Int32GetDatum(nodeGroupId);
	values[Anum_database_shard_is_available - 1] = BoolGetDatum(true);

	/* open shard relation and insert new tuple */
	Relation databaseShardTable = table_open(DatabaseShardRelationId(), RowExclusiveLock);

	TupleDesc tupleDescriptor = RelationGetDescr(databaseShardTable);
	HeapTuple heapTuple = heap_form_tuple(tupleDescriptor, values, isNulls);

	CatalogTupleInsert(databaseShardTable, heapTuple);

	CommandCounterIncrement();
	table_close(databaseShardTable, NoLock);
}


/*
 * InsertDatabaseShardAssignmentOnOtherNodes inserts a record into the
 * citus_catalog.database_sharding table on other nodes.
 */
static void
InsertDatabaseShardAssignmentOnOtherNodes(Oid databaseOid, int nodeGroupId)
{
	char *insertCommand = InsertDatabaseShardAssignmentCommand(databaseOid, nodeGroupId);
	SendCommandToWorkersWithMetadata(insertCommand);
}


/*
 * UpdateDatabaseShard updates a database shard after it is moved to a new node.
 */
void
UpdateDatabaseShard(Oid databaseOid, int targetNodeGroupId)
{
	DeleteDatabaseShardByDatabaseId(databaseOid);
	InsertDatabaseShardAssignment(databaseOid, targetNodeGroupId);
	AllowConnectionsOnlyOnNodeGroup(databaseOid, targetNodeGroupId);

	ReconfigurePgBouncersOnCommit = true;
}


/*
 * DeleteDatabaseShardByDatabaseId deletes a record from the
 * citus_catalog.database_sharding table.
 */
static void
DeleteDatabaseShardByDatabaseId(Oid databaseOid)
{
	DeleteDatabaseShardByDatabaseIdLocally(databaseOid);

	if (EnableMetadataSync)
	{
		DeleteDatabaseShardByDatabaseIdOnOtherNodes(databaseOid);
	}
}


/*
 * DeleteDatabaseShardByDatabaseIdLocally deletes a database_shard record by database OID.
 */
void
DeleteDatabaseShardByDatabaseIdLocally(Oid databaseOid)
{
	Relation databaseShardTable = table_open(DatabaseShardRelationId(),
											 RowExclusiveLock);

	const int scanKeyCount = 1;
	ScanKeyData scanKey[1];
	bool indexOK = true;

	ScanKeyInit(&scanKey[0], Anum_database_shard_database_id,
				BTEqualStrategyNumber, F_OIDEQ, ObjectIdGetDatum(databaseOid));

	SysScanDesc scanDescriptor = systable_beginscan(databaseShardTable,
													DatabaseShardPrimaryKeyIndexId(),
													indexOK,
													NULL, scanKeyCount, scanKey);

	HeapTuple heapTuple = systable_getnext(scanDescriptor);
	if (heapTuple != NULL)
	{
		simple_heap_delete(databaseShardTable, &heapTuple->t_self);
	}

	systable_endscan(scanDescriptor);

	CommandCounterIncrement();
	table_close(databaseShardTable, NoLock);
}


/*
 * DeleteDatabaseShardByDatabaseIdOnOtherNodes deletes a record from the
 * citus_catalog.database_sharding table on other nodes.
 */
static void
DeleteDatabaseShardByDatabaseIdOnOtherNodes(Oid databaseOid)
{
	char *deleteCommand = DeleteDatabaseShardByDatabaseIdCommand(databaseOid);
	SendCommandToWorkersWithMetadata(deleteCommand);
}


/*
 * ListDatabaseShards lists all database shards in citus_catalog.database_shard.
 */
List *
ListDatabaseShards(void)
{
	Relation databaseShardTable = table_open(DatabaseShardRelationId(), AccessShareLock);
	TupleDesc tupleDescriptor = RelationGetDescr(databaseShardTable);

	List *dbShardList = NIL;
	int scanKeyCount = 0;
	bool indexOK = false;

	SysScanDesc scanDescriptor = systable_beginscan(databaseShardTable, InvalidOid,
													indexOK, NULL, scanKeyCount, NULL);

	HeapTuple heapTuple = NULL;
	while (HeapTupleIsValid(heapTuple = systable_getnext(scanDescriptor)))
	{
		DatabaseShard *dbShard = TupleToDatabaseShard(heapTuple, tupleDescriptor);
		dbShardList = lappend(dbShardList, dbShard);
	}

	systable_endscan(scanDescriptor);
	table_close(databaseShardTable, NoLock);

	return dbShardList;
}


/*
 * GetDatabaseShardByOid gets a database shard by database OID or
 * NULL if no database shard could be found.
 */
DatabaseShard *
GetDatabaseShardByOid(Oid databaseOid)
{
	DatabaseShard *result = NULL;

	Relation databaseShardTable = table_open(DatabaseShardRelationId(), AccessShareLock);
	TupleDesc tupleDescriptor = RelationGetDescr(databaseShardTable);

	const int scanKeyCount = 1;
	ScanKeyData scanKey[1];
	bool indexOK = true;

	ScanKeyInit(&scanKey[0], Anum_database_shard_database_id,
				BTEqualStrategyNumber, F_OIDEQ, ObjectIdGetDatum(databaseOid));

	SysScanDesc scanDescriptor = systable_beginscan(databaseShardTable,
													DatabaseShardPrimaryKeyIndexId(),
													indexOK,
													NULL, scanKeyCount, scanKey);

	HeapTuple heapTuple = systable_getnext(scanDescriptor);
	if (HeapTupleIsValid(heapTuple))
	{
		result = TupleToDatabaseShard(heapTuple, tupleDescriptor);
	}

	systable_endscan(scanDescriptor);
	table_close(databaseShardTable, NoLock);

	return result;
}


/*
 * TupleToDatabaseShard converts a database_shard record tuple into a DatabaseShard struct.
 */
static DatabaseShard *
TupleToDatabaseShard(HeapTuple heapTuple, TupleDesc tupleDescriptor)
{
	Datum datumArray[Natts_database_shard];
	bool isNullArray[Natts_database_shard];
	heap_deform_tuple(heapTuple, tupleDescriptor, datumArray, isNullArray);

	DatabaseShard *record = palloc0(sizeof(DatabaseShard));

	record->databaseOid =
		DatumGetObjectId(datumArray[Anum_database_shard_database_id - 1]);

	record->nodeGroupId =
		DatumGetInt32(datumArray[Anum_database_shard_node_group_id - 1]);

	record->isAvailable =
		DatumGetBool(datumArray[Anum_database_shard_is_available - 1]);

	return record;
}


/*
 * citus_internal_add_database_shard is an internal UDF to
 * add a row to database_shard.
 */
Datum
citus_internal_add_database_shard(PG_FUNCTION_ARGS)
{
	char *databaseName = TextDatumGetCString(PG_GETARG_DATUM(0));
	int nodeGroupId = PG_GETARG_INT32(1);

	bool missingOk = false;
	Oid databaseOid = get_database_oid(databaseName, missingOk);

	if (!pg_database_ownercheck(databaseOid, GetUserId()))
	{
		aclcheck_error(ACLCHECK_NOT_OWNER, OBJECT_DATABASE,
					   databaseName);
	}

	InsertDatabaseShardAssignmentLocally(databaseOid, nodeGroupId);

	/* make sure new database is added to pgbouncer config */
	ReconfigurePgBouncersOnCommit = true;

	PG_RETURN_VOID();
}


/*
 * InsertDatabaseShardAssignmentCommand returns a command to insert a database shard
 * assignment into the metadata on a remote node.
 */
static char *
InsertDatabaseShardAssignmentCommand(Oid databaseOid, int nodeGroupId)
{
	StringInfo command = makeStringInfo();
	char *databaseName = get_database_name(databaseOid);

	appendStringInfo(command,
					 "SELECT pg_catalog.citus_internal_add_database_shard(%s,%d)",
					 quote_literal_cstr(databaseName),
					 nodeGroupId);

	return command->data;
}


/*
 * citus_internal_delete_database_shard is an internal UDF to
 * delete a row from database_shard.
 */
Datum
citus_internal_delete_database_shard(PG_FUNCTION_ARGS)
{
	char *databaseName = TextDatumGetCString(PG_GETARG_DATUM(0));

	bool missingOk = false;
	Oid databaseOid = get_database_oid(databaseName, missingOk);

	if (!pg_database_ownercheck(databaseOid, GetUserId()))
	{
		aclcheck_error(ACLCHECK_NOT_OWNER, OBJECT_DATABASE,
					   databaseName);
	}

	DeleteDatabaseShardByDatabaseIdLocally(databaseOid);

	/* make sure new database is added to pgbouncer config */
	ReconfigurePgBouncersOnCommit = true;

	PG_RETURN_VOID();
}


/*
 * DeleteDatabaseShardByDatabaseIdCommand returns a command to delete a database shard
 * assignment from the metadata on a remote node.
 */
static char *
DeleteDatabaseShardByDatabaseIdCommand(Oid databaseOid)
{
	StringInfo command = makeStringInfo();
	char *databaseName = get_database_name(databaseOid);

	appendStringInfo(command,
					 "SELECT pg_catalog.citus_internal_delete_database_shard(%s)",
					 quote_literal_cstr(databaseName));

	return command->data;
}
