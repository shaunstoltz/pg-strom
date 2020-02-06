/*
 * main.c
 *
 * Entrypoint of PG-Strom extension, and misc uncategolized functions.
 * ----
 * Copyright 2011-2020 (C) KaiGai Kohei <kaigai@kaigai.gr.jp>
 * Copyright 2014-2020 (C) The PG-Strom Development Team
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */
#include "pg_strom.h"

PG_MODULE_MAGIC;

/*
 * miscellaneous GUC parameters
 */
bool		pgstrom_enabled;
bool		pgstrom_debug_kernel_source;
bool		pgstrom_cpu_fallback_enabled;
static int	pgstrom_chunk_size_kb;

/* cost factors */
double		pgstrom_gpu_setup_cost;
double		pgstrom_gpu_dma_cost;
double		pgstrom_gpu_operator_cost;

/* misc static variables */
static planner_hook_type	planner_hook_next;
static CustomPathMethods	pgstrom_dummy_path_methods;
static CustomScanMethods	pgstrom_dummy_plan_methods;

/* misc variables */
long		PAGE_SIZE;
long		PAGE_MASK;
int			PAGE_SHIFT;
long		PHYS_PAGES;

/* SQL function declarations */
Datum pgstrom_license_query(PG_FUNCTION_ARGS);

/* pg_strom.chunk_size */
Size
pgstrom_chunk_size(void)
{
	return ((Size)pgstrom_chunk_size_kb) << 10;
}

static void
pgstrom_init_misc_guc(void)
{
	/* turn on/off PG-Strom feature */
	DefineCustomBoolVariable("pg_strom.enabled",
							 "Enables the planner's use of PG-Strom",
							 NULL,
							 &pgstrom_enabled,
							 true,
							 PGC_USERSET,
							 GUC_NOT_IN_SAMPLE,
							 NULL, NULL, NULL);
	/* turn on/off CPU fallback if GPU could not execute the query */
	DefineCustomBoolVariable("pg_strom.cpu_fallback",
							 "Enables CPU fallback if GPU required re-run",
							 NULL,
							 &pgstrom_cpu_fallback_enabled,
							 false,
							 PGC_USERSET,
							 GUC_NOT_IN_SAMPLE,
							 NULL, NULL, NULL);
	/* turn on/off cuda kernel source saving */
	DefineCustomBoolVariable("pg_strom.debug_kernel_source",
							 "Turn on/off to display the kernel source path",
							 NULL,
							 &pgstrom_debug_kernel_source,
							 false,
							 PGC_USERSET,
							 GUC_NOT_IN_SAMPLE,
							 NULL, NULL, NULL);
	/* default length of pgstrom_data_store */
	DefineCustomIntVariable("pg_strom.chunk_size",
							"default size of pgstrom_data_store",
							NULL,
							&pgstrom_chunk_size_kb,
							65534,	/* almost 64MB */
							4096,
							MAX_KILOBYTES,
							PGC_INTERNAL,
							GUC_NOT_IN_SAMPLE | GUC_UNIT_KB,
							NULL, NULL, NULL);
	/* cost factor for Gpu setup */
	DefineCustomRealVariable("pg_strom.gpu_setup_cost",
							 "Cost to setup GPU device to run",
							 NULL,
							 &pgstrom_gpu_setup_cost,
							 4000 * DEFAULT_SEQ_PAGE_COST,
							 0,
							 DBL_MAX,
							 PGC_USERSET,
							 GUC_NOT_IN_SAMPLE,
							 NULL, NULL, NULL);
	/* cost factor for each Gpu task */
	DefineCustomRealVariable("pg_strom.gpu_dma_cost",
							 "Cost to send/recv data via DMA",
							 NULL,
							 &pgstrom_gpu_dma_cost,
							 10 * DEFAULT_SEQ_PAGE_COST,
							 0,
							 DBL_MAX,
                             PGC_USERSET,
                             GUC_NOT_IN_SAMPLE,
                             NULL, NULL, NULL);
	/* cost factor for Gpu operator */
	DefineCustomRealVariable("pg_strom.gpu_operator_cost",
							 "Cost of processing each operators by GPU",
							 NULL,
							 &pgstrom_gpu_operator_cost,
							 DEFAULT_CPU_OPERATOR_COST / 16.0,
							 0,
							 DBL_MAX,
							 PGC_USERSET,
							 GUC_NOT_IN_SAMPLE,
							 NULL, NULL, NULL);
}

/*
 * pgstrom_create_dummy_path
 */
Path *
pgstrom_create_dummy_path(PlannerInfo *root,
						  Path *subpath,
						  PathTarget *target)
{
	CustomPath *cpath = makeNode(CustomPath);

	cpath->path.pathtype		= T_CustomScan;
	cpath->path.parent			= subpath->parent;
	cpath->path.pathtarget		= target;
	cpath->path.param_info		= NULL;
	cpath->path.parallel_aware	= subpath->parallel_aware;
	cpath->path.parallel_safe	= subpath->parallel_safe;
	cpath->path.parallel_workers = subpath->parallel_workers;
	cpath->path.pathkeys		= subpath->pathkeys;
	cpath->path.rows			= subpath->rows;
	cpath->path.startup_cost	= subpath->startup_cost;
	cpath->path.total_cost		= subpath->total_cost;

	cpath->custom_paths			= list_make1(subpath);
	cpath->methods      		= &pgstrom_dummy_path_methods;

	return &cpath->path;
}

/*
 * pgstrom_dummy_create_plan - PlanCustomPath callback
 */
static Plan *
pgstrom_dummy_create_plan(PlannerInfo *root,
						  RelOptInfo *rel,
						  CustomPath *best_path,
						  List *tlist,
						  List *clauses,
						  List *custom_plans)
{
	CustomScan *cscan = makeNode(CustomScan);

	Assert(list_length(custom_plans) == 1);
	cscan->scan.plan.parallel_aware = best_path->path.parallel_aware;
	cscan->scan.plan.targetlist = tlist;
	cscan->scan.plan.qual = NIL;
	cscan->scan.plan.lefttree = linitial(custom_plans);
	cscan->scan.scanrelid = 0;
	cscan->custom_scan_tlist = tlist;
	cscan->methods = &pgstrom_dummy_plan_methods;

	return &cscan->scan.plan;
}

/*
 * pgstrom_dummy_remove_plan
 */
static Plan *
pgstrom_dummy_remove_plan(PlannedStmt *pstmt, CustomScan *cscan)
{
	Plan	   *subplan = outerPlan(cscan);
	ListCell   *lc1;
	ListCell   *lc2;

	Assert(innerPlan(cscan) == NULL &&
		   cscan->custom_plans == NIL);
	Assert(list_length(cscan->scan.plan.targetlist) ==
		   list_length(subplan->targetlist));
	/*
	 * Push down the resource name to subplan
	 */
	forboth (lc1, cscan->scan.plan.targetlist,
			 lc2, subplan->targetlist)
	{
		TargetEntry	   *tle_1 = lfirst(lc1);
		TargetEntry	   *tle_2 = lfirst(lc2);

		if (exprType((Node *)tle_1->expr) != exprType((Node *)tle_2->expr))
			elog(ERROR, "Bug? dummy custom scan node has incompatible tlist");

		if (tle_2->resname != NULL &&
			(tle_1->resname == NULL ||
			 strcmp(tle_1->resname, tle_2->resname) != 0))
		{
			elog(DEBUG2,
				 "attribute %d of subplan: [%s] is over-written by [%s]",
				 tle_2->resno,
				 tle_2->resname,
				 tle_1->resname);
		}
		if (tle_1->resjunk != tle_2->resjunk)
			elog(DEBUG2,
				 "attribute %d of subplan: [%s] is marked as %s attribute",
				 tle_2->resno,
                 tle_2->resname,
				 tle_1->resjunk ? "junk" : "non-junk");

		tle_2->resname = tle_1->resname;
		tle_2->resjunk = tle_1->resjunk;
	}
	return outerPlan(cscan);
}

/*
 * pgstrom_dummy_create_scan_state - CreateCustomScanState callback
 */
static Node *
pgstrom_dummy_create_scan_state(CustomScan *cscan)
{
	elog(ERROR, "Bug? dummy custom scan node still remain on executor stage");
}

/*
 * pgstrom_post_planner
 *
 * remove 'dummy' custom scan node.
 */
static void
pgstrom_post_planner_recurse(PlannedStmt *pstmt, Plan **p_plan)
{
	Plan	   *plan = *p_plan;
	ListCell   *lc;

	Assert(plan != NULL);

	switch (nodeTag(plan))
	{
		case T_ModifyTable:
			{
				ModifyTable *splan = (ModifyTable *) plan;

				foreach (lc, splan->plans)
					pgstrom_post_planner_recurse(pstmt, (Plan **)&lfirst(lc));
			}
			break;
			
		case T_Append:
			{
				Append	   *splan = (Append *) plan;

				foreach (lc, splan->appendplans)
					pgstrom_post_planner_recurse(pstmt, (Plan **)&lfirst(lc));
			}
			break;

		case T_MergeAppend:
			{
				MergeAppend *splan = (MergeAppend *) plan;

				foreach (lc, splan->mergeplans)
					pgstrom_post_planner_recurse(pstmt, (Plan **)&lfirst(lc));
			}
			break;

		case T_BitmapAnd:
			{
				BitmapAnd  *splan = (BitmapAnd *) plan;

				foreach (lc, splan->bitmapplans)
					pgstrom_post_planner_recurse(pstmt, (Plan **)&lfirst(lc));
			}
			break;

		case T_BitmapOr:
			{
				BitmapOr   *splan = (BitmapOr *) plan;

				foreach (lc, splan->bitmapplans)
					pgstrom_post_planner_recurse(pstmt, (Plan **)&lfirst(lc));
			}
			break;

		case T_SubqueryScan:
			{
				SubqueryScan *sscan = (SubqueryScan *) plan;

				pgstrom_post_planner_recurse(pstmt, &sscan->subplan);
			}
			break;

		case T_CustomScan:
			{
				CustomScan *cscan = (CustomScan *) plan;

				if (cscan->methods == &pgstrom_dummy_plan_methods)
				{
					*p_plan = pgstrom_dummy_remove_plan(pstmt, cscan);
					pgstrom_post_planner_recurse(pstmt, p_plan);
					return;
				}
				else if (pgstrom_plan_is_gpupreagg(&cscan->scan.plan))
					gpupreagg_post_planner(pstmt, cscan);

				foreach (lc, cscan->custom_plans)
					pgstrom_post_planner_recurse(pstmt, (Plan **)&lfirst(lc));
			}
			break;

		default:
			break;
	}

	if (plan->lefttree)
		pgstrom_post_planner_recurse(pstmt, &plan->lefttree);
	if (plan->righttree)
		pgstrom_post_planner_recurse(pstmt, &plan->righttree);
}

static PlannedStmt *
pgstrom_post_planner(Query *parse,
					 int cursorOptions,
					 ParamListInfo boundParams)
{
	PlannedStmt	   *pstmt;
	ListCell	   *lc;

	if (planner_hook_next)
		pstmt = planner_hook_next(parse, cursorOptions, boundParams);
	else
		pstmt = standard_planner(parse, cursorOptions, boundParams);

	pgstrom_post_planner_recurse(pstmt, &pstmt->planTree);
	foreach (lc, pstmt->subplans)
		pgstrom_post_planner_recurse(pstmt, (Plan **)&lfirst(lc));

	return pstmt;
}

/*
 * commercial_license_expired_at
 */
static TimestampTz	commercial_license_expired_timestamp = MIN_TIMESTAMP - 1;

TimestampTz
commercial_license_expired_at(void)
{
	return commercial_license_expired_timestamp;
}

/*
 * pgstrom_license_query
 */
static bool
commercial_license_query(StringInfo buf)
{
	StromCmd__LicenseInfo *cmd;
	size_t		buffer_sz = 10000;
	int			fdesc;
	int			i_year, i_mon, i_day;
	int			e_year, e_mon, e_day;
	int			i;
	struct tm	tm;
	TimestampTz	tv;

	cmd = alloca(sizeof(StromCmd__LicenseInfo) + buffer_sz);
	memset(cmd, 0, sizeof(StromCmd__LicenseInfo));
	cmd->buffer_sz = buffer_sz;

	fdesc = open(NVME_STROM_IOCTL_PATHNAME, O_RDONLY);
	if (fdesc < 0)
	{
		if (errno == ENOENT)
			elog(LOG, "PG-Strom: nvme_strom driver is not installed");
		else
			elog(LOG, "failed to open \"%s\": %m", NVME_STROM_IOCTL_PATHNAME);
		return false;
	}
	if (ioctl(fdesc, STROM_IOCTL__LICENSE_QUERY, cmd) != 0)
	{
		if (errno == EINVAL)
			elog(LOG, "PG-Strom: no valid commercial license is installed");
		else if (errno == EKEYEXPIRED)
			elog(LOG, "PG-Strom: commercial license is expired");
		else
			elog(LOG, "PG-Strom: failed on STROM_IOCTL__LICENSE_QUERY: %m");
		close(fdesc);
		return false;
	}
	/* convert to text */
	i_year = (cmd->issued_at / 10000);
	i_mon  = (cmd->issued_at / 100) % 100;
	i_day  = (cmd->issued_at % 100);
	e_year = (cmd->expired_at / 10000);
	e_mon  = (cmd->expired_at / 100) % 100;
	e_day  = (cmd->expired_at % 100);

	if (i_year < 2000 || i_year > 9999 || i_mon < 1 || i_mon > 12)
	{
		elog(LOG, "Strange date in the ISSUED_AT field: %08d",
			 cmd->issued_at);
		close(fdesc);
		return false;
	}
	if (e_year < 2000 || e_year > 9999 || e_mon < 1 || e_mon > 12)
	{
		elog(LOG, "Strange date in the EXPIRED_AT_AT field: %08d",
			 cmd->expired_at);
		close(fdesc);
		return false;
	}

	/* update expired timestamp */
	memset(&tm, 0, sizeof(struct tm));
	tm.tm_year = e_year;
	tm.tm_mon  = e_mon;
	tm.tm_mday = e_day;
	tv = (TimestampTz) mktime(&tm) -
		((POSTGRES_EPOCH_JDATE - UNIX_EPOCH_JDATE) * SECS_PER_DAY);
	tv = (tv + SECS_PER_DAY - 1) * USECS_PER_SEC;
	if (!IS_VALID_TIMESTAMP(commercial_license_expired_timestamp) ||
		tv > commercial_license_expired_timestamp)
		commercial_license_expired_timestamp = tv;

	/* make a JSON string  */
	appendStringInfo(buf,
					 "{ \"version\" : %u",
					 cmd->version);
	if (cmd->serial_nr)
		appendStringInfo(buf,
						 ", \"serial_nr\" : %s",
						 quote_identifier(cmd->serial_nr));
	appendStringInfo(buf,
					 ", \"issued_at\" : \"%d-%s-%d\""
					 ", \"expired_at\" : \"%d-%s-%d\"",
					 i_day, months[i_mon-1], i_year,
					 e_day, months[e_mon-1], e_year);
	if (cmd->licensee_org)
		appendStringInfo(buf,
						 ", \"licensee_org\" : %s",
						 quote_identifier(cmd->licensee_org));
	if (cmd->licensee_name)
		appendStringInfo(buf,
						 ", \"licensee_name\" : %s",
						 quote_identifier(cmd->licensee_name));
	if (cmd->licensee_mail)
		appendStringInfo(buf,
						 ", \"licensee_mail\" : %s",
						 quote_identifier(cmd->licensee_mail));
	if (cmd->description)
		appendStringInfo(buf,
						 ", \"description\" : %s",
						 quote_identifier(cmd->description));
	if (cmd->nr_gpus > 0)
	{
		appendStringInfo(buf, ", \"gpus\" : [");
		for (i=0; i < cmd->nr_gpus; i++)
		{
			appendStringInfo(
				buf,
				"%s{ \"uuid\" : \"%s\", \"pci_id\" : \"%04x:%02x:%02x.%d\" }",
				(i == 0 ? " " : " , "),
				cmd->u.gpus[i].uuid,
				cmd->u.gpus[i].domain,
				cmd->u.gpus[i].bus_id,
				cmd->u.gpus[i].dev_id,
				cmd->u.gpus[i].func_id);
		}
		appendStringInfo(buf, " ]");
	}
	appendStringInfo(buf, " }");

	return true;
}

Datum
pgstrom_license_query(PG_FUNCTION_ARGS)
{
	StringInfoData	buf;

	if (!superuser())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 (errmsg("only superuser can query commercial license"))));
	initStringInfo(&buf);
	if (!commercial_license_query(&buf))
		PG_RETURN_NULL();
	PG_RETURN_POINTER(DirectFunctionCall1(json_in, PointerGetDatum(buf.data)));
}
PG_FUNCTION_INFO_V1(pgstrom_license_query);

/*
 * check_heterodb_license
 */
static void
check_heterodb_license(void)
{
	StringInfoData	buf;

	initStringInfo(&buf);
	if (commercial_license_query(&buf))
		elog(LOG, "HeteroDB License: %s", buf.data);
	pfree(buf.data);
}

/*
 * _PG_init
 *
 * Main entrypoint of PG-Strom. It shall be invoked only once when postmaster
 * process is starting up, then it calls other sub-systems to initialize for
 * each ones.
 */
void
_PG_init(void)
{
	/*
	 * PG-Strom has to be loaded using shared_preload_libraries option
	 */
	if (!process_shared_preload_libraries_in_progress)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
		errmsg("PG-Strom must be loaded via shared_preload_libraries")));

	/* link nvrtc library according to the current CUDA version */
	pgstrom_init_nvrtc();

	/* dump version number */
#ifdef PGSTROM_VERSION
	elog(LOG, "PG-Strom version %s built for PostgreSQL %s",
		 PGSTROM_VERSION, PG_MAJORVERSION);
#else
	elog(LOG, "PG-Strom built for PostgreSQL %s", PG_MAJORVERSION);
#endif
	/* init misc variables */
	PAGE_SIZE = sysconf(_SC_PAGESIZE);
	PAGE_MASK = PAGE_SIZE - 1;
	PAGE_SHIFT = get_next_log2(PAGE_SIZE);
	PHYS_PAGES = sysconf(_SC_PHYS_PAGES);

	/* init GPU/CUDA infrastracture */
	pgstrom_init_misc_guc();
	pgstrom_init_shmbuf();
	pgstrom_init_gpu_device();
	pgstrom_init_gpu_mmgr();
	pgstrom_init_gpu_context();
	pgstrom_init_cuda_program();
	pgstrom_init_nvme_strom();

	/* registration of custom-scan providers */
	pgstrom_init_gputasks();
	pgstrom_init_gpuscan();
	pgstrom_init_gpujoin();
	pgstrom_init_inners();
	pgstrom_init_gpupreagg();
	pgstrom_init_relscan();

	/* miscellaneous initializations */
	pgstrom_init_codegen();
	pgstrom_init_plcuda();
	pgstrom_init_arrow_fdw();
	pgstrom_init_gstore_buf();
	pgstrom_init_gstore_fdw();

	/* check commercial license, if any */
	check_heterodb_license();

	/* dummy custom-scan node */
	memset(&pgstrom_dummy_path_methods, 0, sizeof(CustomPathMethods));
	pgstrom_dummy_path_methods.CustomName	= "Dummy";
	pgstrom_dummy_path_methods.PlanCustomPath
		= pgstrom_dummy_create_plan;

	memset(&pgstrom_dummy_plan_methods, 0, sizeof(CustomScanMethods));
	pgstrom_dummy_plan_methods.CustomName	= "Dummy";
	pgstrom_dummy_plan_methods.CreateCustomScanState
		= pgstrom_dummy_create_scan_state;

	/* planner hook registration */
	planner_hook_next = planner_hook;
	planner_hook = pgstrom_post_planner;
}
