/*-------------------------------------------------------------------------
 *
 * database.c
 *    Commands to interact with the database object in a distributed
 *    environment.
 *
 * Copyright (c) Citus Data, Inc.
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/htup_details.h"
#include "access/xact.h"
#include "catalog/objectaddress.h"
#include "catalog/pg_database.h"
#include "commands/dbcommands.h"
#include "miscadmin.h"
#include "nodes/parsenodes.h"
#include "utils/syscache.h"
#include "utils/builtins.h"

#include "distributed/commands.h"
#include "distributed/commands/utility_hook.h"
#include "distributed/deparser.h"
#include "distributed/metadata_sync.h"
#include "distributed/metadata_utility.h"
#include "distributed/multi_executor.h"
#include "distributed/relation_access_tracking.h"
#include "distributed/worker_transaction.h"
#include "distributed/deparser.h"
#include "distributed/worker_protocol.h"
#include "distributed/metadata/distobject.h"
#include "distributed/deparse_shard_query.h"
#include "distributed/listutils.h"
#include "distributed/adaptive_executor.h"


static AlterOwnerStmt * RecreateAlterDatabaseOwnerStmt(Oid databaseOid);


PG_FUNCTION_INFO_V1(citus_internal_database_command);
static Oid get_database_owner(Oid db_oid);
List * PreprocessGrantOnDatabaseStmt(Node *node, const char *queryString,
									 ProcessUtilityContext processUtilityContext);

/* controlled via GUC */
bool EnableCreateDatabasePropagation = true;
bool EnableAlterDatabaseOwner = true;

/*
 * AlterDatabaseOwnerObjectAddress returns the ObjectAddress of the database that is the
 * object of the AlterOwnerStmt. Errors if missing_ok is false.
 */
List *
AlterDatabaseOwnerObjectAddress(Node *node, bool missing_ok, bool isPostprocess)
{
	AlterOwnerStmt *stmt = castNode(AlterOwnerStmt, node);
	Assert(stmt->objectType == OBJECT_DATABASE);

	Oid databaseOid = get_database_oid(strVal((String *) stmt->object), missing_ok);
	ObjectAddress *address = palloc0(sizeof(ObjectAddress));
	ObjectAddressSet(*address, DatabaseRelationId, databaseOid);

	return list_make1(address);
}


/*
 * DatabaseOwnerDDLCommands returns a list of sql statements to idempotently apply a
 * change of the database owner on the workers so that the database is owned by the same
 * user on all nodes in the cluster.
 */
List *
DatabaseOwnerDDLCommands(const ObjectAddress *address)
{
	Node *stmt = (Node *) RecreateAlterDatabaseOwnerStmt(address->objectId);
	return list_make1(DeparseTreeNode(stmt));
}


/*
 * RecreateAlterDatabaseOwnerStmt creates an AlterOwnerStmt that represents the operation
 * of changing the owner of the database to its current owner.
 */
static AlterOwnerStmt *
RecreateAlterDatabaseOwnerStmt(Oid databaseOid)
{
	AlterOwnerStmt *stmt = makeNode(AlterOwnerStmt);

	stmt->objectType = OBJECT_DATABASE;
	stmt->object = (Node *) makeString(get_database_name(databaseOid));

	Oid ownerOid = get_database_owner(databaseOid);
	stmt->newowner = makeNode(RoleSpec);
	stmt->newowner->roletype = ROLESPEC_CSTRING;
	stmt->newowner->rolename = GetUserNameFromId(ownerOid, false);

	return stmt;
}


/*
 * get_database_owner returns the Oid of the role owning the database
 */
static Oid
get_database_owner(Oid db_oid)
{
	HeapTuple tuple = SearchSysCache1(DATABASEOID, ObjectIdGetDatum(db_oid));
	if (!HeapTupleIsValid(tuple))
	{
		ereport(ERROR, (errcode(ERRCODE_UNDEFINED_DATABASE),
						errmsg("database with OID %u does not exist", db_oid)));
	}

	Oid dba = ((Form_pg_database) GETSTRUCT(tuple))->datdba;

	ReleaseSysCache(tuple);

	return dba;
}


/*
 * PreprocessGrantOnDatabaseStmt is executed before the statement is applied to the local
 * postgres instance.
 *
 * In this stage we can prepare the commands that need to be run on all workers to grant
 * on databases.
 */
List *
PreprocessGrantOnDatabaseStmt(Node *node, const char *queryString,
							  ProcessUtilityContext processUtilityContext)
{
	if (!ShouldPropagate())
	{
		return NIL;
	}

	GrantStmt *stmt = castNode(GrantStmt, node);
	Assert(stmt->objtype == OBJECT_DATABASE);

	List *databaseList = stmt->objects;

	if (list_length(databaseList) == 0)
	{
		return NIL;
	}

	EnsureCoordinator();

	char *sql = DeparseTreeNode((Node *) stmt);

	List *commands = list_make3(DISABLE_DDL_PROPAGATION,
								(void *) sql,
								ENABLE_DDL_PROPAGATION);

	return NodeDDLTaskList(NON_COORDINATOR_NODES, commands);
}


/*
 * PreprocessAlterDatabaseStmt is executed before the statement is applied to the local
 * postgres instance.
 *
 * In this stage we can prepare the commands that need to be run on all workers to grant
 * on databases.
 */
List *
PreprocessAlterDatabaseStmt(Node *node, const char *queryString,
							ProcessUtilityContext processUtilityContext)
{
	if (!ShouldPropagate())
	{
		return NIL;
	}

	AlterDatabaseStmt *stmt = castNode(AlterDatabaseStmt, node);

	EnsureCoordinator();

	char *sql = DeparseTreeNode((Node *) stmt);

	List *commands = list_make3(DISABLE_DDL_PROPAGATION,
								(void *) sql,
								ENABLE_DDL_PROPAGATION);

	return NodeDDLTaskList(NON_COORDINATOR_NODES, commands);
}


#if PG_VERSION_NUM >= PG_VERSION_15

/*
 * PreprocessAlterDatabaseSetStmt is executed before the statement is applied to the local
 * postgres instance.
 *
 * In this stage we can prepare the commands that need to be run on all workers to grant
 * on databases.
 */
List *
PreprocessAlterDatabaseRefreshCollStmt(Node *node, const char *queryString,
									   ProcessUtilityContext processUtilityContext)
{
	if (!ShouldPropagate())
	{
		return NIL;
	}

	AlterDatabaseRefreshCollStmt *stmt = castNode(AlterDatabaseRefreshCollStmt, node);

	EnsureCoordinator();

	char *sql = DeparseTreeNode((Node *) stmt);

	List *commands = list_make3(DISABLE_DDL_PROPAGATION,
								(void *) sql,
								ENABLE_DDL_PROPAGATION);

	return NodeDDLTaskList(NON_COORDINATOR_NODES, commands);
}


#endif

/*
 * PreprocessAlterDatabaseSetStmt is executed before the statement is applied to the local
 * postgres instance.
 *
 * In this stage we can prepare the commands that need to be run on all workers to grant
 * on databases.
 */
List *
PreprocessAlterDatabaseSetStmt(Node *node, const char *queryString,
							   ProcessUtilityContext processUtilityContext)
{
	if (!ShouldPropagate())
	{
		return NIL;
	}

	AlterDatabaseSetStmt *stmt = castNode(AlterDatabaseSetStmt, node);

	EnsureCoordinator();

	char *sql = DeparseTreeNode((Node *) stmt);

	List *commands = list_make3(DISABLE_DDL_PROPAGATION,
								(void *) sql,
								ENABLE_DDL_PROPAGATION);

	return NodeDDLTaskList(NON_COORDINATOR_NODES, commands);
}


/*
 * PostprocessCreatedbStmt is executed after the statement is applied to the local
 * postgres instance. In this stage we can prepare the commands that need to be run on
 * all workers to create the database.
 *
 */
List *
PostprocessCreateDatabaseStmt(Node *node, const char *queryString)
{
	if (!ShouldPropagate())
	{
		return NIL;
	}

	EnsureCoordinator();

	char *createDatabaseCommand = DeparseTreeNode(node);

	StringInfo internalCreateCommand = makeStringInfo();
	appendStringInfo(internalCreateCommand,
					 "SELECT pg_catalog.citus_internal_database_command(%s)",
					 quote_literal_cstr(createDatabaseCommand));

	List *commands = list_make3(DISABLE_DDL_PROPAGATION,
								(void *) internalCreateCommand->data,
								ENABLE_DDL_PROPAGATION);

	return NodeDDLTaskList(NON_COORDINATOR_NODES, commands);
}


/*
 * citus_internal_database_command is an internal UDF to
 * create/drop a database in an idempotent maner without
 * transaction block restrictions.
 */
Datum
citus_internal_database_command(PG_FUNCTION_ARGS)
{
	int saveNestLevel = NewGUCNestLevel();
	text *commandText = PG_GETARG_TEXT_P(0);
	char *command = text_to_cstring(commandText);
	Node *parseTree = ParseTreeNode(command);

	set_config_option("citus.enable_ddl_propagation", "off",
					  (superuser() ? PGC_SUSET : PGC_USERSET), PGC_S_SESSION,
					  GUC_ACTION_LOCAL, true, 0, false);

	set_config_option("citus.enable_create_database_propagation", "off",
					  (superuser() ? PGC_SUSET : PGC_USERSET), PGC_S_SESSION,
					  GUC_ACTION_LOCAL, true, 0, false);

	/*
	 * createdb() / DropDatabase() uses ParseState to report the error position for the
	 * input command and the position is reported to be 0 when it's provided as NULL.
	 * We're okay with that because we don't expect this UDF to be called with an incorrect
	 * DDL command.
	 *
	 */
	ParseState *pstate = NULL;

	if (IsA(parseTree, CreatedbStmt))
	{
		CreatedbStmt *stmt = castNode(CreatedbStmt, parseTree);

		bool missingOk = true;
		Oid databaseOid = get_database_oid(stmt->dbname, missingOk);

		if (!OidIsValid(databaseOid))
		{
			createdb(pstate, (CreatedbStmt *) parseTree);
		}
	}
	else if (IsA(parseTree, DropdbStmt))
	{
		DropdbStmt *stmt = castNode(DropdbStmt, parseTree);

		bool missingOk = false;
		Oid databaseOid = get_database_oid(stmt->dbname, missingOk);


		if (OidIsValid(databaseOid))
		{
			DropDatabase(pstate, (DropdbStmt *) parseTree);
		}
	}
	else
	{
		ereport(ERROR, (errmsg("unsupported command type %d", nodeTag(parseTree))));
	}

	/* Below command rollbacks flags to the state before this session*/
	AtEOXact_GUC(true, saveNestLevel);

	PG_RETURN_VOID();
}


List *
PreprocessDropDatabaseStmt(Node *node, const char *queryString,
						   ProcessUtilityContext processUtilityContext)
{
	if (!ShouldPropagate())
	{
		return NIL;
	}

	EnsureCoordinator();

	char *dropDatabaseCommand = DeparseTreeNode(node);

	StringInfo internalDropCommand = makeStringInfo();
	appendStringInfo(internalDropCommand,
					 "SELECT pg_catalog.citus_internal_database_command(%s)",
					 quote_literal_cstr(dropDatabaseCommand));

	List *commands = list_make3(DISABLE_DDL_PROPAGATION,
								(void *) internalDropCommand->data,
								ENABLE_DDL_PROPAGATION);

	return NodeDDLTaskList(NON_COORDINATOR_NODES, commands);
}
