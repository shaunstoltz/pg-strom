/*
 * arrow_fdw.c
 *
 * Routines to map Apache Arrow files as PG's Foreign-Table.
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
#include "arrow_defs.h"
#include "arrow_ipc.h"
#include "cuda_numeric.cu"

/*
 * RecordBatchState
 */
typedef struct RecordBatchFieldState
{
	Oid			atttypid;
	int			atttypmod;
	ArrowTypeOptions attopts;
	int64		nitems;				/* usually, same with rb_nitems */
	int64		null_count;
	off_t		nullmap_offset;
	size_t		nullmap_length;
	off_t		values_offset;
	size_t		values_length;
	off_t		extra_offset;
	size_t		extra_length;
	int			num_children;
	struct RecordBatchFieldState *children;
} RecordBatchFieldState;

typedef struct RecordBatchState
{
	File		fdesc;
	struct stat	stat_buf;
	int			rb_index;	/* index number in a file */
	off_t		rb_offset;	/* offset from the head */
	size_t		rb_length;	/* length of the entire RecordBatch */
	int64		rb_nitems;	/* number of items */
	/* per column information */
	int			ncols;
	RecordBatchFieldState columns[FLEXIBLE_ARRAY_MEMBER];
} RecordBatchState;

/*
 * metadata cache (on shared memory)
 */
typedef struct
{
	dev_t		st_dev;
	ino_t		st_ino;
	uint32		hash;
} MetadataCacheKey;

typedef struct
{
	dlist_node	chain;
	dlist_node	lru_chain;
	dlist_head	siblings;	/* if two or more record batches per file */
	/* key of RecordBatch metadata cache */
	struct stat	stat_buf;
	uint32		hash;
	/* fields from RecordBatchState */
	int			rb_index;	/* index of the RecordBatch */
	off_t		rb_offset;	/* offset from the head */
    size_t		rb_length;	/* length of the entire RecordBatch */
    int64		rb_nitems;	/* number of items */
	int			ncols;
	int			nfields;	/* length of fstate[] array */
	RecordBatchFieldState fstate[FLEXIBLE_ARRAY_MEMBER];
} arrowMetadataCache;

#define ARROW_METADATA_HASH_NSLOTS		2048
typedef struct
{
	slock_t		lru_lock;
	dlist_head	lru_list;
	pg_atomic_uint64 consumed;

	LWLock		lock_slots[ARROW_METADATA_HASH_NSLOTS];
	dlist_head	hash_slots[ARROW_METADATA_HASH_NSLOTS];
	dlist_head	mvcc_slots[ARROW_METADATA_HASH_NSLOTS];
} arrowMetadataState;

/*
 * MVCC state for the pending writes
 */
typedef struct
{
	dlist_node	chain;
	MetadataCacheKey key;
	TransactionId xid;
	CommandId	cid;
	uint32		record_batch;
} arrowWriteMVCCLog;

/*
 * REDO Log for INSERT/TRUNCATE
 */
typedef struct
{
	dlist_node	chain;
	MetadataCacheKey key;
	TransactionId xid;
	CommandId	cid;
	char	   *pathname;
	bool		is_truncate;
	/* for TRUNCATE */
	uint32		suffix;
	/* for INSERT */
	loff_t		footer_offset;
	size_t		footer_length;
	char		footer_backup[FLEXIBLE_ARRAY_MEMBER];
} arrowWriteRedoLog;

/*
 * arrowWriteState
 */
typedef struct
{
	MemoryContext memcxt;
	File		file;
	MetadataCacheKey key;
	uint32		hash;
	bool		redo_log_written;
	SQLtable	sql_table;
} arrowWriteState;

/*
 * ArrowFdwState
 */
struct ArrowFdwState
{
	List	   *fdescList;
	Bitmapset  *referenced;
	pg_atomic_uint32   *rbatch_index;
	pg_atomic_uint32	__rbatch_index_local;	/* if single process exec */
	pgstrom_data_store *curr_pds;	/* current focused buffer */
	cl_ulong	curr_index;			/* current index to row on KDS */
	/* state of RecordBatches */
	uint32		num_rbatches;
	RecordBatchState *rbatches[FLEXIBLE_ARRAY_MEMBER];
};

/* ---------- static variables ---------- */
static FdwRoutine		pgstrom_arrow_fdw_routine;
static shmem_startup_hook_type shmem_startup_next = NULL;
static arrowMetadataState *arrow_metadata_state = NULL;
static dlist_head		arrow_write_redo_list;
static bool				arrow_fdw_enabled;				/* GUC */
static int				arrow_metadata_cache_size_kb;	/* GUC */
static size_t			arrow_metadata_cache_size;
static char			   *arrow_debug_row_numbers_hint;	/* GUC */
static int				arrow_record_batch_size_kb;		/* GUC */

/* ---------- static functions ---------- */
static bool		arrowTypeIsEqual(ArrowField *a, ArrowField *b, int depth);
static Oid		arrowTypeToPGTypeOid(ArrowField *field, int *typmod);
static const char *arrowTypeToPGTypeName(ArrowField *field);
static size_t	arrowFieldLength(ArrowField *field, int64 nitems);
static bool		arrowSchemaCompatibilityCheck(TupleDesc tupdesc,
											  RecordBatchState *rb_state);
static List	   *__arrowFdwExtractFilesList(List *options_list,
										   int *p_parallel_nworkers,
										   bool *p_writable,
										   int *p_pinning);
static List	   *arrowFdwExtractFilesList(List *options_list);
static RecordBatchState *makeRecordBatchState(ArrowSchema *schema,
											  ArrowBlock *block,
											  ArrowRecordBatch *rbatch);
static List	   *arrowLookupOrBuildMetadataCache(File fdesc);
static void		pg_datum_arrow_ref(kern_data_store *kds,
								   kern_colmeta *cmeta,
								   size_t index,
								   Datum *p_datum,
								   bool *p_isnull);
/* routines for writable arrow_fdw foreign tables */
static arrowWriteState *createArrowWriteState(Relation frel, File file);
static void writeOutArrowRecordBatch(arrowWriteState *aw_state,
									 bool with_footer);

Datum	pgstrom_arrow_fdw_handler(PG_FUNCTION_ARGS);
Datum	pgstrom_arrow_fdw_validator(PG_FUNCTION_ARGS);
Datum	pgstrom_arrow_fdw_precheck_schema(PG_FUNCTION_ARGS);
Datum	pgstrom_arrow_fdw_truncate(PG_FUNCTION_ARGS);

/*
 * baseRelIsArrowFdw
 */
bool
baseRelIsArrowFdw(RelOptInfo *baserel)
{
	if ((baserel->reloptkind == RELOPT_BASEREL ||
		 baserel->reloptkind == RELOPT_OTHER_MEMBER_REL) &&
		baserel->rtekind == RTE_RELATION &&
		OidIsValid(baserel->serverid) &&
		baserel->fdwroutine &&
		memcmp(baserel->fdwroutine,
			   &pgstrom_arrow_fdw_routine,
			   sizeof(FdwRoutine)) == 0)
		return true;

	return false;
}

/*
 * RecordBatchFieldCount
 */
static int
__RecordBatchFieldCount(RecordBatchFieldState *fstate)
{
	int		j, count = 1;

	for (j=0; j < fstate->num_children; j++)
		count += __RecordBatchFieldCount(&fstate->children[j]);

	return count;
}

static int
RecordBatchFieldCount(RecordBatchState *rbstate)
{
	int		j, count = 0;

	for (j=0; j < rbstate->ncols; j++)
		count += __RecordBatchFieldCount(&rbstate->columns[j]);

	return count;
}

/*
 * RecordBatchFieldLength
 */
static size_t
RecordBatchFieldLength(RecordBatchFieldState *fstate)
{
	size_t	len = 0;
	int		j;

	if (fstate->nullmap_offset > 0)
		len += fstate->nullmap_length;
	if (fstate->values_offset > 0)
		len += fstate->values_length;
	if (fstate->extra_offset > 0)
		len += fstate->extra_length;
	len = BLCKALIGN(len);
	for (j=0; j < fstate->num_children; j++)
		len += RecordBatchFieldLength(&fstate->children[j]);
	return len;
}

/*
 * apply_debug_row_numbers_hint
 *
 * It is an ad-hoc optimization infrastructure. In case of estimated numbers
 * of row is completely wrong, we can give manual hint for optimizer by the
 * GUC: arrow_fdw.debug_row_numbers_hint.
 */
static void
apply_debug_row_numbers_hint(PlannerInfo *root,
							 RelOptInfo *baserel,
							 ForeignTable *ft)
{
	const char *errmsg = "wrong arrow_fdw.debug_row_numbers_hint config";
	char	   *config = pstrdup(arrow_debug_row_numbers_hint);
	char	   *relname = get_rel_name(ft->relid);
	char	   *token, *pos;
	double		nrows_others = -1.0;

	token = strtok_r(config, ",", &pos);
	while (token)
	{
		char   *comma = strchr(token, ':');
		char   *name, *nrows, *c;

		if (!comma)
			elog(ERROR, "%s - must be comma separated NAME:NROWS pairs",
				 errmsg);

		nrows = __trim(comma+1);
		*comma = '\0';
		name = __trim(token);

		for (c = nrows; *c != '\0'; c++)
		{
			if (!isdigit(*c))
				elog(ERROR, "%s - NROWS token contains non-digit ('%c')",
					 errmsg, *c);
		}

		if (strcmp(name, "*") == 0)
		{
			/* wildcard */
			if (nrows_others >= 0.0)
				elog(ERROR, "%s - wildcard (*) appears twice", errmsg);
			nrows_others = atof(nrows);
		}
		else if (strcmp(name, relname) == 0)
		{
			baserel->rows = atof(nrows);
			pfree(config);
			return;
		}
		token = strtok_r(NULL, ",", &pos);
	}
	if (nrows_others >= 0.0)
		baserel->rows = nrows_others;
	pfree(config);
}

/*
 * ArrowGetForeignRelSize
 */
static void
ArrowGetForeignRelSize(PlannerInfo *root,
					   RelOptInfo *baserel,
					   Oid foreigntableid)
{
	ForeignTable   *ft = GetForeignTable(foreigntableid);
	List		   *filesList;
	Size			filesSizeTotal = 0;
	Bitmapset	   *referenced = NULL;
	BlockNumber		npages = 0;
	double			ntuples = 0.0;
	ListCell	   *lc;
	int				parallel_nworkers;
	int				optimal_gpu = INT_MAX;
	int				j, k;

	/* columns to be fetched */
	foreach (lc, baserel->baserestrictinfo)
	{
		RestrictInfo   *rinfo = lfirst(lc);

		pull_varattnos((Node *)rinfo->clause, baserel->relid, &referenced);
	}
	referenced = pgstrom_pullup_outer_refs(root, baserel, referenced);

	filesList = __arrowFdwExtractFilesList(ft->options,
										   &parallel_nworkers,
										   NULL,
										   NULL);
	foreach (lc, filesList)
	{
		char	   *fname = strVal(lfirst(lc));
		File		fdesc;
		List	   *rb_cached;
		ListCell   *cell;
		size_t		len = 0;

		fdesc = PathNameOpenFile(fname, O_RDONLY | PG_BINARY);
		if (fdesc < 0)
		{
			elog(NOTICE, "failed to open file '%s' on behalf of '%s', skipped",
				 fname, get_rel_name(foreigntableid));
			continue;
		}
		k = GetOptimalGpuForFile(fdesc);
		if (optimal_gpu == INT_MAX)
			optimal_gpu = k;
		else if (optimal_gpu != k)
			optimal_gpu = -1;

		rb_cached = arrowLookupOrBuildMetadataCache(fdesc);
		foreach (cell, rb_cached)
		{
			RecordBatchState   *rb_state = lfirst(cell);

			if (cell == list_head(rb_cached))
				filesSizeTotal += BLCKALIGN(rb_state->stat_buf.st_size);

			if (bms_is_member(-FirstLowInvalidHeapAttributeNumber, referenced))
			{
				for (j=0; j < rb_state->ncols; j++)
					len += RecordBatchFieldLength(&rb_state->columns[j]);
			}
			else
			{
				for (k = bms_next_member(referenced, -1);
					 k >= 0;
					 k = bms_next_member(referenced, k))
				{
					j = k + FirstLowInvalidHeapAttributeNumber;
					if (j < 0 || j >= rb_state->ncols)
						continue;
					len += RecordBatchFieldLength(&rb_state->columns[j]);
				}
			}
			ntuples += rb_state->rb_nitems;
		}
		npages = len / BLCKSZ;
		FileClose(fdesc);
	}
	bms_free(referenced);

	if (optimal_gpu < 0 || optimal_gpu >= numDevAttrs)
		optimal_gpu = -1;
	else if (filesSizeTotal < nvme_strom_threshold())
		optimal_gpu = -1;

	baserel->rel_parallel_workers = parallel_nworkers;
	baserel->fdw_private = makeInteger(optimal_gpu);
	baserel->pages = npages;
	baserel->tuples = ntuples;
	baserel->rows = ntuples *
		clauselist_selectivity(root,
							   baserel->baserestrictinfo,
							   0,
							   JOIN_INNER,
							   NULL);
	if (arrow_debug_row_numbers_hint)
		apply_debug_row_numbers_hint(root, baserel, ft);
}

/*
 * GetOptimalGpuForArrowFdw
 *
 * optimal GPU index is saved at baserel->fdw_private
 */
cl_int
GetOptimalGpuForArrowFdw(PlannerInfo *root, RelOptInfo *baserel)
{
	if (!baserel->fdw_private)
	{
		RangeTblEntry *rte = root->simple_rte_array[baserel->relid];

		ArrowGetForeignRelSize(root, baserel, rte->relid);
	}
	return intVal(baserel->fdw_private);
}

static void
cost_arrow_fdw_seqscan(Path *path,
					   PlannerInfo *root,
					   RelOptInfo *baserel,
					   ParamPathInfo *param_info,
					   int num_workers)
{
	Cost		startup_cost = 0.0;
	Cost		disk_run_cost = 0.0;
	Cost		cpu_run_cost = 0.0;
	QualCost	qcost;
	double		nrows;
	double		spc_seq_page_cost;

	if (param_info)
		nrows = param_info->ppi_rows;
	else
		nrows = baserel->rows;

	/* arrow_fdw.enabled */
	if (!arrow_fdw_enabled)
		startup_cost += disable_cost;

	/*
	 * Storage costs
	 *
	 * XXX - smaller number of columns to read shall have less disk cost
	 * because of columnar format. Right now, we don't discount cost for
	 * the pages not to be read.
	 */
	get_tablespace_page_costs(baserel->reltablespace,
							  NULL,
							  &spc_seq_page_cost);
	disk_run_cost = spc_seq_page_cost * baserel->pages;

	/* CPU costs */
	if (param_info)
	{
		cost_qual_eval(&qcost, param_info->ppi_clauses, root);
		qcost.startup += baserel->baserestrictcost.startup;
        qcost.per_tuple += baserel->baserestrictcost.per_tuple;
	}
	else
		qcost = baserel->baserestrictcost;
	startup_cost += qcost.startup;
	cpu_run_cost = (cpu_tuple_cost + qcost.per_tuple) * baserel->tuples;

	/* tlist evaluation costs */
	startup_cost += path->pathtarget->cost.startup;
	cpu_run_cost += path->pathtarget->cost.per_tuple * path->rows;

	/* adjust cost for CPU parallelism */
	if (num_workers > 0)
	{
		double		leader_contribution;
		double		parallel_divisor = (double) num_workers;

		/* see get_parallel_divisor() */
		leader_contribution = 1.0 - (0.3 * (double)num_workers);
		parallel_divisor += Max(leader_contribution, 0.0);

		/* The CPU cost is divided among all the workers. */
		cpu_run_cost /= parallel_divisor;

		/* Estimated row count per background worker process */
		nrows = clamp_row_est(nrows / parallel_divisor);
	}
	path->rows = nrows;
	path->startup_cost = startup_cost;
	path->total_cost = startup_cost + cpu_run_cost + disk_run_cost;
	path->parallel_workers = num_workers;
}

/*
 * ArrowGetForeignPaths
 */
static void
ArrowGetForeignPaths(PlannerInfo *root,
					 RelOptInfo *baserel,
					 Oid foreigntableid)
{
	ForeignPath	   *fpath;
	ParamPathInfo  *param_info;
	Relids			required_outer = baserel->lateral_relids;

	param_info = get_baserel_parampathinfo(root, baserel, required_outer);

	fpath = create_foreignscan_path(root, baserel,
									NULL,	/* default pathtarget */
									-1,		/* dummy */
									-1.0,	/* dummy */
									-1.0,	/* dummy */
									NIL,	/* no pathkeys */
									required_outer,
									NULL,	/* no extra plan */
									NIL);	/* no particular private */
	cost_arrow_fdw_seqscan(&fpath->path, root, baserel, param_info, 0);
	add_path(baserel, (Path *)fpath);

	if (baserel->consider_parallel)
	{
		int		num_workers =
			compute_parallel_worker(baserel,
									baserel->pages, -1.0,
									max_parallel_workers_per_gather);
		if (num_workers == 0)
			return;

		fpath = create_foreignscan_path(root,
										baserel,
										NULL,	/* default pathtarget */
										-1,		/* dummy */
										-1.0,	/* dummy */
										-1.0,	/* dummy */
										NIL,	/* no pathkeys */
										required_outer,
										NULL,	/* no extra plan */
										NIL);	/* no particular private */
		fpath->path.parallel_aware = true;

		cost_arrow_fdw_seqscan(&fpath->path, root, baserel, param_info,
							   num_workers);
		add_partial_path(baserel, (Path *)fpath);
	}
}

/*
 * ArrowGetForeignPlan
 */
static ForeignScan *
ArrowGetForeignPlan(PlannerInfo *root,
					RelOptInfo *baserel,
					Oid foreigntableid,
					ForeignPath *best_path,
					List *tlist,
					List *scan_clauses,
					Plan *outer_plan)
{
	Bitmapset  *referenced = NULL;
	List	   *ref_list = NIL;
	ListCell   *lc;
	int			i, j, k;

	Assert(IS_SIMPLE_REL(baserel));
	/* pick up referenced attributes */
	foreach (lc, baserel->baserestrictinfo)
	{
		RestrictInfo   *rinfo = lfirst(lc);

		pull_varattnos((Node *)rinfo->clause, baserel->relid, &referenced);
	}
	for (i=baserel->min_attr, j=0; i <= baserel->max_attr; i++, j++)
	{
		if (baserel->attr_needed[j] != NULL)
		{
			k = i - FirstLowInvalidHeapAttributeNumber;
			referenced = bms_add_member(referenced, k);
		}
	}
	for (k = bms_next_member(referenced, -1);
		 k >= 0;
		 k = bms_next_member(referenced, k))
	{
		j = k + FirstLowInvalidHeapAttributeNumber;
		ref_list = lappend_int(ref_list, j);
	}
	bms_free(referenced);

	return make_foreignscan(tlist,
							extract_actual_clauses(scan_clauses, false),
							baserel->relid,
							NIL,	/* no expressions to evaluate */
							ref_list, /* list of referenced attnums */
							NIL,	/* no custom tlist */
							NIL,	/* no remote quals */
							outer_plan);
}

typedef struct
{
	ArrowBuffer    *buffer_curr;
	ArrowBuffer    *buffer_tail;
	ArrowFieldNode *fnode_curr;
	ArrowFieldNode *fnode_tail;
} setupRecordBatchContext;

static void
assignArrowTypeOptions(ArrowTypeOptions *attopts, const ArrowType *atype)
{
	memset(attopts, 0, sizeof(ArrowTypeOptions));
	switch (atype->node.tag)
	{
		case ArrowNodeTag__Decimal:
			if (atype->Decimal.precision < SHRT_MIN ||
				atype->Decimal.precision > SHRT_MAX)
				elog(ERROR, "Decimal precision is out of range");
			if (atype->Decimal.scale < SHRT_MIN ||
				atype->Decimal.scale > SHRT_MAX)
				elog(ERROR, "Decimal scale is out of range");
			attopts->decimal.precision = atype->Decimal.precision;
			attopts->decimal.scale     = atype->Decimal.scale;
			break;
		case ArrowNodeTag__Date:
			if (atype->Date.unit == ArrowDateUnit__Day ||
				atype->Date.unit == ArrowDateUnit__MilliSecond)
				attopts->date.unit = atype->Date.unit;
			else
				elog(ERROR, "unknown unit of Date");
			break;
		case ArrowNodeTag__Time:
			if (atype->Time.unit == ArrowTimeUnit__Second ||
				atype->Time.unit == ArrowTimeUnit__MilliSecond ||
				atype->Time.unit == ArrowTimeUnit__MicroSecond ||
				atype->Time.unit == ArrowTimeUnit__NanoSecond)
				attopts->time.unit = atype->Time.unit;
			else
				elog(ERROR, "unknown unit of Time");
			break;
		case ArrowNodeTag__Timestamp:
			if (atype->Timestamp.unit == ArrowTimeUnit__Second ||
				atype->Timestamp.unit == ArrowTimeUnit__MilliSecond ||
				atype->Timestamp.unit == ArrowTimeUnit__MicroSecond ||
				atype->Timestamp.unit == ArrowTimeUnit__NanoSecond)
				attopts->time.unit = atype->Timestamp.unit;
			else
				elog(ERROR, "unknown unit of Timestamp");
			break;
		case ArrowNodeTag__Interval:
			if (atype->Interval.unit == ArrowIntervalUnit__Year_Month ||
				atype->Interval.unit == ArrowIntervalUnit__Day_Time)
				attopts->interval.unit = atype->Interval.unit;
			else
				elog(ERROR, "unknown unit of Interval");
			break;
		default:
			/* no extra attributes */
			break;
	}
}

static void
setupRecordBatchField(setupRecordBatchContext *con,
					  RecordBatchFieldState *fstate,
					  ArrowField  *field,
					  int depth)
{
	ArrowBuffer	   *buffer_curr;
	ArrowFieldNode *fnode;

	if (con->fnode_curr >= con->fnode_tail)
		elog(ERROR, "RecordBatch has less ArrowFieldNode than expected");
	fnode = con->fnode_curr++;
	fstate->atttypid   = arrowTypeToPGTypeOid(field, &fstate->atttypmod);
	fstate->nitems     = fnode->length;
	fstate->null_count = fnode->null_count;

	switch (field->type.node.tag)
	{
		case ArrowNodeTag__Int:
		case ArrowNodeTag__FloatingPoint:
		case ArrowNodeTag__Bool:
		case ArrowNodeTag__Decimal:
		case ArrowNodeTag__Date:
		case ArrowNodeTag__Time:
		case ArrowNodeTag__Timestamp:
		case ArrowNodeTag__Interval:
		case ArrowNodeTag__FixedSizeBinary:
			/* fixed length values */
			if (con->buffer_curr + 2 > con->buffer_tail)
				elog(ERROR, "RecordBatch has less buffers than expected");
			buffer_curr = con->buffer_curr++;
			if (fstate->null_count > 0)
			{
				fstate->nullmap_offset = buffer_curr->offset;
				fstate->nullmap_length = buffer_curr->length;
				if (fstate->nullmap_length < BITMAPLEN(fstate->nitems))
					elog(ERROR, "nullmap length is smaller than expected");
				if ((fstate->nullmap_offset & (MAXIMUM_ALIGNOF - 1)) != 0 ||
					(fstate->nullmap_length & (MAXIMUM_ALIGNOF - 1)) != 0)
					elog(ERROR, "nullmap is not aligned well");
			}
			buffer_curr = con->buffer_curr++;
			fstate->values_offset = buffer_curr->offset;
			fstate->values_length = buffer_curr->length;
			if (fstate->values_length < arrowFieldLength(field,fstate->nitems))
				elog(ERROR, "values array is smaller than expected");
			if ((fstate->values_offset & (MAXIMUM_ALIGNOF - 1)) != 0 ||
				(fstate->values_length & (MAXIMUM_ALIGNOF - 1)) != 0)
				elog(ERROR, "values array is not aligned well");
			break;

		case ArrowNodeTag__List:
			if (field->_num_children != 1)
				elog(ERROR, "Bug? List of arrow type is corrupted");
			if (depth > 0)
				elog(ERROR, "nested array type is not supported");
			/* nullmap */
			if (con->buffer_curr + 1 > con->buffer_tail)
				elog(ERROR, "RecordBatch has less buffers than expected");
			buffer_curr = con->buffer_curr++;
			if (fstate->null_count > 0)
			{
				fstate->nullmap_offset = buffer_curr->offset;
				fstate->nullmap_length = buffer_curr->length;
				if (fstate->nullmap_length < BITMAPLEN(fstate->nitems))
					elog(ERROR, "nullmap length is smaller than expected");
				if ((fstate->nullmap_offset & (MAXIMUM_ALIGNOF - 1)) != 0 ||
					(fstate->nullmap_length & (MAXIMUM_ALIGNOF - 1)) != 0)
					elog(ERROR, "nullmap is not aligned well");
			}
			/* offset values */
			buffer_curr = con->buffer_curr++;
			fstate->values_offset = buffer_curr->offset;
			fstate->values_length = buffer_curr->length;
			if (fstate->values_length < arrowFieldLength(field,fstate->nitems))
				elog(ERROR, "offset array is smaller than expected");
			if ((fstate->values_offset & (MAXIMUM_ALIGNOF - 1)) != 0 ||
				(fstate->values_length & (MAXIMUM_ALIGNOF - 1)) != 0)
				elog(ERROR, "offset array is not aligned well");
			/* setup array element */
			fstate->children = palloc0(sizeof(RecordBatchFieldState));
			setupRecordBatchField(con,
								  &fstate->children[0],
								  &field->children[0],
								  depth+1);
			fstate->num_children = 1;
			break;

		case ArrowNodeTag__Utf8:
		case ArrowNodeTag__Binary:
			/* variable length values */
			if (con->buffer_curr + 3 > con->buffer_tail)
				elog(ERROR, "RecordBatch has less buffers than expected");
			buffer_curr = con->buffer_curr++;
			if (fstate->null_count > 0)
			{
				fstate->nullmap_offset = buffer_curr->offset;
				fstate->nullmap_length = buffer_curr->length;
				if (fstate->nullmap_length < BITMAPLEN(fstate->nitems))
					elog(ERROR, "nullmap length is smaller than expected");
				if ((fstate->nullmap_offset & (MAXIMUM_ALIGNOF - 1)) != 0 ||
					(fstate->nullmap_length & (MAXIMUM_ALIGNOF - 1)) != 0)
					elog(ERROR, "nullmap is not aligned well");
			}

			buffer_curr = con->buffer_curr++;
			fstate->values_offset = buffer_curr->offset;
			fstate->values_length = buffer_curr->length;
			if (fstate->values_length < arrowFieldLength(field,fstate->nitems))
				elog(ERROR, "offset array is smaller than expected");
			if ((fstate->values_offset & (MAXIMUM_ALIGNOF - 1)) != 0 ||
				(fstate->values_length & (MAXIMUM_ALIGNOF - 1)) != 0)
				elog(ERROR, "offset array is not aligned well");

			buffer_curr = con->buffer_curr++;
			fstate->extra_offset = buffer_curr->offset;
			fstate->extra_length = buffer_curr->length;
			if ((fstate->extra_offset & (MAXIMUM_ALIGNOF - 1)) != 0 ||
				(fstate->extra_length & (MAXIMUM_ALIGNOF - 1)) != 0)
				elog(ERROR, "extra buffer is not aligned well");
			break;

		case ArrowNodeTag__Struct:
			if (depth > 0)
				elog(ERROR, "nested composite type is not supported");
			/* only nullmap */
			if (con->buffer_curr + 1 > con->buffer_tail)
				elog(ERROR, "RecordBatch has less buffers than expected");
			buffer_curr = con->buffer_curr++;
			if (fstate->null_count > 0)
			{
				fstate->nullmap_offset = buffer_curr->offset;
				fstate->nullmap_length = buffer_curr->length;
				if (fstate->nullmap_length < BITMAPLEN(fstate->nitems))
					elog(ERROR, "nullmap length is smaller than expected");
				if ((fstate->nullmap_offset & (MAXIMUM_ALIGNOF - 1)) != 0 ||
					(fstate->nullmap_length & (MAXIMUM_ALIGNOF - 1)) != 0)
					elog(ERROR, "nullmap is not aligned well");
			}

			if (field->_num_children > 0)
			{
				int		i;

				fstate->children = palloc0(sizeof(RecordBatchFieldState) *
									  field->_num_children);
				for (i=0; i < field->_num_children; i++)
				{
					setupRecordBatchField(con,
										  &fstate->children[i],
										  &field->children[i],
										  depth+1);
				}
			}
			fstate->num_children = field->_num_children;
			break;
		default:
			elog(ERROR, "Bug? ArrowSchema contains unsupported types");
	}
	/* assign extra attributes (precision, unitsz, ...) */
	assignArrowTypeOptions(&fstate->attopts, &field->type);
}

static RecordBatchState *
makeRecordBatchState(ArrowSchema *schema,
					 ArrowBlock *block,
					 ArrowRecordBatch *rbatch)
{
	setupRecordBatchContext con;
	RecordBatchState *result;
	int			j, ncols = schema->_num_fields;

	result = palloc0(offsetof(RecordBatchState, columns[ncols]));
	result->ncols = ncols;
	result->rb_offset = block->offset + block->metaDataLength;
	result->rb_length = block->bodyLength;
	result->rb_nitems = rbatch->length;

	memset(&con, 0, sizeof(setupRecordBatchContext));
	con.buffer_curr = rbatch->buffers;
	con.buffer_tail = rbatch->buffers + rbatch->_num_buffers;
	con.fnode_curr  = rbatch->nodes;
	con.fnode_tail  = rbatch->nodes + rbatch->_num_nodes;

	for (j=0; j < ncols; j++)
	{
		RecordBatchFieldState *fstate = &result->columns[j];
		ArrowField	   *field = &schema->fields[j];

		setupRecordBatchField(&con, fstate, field, 0);
	}
	if (con.buffer_curr != con.buffer_tail ||
		con.fnode_curr  != con.fnode_tail)
		elog(ERROR, "arrow_fdw: RecordBatch may have corruption.");

	return result;
}

/*
 * ExecInitArrowFdw
 */
ArrowFdwState *
ExecInitArrowFdw(Relation relation, Bitmapset *outer_refs)
{
	TupleDesc		tupdesc = RelationGetDescr(relation);
	ForeignTable   *ft = GetForeignTable(RelationGetRelid(relation));
	List		   *filesList = arrowFdwExtractFilesList(ft->options);
	List		   *fdescList = NIL;
	Bitmapset	   *referenced = NULL;
	bool			whole_row_ref = false;
	ArrowFdwState  *af_state;
	List		   *rb_state_list = NIL;
	ListCell	   *lc;
	int				i, num_rbatches;

	Assert(RelationGetForm(relation)->relkind == RELKIND_FOREIGN_TABLE &&
		   memcmp(GetFdwRoutineForRelation(relation, false),
				  &pgstrom_arrow_fdw_routine, sizeof(FdwRoutine)) == 0);
	/* expand 'referenced' if it has whole-row reference */
	if (bms_is_member(-FirstLowInvalidHeapAttributeNumber, outer_refs))
		whole_row_ref = true;
	for (i=0; i < tupdesc->natts; i++)
	{
		Form_pg_attribute attr = tupleDescAttr(tupdesc, i);
		int		k = attr->attnum - FirstLowInvalidHeapAttributeNumber;

		if (attr->attisdropped)
			continue;
		if (whole_row_ref || bms_is_member(k, outer_refs))
			referenced = bms_add_member(referenced, k);
	}

	foreach (lc, filesList)
	{
		char	   *fname = strVal(lfirst(lc));
		File		fdesc;
		List	   *rb_cached = NIL;
		ListCell   *cell;

		fdesc = PathNameOpenFile(fname, O_RDONLY | PG_BINARY);
		if (fdesc < 0)
			elog(ERROR, "failed to open '%s'", fname);
		fdescList = lappend_int(fdescList, fdesc);

		rb_cached = arrowLookupOrBuildMetadataCache(fdesc);
		/* check schema compatibility */
		foreach (cell, rb_cached)
		{
			RecordBatchState   *rb_state = lfirst(cell);

			if (!arrowSchemaCompatibilityCheck(tupdesc, rb_state))
				elog(ERROR, "arrow file '%s' on behalf of foreign table '%s' has incompatible schema definition",
					 fname, RelationGetRelationName(relation));
		}
		rb_state_list = list_concat(rb_state_list, rb_cached);
	}
	num_rbatches = list_length(rb_state_list);
	af_state = palloc0(offsetof(ArrowFdwState, rbatches[num_rbatches]));
	af_state->fdescList = fdescList;
	af_state->referenced = referenced;
	af_state->rbatch_index = &af_state->__rbatch_index_local;
	i = 0;
	foreach (lc, rb_state_list)
		af_state->rbatches[i++] = (RecordBatchState *)lfirst(lc);
	af_state->num_rbatches = num_rbatches;

	return af_state;
}

/*
 * ArrowBeginForeignScan
 */
static void
ArrowBeginForeignScan(ForeignScanState *node, int eflags)
{
	Relation		relation = node->ss.ss_currentRelation;
	TupleDesc		tupdesc = RelationGetDescr(relation);
	ForeignScan	   *fscan = (ForeignScan *) node->ss.ps.plan;
	ListCell	   *lc;
	Bitmapset	   *referenced = NULL;

	foreach (lc, fscan->fdw_private)
	{
		int		j = lfirst_int(lc);

		if (j >= 0 && j <= tupdesc->natts)
			referenced = bms_add_member(referenced, j -
										FirstLowInvalidHeapAttributeNumber);
	}
	node->fdw_state = ExecInitArrowFdw(relation, referenced);
}

typedef struct
{
	off_t		rb_offset;
	off_t		f_offset;
	off_t		m_offset;
	cl_int		io_index;
	cl_int      depth;
	strom_io_chunk ioc[FLEXIBLE_ARRAY_MEMBER];
} arrowFdwSetupIOContext;

/*
 * arrowFdwSetupIOvectorField
 */
static inline void
__setupIOvectorField(arrowFdwSetupIOContext *con,
					 off_t chunk_offset,
					 size_t chunk_length,
					 cl_uint *p_cmeta_offset,
					 cl_uint *p_cmeta_length)
{
	off_t		f_pos = con->rb_offset + chunk_offset;

	if (f_pos == con->f_offset &&
		con->m_offset == MAXALIGN(con->m_offset))
	{
		/* good, buffer is continuous */
		*p_cmeta_offset = __kds_packed(con->m_offset);
		*p_cmeta_length = __kds_packed(chunk_length);

		con->m_offset += chunk_length;
		con->f_offset += chunk_length;
	}
	else
	{
		off_t		f_base = TYPEALIGN_DOWN(PAGE_SIZE, f_pos);
		off_t		f_tail;
		off_t		shift = f_pos - f_base;
		strom_io_chunk *ioc;

		if (con->io_index < 0)
			con->io_index = 0;	/* no previous i/o chunks */
		else
		{
			ioc = &con->ioc[con->io_index++];

			f_tail = TYPEALIGN(PAGE_SIZE, con->f_offset);
			ioc->nr_pages = f_tail / PAGE_SIZE - ioc->fchunk_id;
			con->m_offset += (f_tail - con->f_offset); //safety margin;
		}
		ioc = &con->ioc[con->io_index];
		/* adjust position if con->m_offset is not aligned well */
		if (con->m_offset + shift != MAXALIGN(con->m_offset + shift))
			con->m_offset = MAXALIGN(con->m_offset + shift) - shift;
		ioc->m_offset   = con->m_offset;
		ioc->fchunk_id  = f_base / PAGE_SIZE;

		*p_cmeta_offset = __kds_packed(con->m_offset + shift);
		*p_cmeta_length = __kds_packed(chunk_length);

		con->m_offset  += shift + chunk_length;
		con->f_offset   = f_pos + chunk_length;
	}
}

static void
arrowFdwSetupIOvectorField(arrowFdwSetupIOContext *con,
						   RecordBatchFieldState *fstate,
						   kern_data_store *kds,
						   kern_colmeta *cmeta)
{
	//int		index = cmeta - kds->colmeta;

	if (fstate->nullmap_length > 0)
	{
		Assert(fstate->null_count > 0);
		__setupIOvectorField(con,
							 fstate->nullmap_offset,
							 fstate->nullmap_length,
							 &cmeta->nullmap_offset,
							 &cmeta->nullmap_length);
		//elog(INFO, "D%d att[%d] nullmap=%lu,%lu m_offset=%lu f_offset=%lu", con->depth, index, fstate->nullmap_offset, fstate->nullmap_length, con->m_offset, con->f_offset);
	}
	if (fstate->values_length > 0)
	{
		__setupIOvectorField(con,
							 fstate->values_offset,
							 fstate->values_length,
							 &cmeta->values_offset,
							 &cmeta->values_length);
		//elog(INFO, "D%d att[%d] values=%lu,%lu m_offset=%lu f_offset=%lu", con->depth, index, fstate->values_offset, fstate->values_length, con->m_offset, con->f_offset);
	}
	if (fstate->extra_length > 0)
	{
		__setupIOvectorField(con,
							 fstate->extra_offset,
							 fstate->extra_length,
							 &cmeta->extra_offset,
							 &cmeta->extra_length);
		//elog(INFO, "D%d att[%d] extra=%lu,%lu m_offset=%lu f_offset=%lu", con->depth, index, fstate->extra_offset, fstate->extra_length, con->m_offset, con->f_offset);
	}

	/* nested sub-fields if composite types */
	if (cmeta->atttypkind == TYPE_KIND__ARRAY ||
		cmeta->atttypkind == TYPE_KIND__COMPOSITE)
	{
		kern_colmeta *subattr;
		int		j;

		Assert(fstate->num_children == cmeta->num_subattrs);
		con->depth++;
		for (j=0, subattr = &kds->colmeta[cmeta->idx_subattrs];
			 j < cmeta->num_subattrs;
			 j++, subattr++)
		{
			RecordBatchFieldState *child = &fstate->children[j];

			arrowFdwSetupIOvectorField(con, child, kds, subattr);
		}
		con->depth--;
	}
}

/*
 * arrowFdwSetupIOvector
 */
static strom_io_vector *
arrowFdwSetupIOvector(kern_data_store *kds,
					  RecordBatchState *rb_state,
					  Bitmapset *referenced)
{
	arrowFdwSetupIOContext *con;
	strom_io_vector *iovec = NULL;
	int			j, nr_chunks = 0;

	Assert(kds->nr_colmeta >= kds->ncols);
	con = alloca(offsetof(arrowFdwSetupIOContext,
						  ioc[3 * kds->nr_colmeta]));
	con->rb_offset = rb_state->rb_offset;
	con->f_offset  = ~0UL;	/* invalid offset */
	con->m_offset  = TYPEALIGN(PAGE_SIZE, KERN_DATA_STORE_HEAD_LENGTH(kds));
	con->io_index  = -1;
	for (j=0; j < kds->ncols; j++)
	{
		RecordBatchFieldState *fstate = &rb_state->columns[j];
		kern_colmeta *cmeta = &kds->colmeta[j];
		int			attidx = j + 1 - FirstLowInvalidHeapAttributeNumber;

		if (referenced && bms_is_member(attidx, referenced))
			arrowFdwSetupIOvectorField(con, fstate, kds, cmeta);
	}
	if (con->io_index >= 0)
	{
		/* close the last I/O chunks */
		strom_io_chunk *ioc = &con->ioc[con->io_index];

		ioc->nr_pages = (TYPEALIGN(PAGE_SIZE, con->f_offset) / PAGE_SIZE -
						 ioc->fchunk_id);
		con->m_offset = ioc->m_offset + PAGE_SIZE * ioc->nr_pages;
		nr_chunks = con->io_index + 1;
	}
	kds->length = con->m_offset;

	iovec = palloc0(offsetof(strom_io_vector, ioc[nr_chunks]));
	iovec->nr_chunks = nr_chunks;
	if (nr_chunks > 0)
		memcpy(iovec->ioc, con->ioc, sizeof(strom_io_chunk) * nr_chunks);
	return iovec;
}

/*
 * __dump_kds_and_iovec - just for debug
 */
static inline void
__dump_kds_and_iovec(kern_data_store *kds, strom_io_vector *iovec)
{
#if 0
	int		j;

	elog(INFO, "nchunks = %d", iovec->nr_chunks);
	for (j=0; j < iovec->nr_chunks; j++)
	{
		strom_io_chunk *ioc = &iovec->ioc[j];

		elog(INFO, "io[%d] [ m_offset=%lu, f_read=%lu...%lu, nr_pages=%u}",
			 j,
			 ioc->m_offset,
			 ioc->fchunk_id * PAGE_SIZE,
			 (ioc->fchunk_id + ioc->nr_pages) * PAGE_SIZE,
			 ioc->nr_pages);
	}

	elog(INFO, "kds {length=%zu nitems=%u typeid=%u typmod=%u table_oid=%u}",
		 kds->length, kds->nitems,
		 kds->tdtypeid, kds->tdtypmod, kds->table_oid);
	for (j=0; j < kds->nr_colmeta; j++)
	{
		kern_colmeta *cmeta = &kds->colmeta[j];

		elog(INFO, "%ccol[%d] nullmap=%lu,%lu values=%lu,%lu extra=%lu,%lu",
			 j < kds->ncols ? ' ' : '*', j,
			 __kds_unpack(cmeta->nullmap_offset),
			 __kds_unpack(cmeta->nullmap_length),
			 __kds_unpack(cmeta->values_offset),
			 __kds_unpack(cmeta->values_length),
			 __kds_unpack(cmeta->extra_offset),
			 __kds_unpack(cmeta->extra_length));

	}
#endif
}

/*
 * arrowFdwLoadRecordBatch
 */
static pgstrom_data_store *
__arrowFdwLoadRecordBatch(RecordBatchState *rb_state,
						  Relation relation,
						  Bitmapset *referenced,
						  GpuContext *gcontext,
						  MemoryContext mcontext,
						  int optimal_gpu)
{
	TupleDesc			tupdesc = RelationGetDescr(relation);
	pgstrom_data_store *pds;
	kern_data_store	   *kds;
	strom_io_vector	   *iovec;
	size_t				head_sz;
	int					j, fdesc;
	CUresult			rc;

	/* setup KDS and I/O-vector */
	head_sz = KDS_calculateHeadSize(tupdesc);
	kds = alloca(head_sz);
	init_kernel_data_store(kds, tupdesc, 0, KDS_FORMAT_ARROW, 0);
	kds->nitems = rb_state->rb_nitems;
	kds->nrooms = rb_state->rb_nitems;
	kds->table_oid = RelationGetRelid(relation);
	Assert(head_sz == KERN_DATA_STORE_HEAD_LENGTH(kds));
	for (j=0; j < kds->nr_colmeta; j++)
		kds->colmeta[j].attopts = rb_state->columns[j].attopts;
	iovec = arrowFdwSetupIOvector(kds, rb_state, referenced);
	__dump_kds_and_iovec(kds, iovec);

	fdesc = FileGetRawDesc(rb_state->fdesc);
	/*
	 * If SSD-to-GPU Direct SQL is available on the arrow file, setup a small
	 * PDS on host-pinned memory, with strom_io_vector.
	 */
	if (gcontext &&
		gcontext->cuda_dindex == optimal_gpu &&
		iovec->nr_chunks > 0 &&
		kds->length <= gpuMemAllocIOMapMaxLength())
	{
		size_t	iovec_sz = offsetof(strom_io_vector, ioc[iovec->nr_chunks]);

		rc = gpuMemAllocHost(gcontext, (void **)&pds,
							 offsetof(pgstrom_data_store, kds) +
							 head_sz + iovec_sz);
		if (rc != CUDA_SUCCESS)
			elog(ERROR, "failed on gpuMemAllocHost: %s", errorText(rc));

		pds->gcontext = gcontext;
		pg_atomic_init_u32(&pds->refcnt, 1);
		pds->nblocks_uncached = 0;
		pds->filedesc = fdesc;
		pds->iovec = (strom_io_vector *)((char *)&pds->kds + head_sz);
		memcpy(&pds->kds, kds, head_sz);
		memcpy(pds->iovec, iovec, iovec_sz);
	}
	else
	{
		/* Elsewhere, load RecordBatch by filesystem */
		if (gcontext)
		{
			rc = gpuMemAllocManaged(gcontext,
									(CUdeviceptr *)&pds,
									offsetof(pgstrom_data_store,
											 kds) + kds->length,
									CU_MEM_ATTACH_GLOBAL);
			if (rc != CUDA_SUCCESS)
				elog(ERROR, "failed on gpuMemAllocManaged: %s", errorText(rc));
		}
		else
		{
			pds = MemoryContextAllocHuge(mcontext,
										 offsetof(pgstrom_data_store,
												  kds) + kds->length);
		}
		__PDS_fillup_arrow(pds, gcontext, kds, fdesc, iovec);
	}
	pfree(iovec);
	return pds;
}

static pgstrom_data_store *
arrowFdwLoadRecordBatch(ArrowFdwState *af_state,
						Relation relation,
						EState *estate,
						GpuContext *gcontext,
						int optimal_gpu)
{
	uint32		rb_index;

	/* fetch next RecordBatch */
	rb_index = pg_atomic_fetch_add_u32(af_state->rbatch_index, 1);
	if (rb_index >= af_state->num_rbatches)
		return NULL;	/* no more RecordBatch to read */

	return __arrowFdwLoadRecordBatch(af_state->rbatches[rb_index],
									 relation,
									 af_state->referenced,
									 gcontext,
									 estate->es_query_cxt,
									 optimal_gpu);
}

/*
 * ExecScanChunkArrowFdw
 */
pgstrom_data_store *
ExecScanChunkArrowFdw(GpuTaskState *gts)
{
	pgstrom_data_store *pds;

	InstrStartNode(&gts->outer_instrument);
	pds = arrowFdwLoadRecordBatch(gts->af_state,
								  gts->css.ss.ss_currentRelation,
								  gts->css.ss.ps.state,
								  gts->gcontext,
								  gts->optimal_gpu);
	InstrStopNode(&gts->outer_instrument,
				  !pds ? 0.0 : (double)pds->kds.nitems);
	return pds;
}

/*
 * ArrowIterateForeignScan
 */
static TupleTableSlot *
ArrowIterateForeignScan(ForeignScanState *node)
{
	ArrowFdwState  *af_state = node->fdw_state;
	Relation		relation = node->ss.ss_currentRelation;
	TupleTableSlot *slot = node->ss.ss_ScanTupleSlot;
	pgstrom_data_store *pds;

	while ((pds = af_state->curr_pds) == NULL ||
		   af_state->curr_index >= pds->kds.nitems)
	{
		EState	   *estate = node->ss.ps.state;

		/* unload the previous RecordBatch, if any */
		if (pds)
			PDS_release(pds);
		af_state->curr_index = 0;
		af_state->curr_pds = arrowFdwLoadRecordBatch(af_state,
													 relation,
													 estate,
													 NULL, -1);
		if (!af_state->curr_pds)
			return NULL;
	}
	Assert(pds && af_state->curr_index < pds->kds.nitems);
	if (KDS_fetch_tuple_arrow(slot, &pds->kds, af_state->curr_index++))
		return slot;
	return NULL;
}

/*
 * ArrowReScanForeignScan
 */
void
ExecReScanArrowFdw(ArrowFdwState *af_state)
{
	/* rewind the current scan state */
	pg_atomic_write_u32(af_state->rbatch_index, 0);
	if (af_state->curr_pds)
		PDS_release(af_state->curr_pds);
	af_state->curr_pds = NULL;
	af_state->curr_index = 0;
}

static void
ArrowReScanForeignScan(ForeignScanState *node)
{
	ExecReScanArrowFdw((ArrowFdwState *)node->fdw_state);
}

/*
 * ArrowEndForeignScan
 */
void
ExecEndArrowFdw(ArrowFdwState *af_state)
{
	ListCell   *lc;

	foreach (lc, af_state->fdescList)
		FileClose((File)lfirst_int(lc));
}

static void
ArrowEndForeignScan(ForeignScanState *node)
{
	ExecEndArrowFdw((ArrowFdwState *)node->fdw_state);
}

/*
 * ArrowExplainForeignScan 
 */
void
ExplainArrowFdw(ArrowFdwState *af_state, Relation frel, ExplainState *es)
{
	TupleDesc	tupdesc = RelationGetDescr(frel);
	ListCell   *lc;
	int			fcount = 0;
	char		label[80];
	size_t	   *chunk_sz = alloca(sizeof(size_t) * tupdesc->natts);
	int			i, j, k;
	StringInfoData	buf;

	/* shows referenced columns */
	initStringInfo(&buf);
	for (k = bms_next_member(af_state->referenced, -1);
		 k >= 0;
		 k = bms_next_member(af_state->referenced, k))
	{
		j = k + FirstLowInvalidHeapAttributeNumber - 1;

		if (j >= 0)
		{
			Form_pg_attribute	attr = tupleDescAttr(tupdesc, j);
			const char		   *attName = NameStr(attr->attname);
			if (buf.len > 0)
				appendStringInfoString(&buf, ", ");
			appendStringInfoString(&buf, quote_identifier(attName));
		}
	}
	ExplainPropertyText("referenced", buf.data, es);

	/* shows files on behalf of the foreign table */
	foreach (lc, af_state->fdescList)
	{
		File		fdesc = (File)lfirst_int(lc);
		const char *fname = FilePathName(fdesc);
		int			rbcount = 0;
		char	   *pos = label;
		struct stat	st_buf;

		pos += snprintf(label, sizeof(label), "files%d", fcount++);
		if (fstat(FileGetRawDesc(fdesc), &st_buf) != 0)
			memset(&st_buf, 0, sizeof(struct stat));
		if (es->format == EXPLAIN_FORMAT_TEXT)
		{
			resetStringInfo(&buf);
			if (st_buf.st_size == 0)
				appendStringInfoString(&buf, fname);
			else
				appendStringInfo(&buf, "%s (size: %s)",
								 fname,
								 format_bytesz(st_buf.st_size));
			ExplainPropertyText(label, buf.data, es);
		}
		else
		{
			ExplainPropertyText(label, fname, es);

			if (st_buf.st_size > 0)
			{
				sprintf(pos, "-size");
				ExplainPropertyText(label, format_bytesz(st_buf.st_size), es);
			}
		}
		if (!es->verbose)
			continue;

		/* below only verbose mode */
		memset(chunk_sz, 0, sizeof(size_t) * tupdesc->natts);
		for (i=0; i < af_state->num_rbatches; i++)
		{
			RecordBatchState *rb_state = af_state->rbatches[i];

			if (rb_state->fdesc != fdesc)
				continue;

			for (k = bms_next_member(af_state->referenced, -1);
				 k >= 0;
				 k = bms_next_member(af_state->referenced, k))
			{
				j = k + FirstLowInvalidHeapAttributeNumber - 1;
				if (j < 0 || j >= tupdesc->natts)
					continue;
				chunk_sz[j] += RecordBatchFieldLength(&rb_state->columns[j]);
			}
			rbcount++;
		}

		if (rbcount >= 0)
		{
			for (k = bms_next_member(af_state->referenced, -1);
                 k >= 0;
                 k = bms_next_member(af_state->referenced, k))
            {
				Form_pg_attribute attr;

				j = k + FirstLowInvalidHeapAttributeNumber - 1;
				if (j < 0 || j >= tupdesc->natts)
					continue;
				attr = tupleDescAttr(tupdesc, j);
				snprintf(label, sizeof(label),
						 "  %s", NameStr(attr->attname));
				ExplainPropertyText(label, format_bytesz(chunk_sz[j]), es);
			}
		}
	}
}

static void
ArrowExplainForeignScan(ForeignScanState *node, ExplainState *es)
{
	Relation	frel = node->ss.ss_currentRelation;

	ExplainArrowFdw((ArrowFdwState *)node->fdw_state, frel, es);
}

/*
 * readArrowFile
 */
static void
readArrowFile(char *pathname, ArrowFileInfo *af_info)
{
    File	filp = PathNameOpenFile(pathname, O_RDONLY | PG_BINARY);

	if (filp < 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not open file \"%s\": %m", pathname)));
    readArrowFileDesc(FileGetRawDesc(filp), af_info);
    FileClose(filp);
}

/*
 * RecordBatchAcquireSampleRows - random sampling
 */
static int
RecordBatchAcquireSampleRows(Relation relation,
							 RecordBatchState *rb_state,
							 HeapTuple *rows,
							 int nsamples)
{
	TupleDesc		tupdesc = RelationGetDescr(relation);
	pgstrom_data_store *pds;
	Bitmapset	   *referenced = NULL;
	Datum		   *values;
	bool		   *isnull;
	int				count;
	int				i, j, nwords;

	/* ANALYZE needs to fetch all the attributes */
	nwords = (tupdesc->natts - FirstLowInvalidHeapAttributeNumber +
			  BITS_PER_BITMAPWORD - 1) / BITS_PER_BITMAPWORD;
	referenced = alloca(offsetof(Bitmapset, words[nwords]));
	referenced->nwords = nwords;
	memset(referenced->words, -1, sizeof(bitmapword) * nwords);
	
	pds = __arrowFdwLoadRecordBatch(rb_state,
									relation,
									referenced,
									NULL,
									CurrentMemoryContext,
									-1);
	values = alloca(sizeof(Datum) * tupdesc->natts);
	isnull = alloca(sizeof(bool)  * tupdesc->natts);
	for (count = 0; count < nsamples; count++)
	{
		/* fetch a row randomly */
		i = (double)pds->kds.nitems * (((double) random()) /
									   ((double)MAX_RANDOM_VALUE + 1));
		Assert(i < pds->kds.nitems);

		for (j=0; j < pds->kds.ncols; j++)
		{
			kern_colmeta   *cmeta = &pds->kds.colmeta[j];
			
			pg_datum_arrow_ref(&pds->kds,
							   cmeta,
							   i,
							   values + j,
							   isnull + j);
		}
		rows[count] = heap_form_tuple(tupdesc, values, isnull);
	}
	PDS_release(pds);

	return count;
}

/*
 * ArrowAcquireSampleRows
 */
static int
ArrowAcquireSampleRows(Relation relation,
					   int elevel,
					   HeapTuple *rows,
					   int nrooms,
					   double *p_totalrows,
					   double *p_totaldeadrows)
{
	TupleDesc		tupdesc = RelationGetDescr(relation);
	ForeignTable   *ft = GetForeignTable(RelationGetRelid(relation));
	List		   *filesList = arrowFdwExtractFilesList(ft->options);
	List		   *rb_state_list = NIL;
	ListCell	   *lc;
	File		   *fdesc_array;
	int64			total_nrows = 0;
	int64			count_nrows = 0;
	int				nsamples_min = nrooms / 100;
	int				nitems = 0;
	int				fcount = 0;

	fdesc_array = alloca(sizeof(File) * list_length(filesList));
	foreach (lc, filesList)
	{
		char	   *fname = strVal(lfirst(lc));
		File		fdesc;
		List	   *rb_cached;
		ListCell   *cell;

		fdesc = PathNameOpenFile(fname, O_RDONLY | PG_BINARY);
        if (fdesc < 0)
		{
			elog(NOTICE, "failed to open file '%s' on behalf of '%s', skipped",
				 fname, RelationGetRelationName(relation));
			continue;
		}
		fdesc_array[fcount++] = fdesc;
		
		rb_cached = arrowLookupOrBuildMetadataCache(fdesc);
		foreach (cell, rb_cached)
		{
			RecordBatchState *rb_state = lfirst(cell);

			if (!arrowSchemaCompatibilityCheck(tupdesc, rb_state))
				elog(ERROR, "arrow file '%s' on behalf of foreign table '%s' has incompatible schema definition",
					 fname, RelationGetRelationName(relation));
			if (rb_state->rb_nitems == 0)
				continue;	/* not reasonable to sample, skipped */
			total_nrows += rb_state->rb_nitems;

			rb_state_list = lappend(rb_state_list, rb_state);
		}
	}
	nrooms = Min(nrooms, total_nrows);

	/* fetch samples for each record-batch */
	foreach (lc, rb_state_list)
	{
		RecordBatchState *rb_state = lfirst(lc);
		int			nsamples;

		count_nrows += rb_state->rb_nitems;
		nsamples = (double)nrooms * ((double)count_nrows /
									 (double)total_nrows) - nitems;
		if (nitems + nsamples > nrooms)
			nsamples = nrooms - nitems;
		if (nsamples > nsamples_min)
			nitems += RecordBatchAcquireSampleRows(relation,
												   rb_state,
												   rows + nitems,
												   nsamples);
	}
	while (fcount > 0)
		FileClose(fdesc_array[--fcount]);

	*p_totalrows = total_nrows;
	*p_totaldeadrows = 0.0;

	return nitems;
}

/*
 * ArrowAnalyzeForeignTable
 */
static bool
ArrowAnalyzeForeignTable(Relation frel,
						 AcquireSampleRowsFunc *p_sample_rows_func,
						 BlockNumber *p_totalpages)
{
	ForeignTable   *ft = GetForeignTable(RelationGetRelid(frel));
	List		   *filesList = arrowFdwExtractFilesList(ft->options);
	ListCell	   *lc;
	Size			totalpages = 0;

	foreach (lc, filesList)
	{
		const char *fname = strVal(lfirst(lc));
		struct stat	statbuf;

		if (stat(fname, &statbuf) != 0)
		{
			elog(NOTICE, "failed on stat('%s') on behalf of '%s', skipped",
				 fname, get_rel_name(ft->relid));
			continue;
		}
		totalpages += (statbuf.st_size + BLCKSZ - 1) / BLCKSZ;
	}

	if (totalpages > MaxBlockNumber)
		totalpages = MaxBlockNumber;

	*p_sample_rows_func = ArrowAcquireSampleRows;
	*p_totalpages = totalpages;

	return true;
}

/*
 * ArrowImportForeignSchema
 */
static List *
ArrowImportForeignSchema(ImportForeignSchemaStmt *stmt, Oid serverOid)
{
	ArrowSchema	schema;
	List	   *filesList;
	ListCell   *lc;
	int			j;
	StringInfoData	cmd;

	/* sanity checks */
	switch (stmt->list_type)
	{
		case FDW_IMPORT_SCHEMA_ALL:
			break;
		case FDW_IMPORT_SCHEMA_LIMIT_TO:
			elog(ERROR, "arrow_fdw does not support LIMIT TO clause");
		case FDW_IMPORT_SCHEMA_EXCEPT:
			elog(ERROR, "arrow_fdw does not support EXCEPT clause");
		default:
			elog(ERROR, "arrow_fdw: Bug? unknown list-type");
	}
	filesList = arrowFdwExtractFilesList(stmt->options);
	if (filesList == NIL)
		ereport(ERROR,
				(errmsg("No valid apache arrow files are specified"),
				 errhint("Use 'file' or 'dir' option to specify apache arrow files on behalf of the foreign table")));

	/* read the schema */
	memset(&schema, 0, sizeof(ArrowSchema));
	foreach (lc, filesList)
	{
		char	   *fname = strVal(lfirst(lc));
		ArrowFileInfo af_info;

		readArrowFile(fname, &af_info);
		if (lc == list_head(filesList))
		{
			copyArrowNode(&schema.node, &af_info.footer.schema.node);
		}
		else
		{
			/* compatibility checks */
			ArrowSchema	   *stemp = &af_info.footer.schema;

			if (schema.endianness != stemp->endianness ||
				schema._num_fields != stemp->_num_fields)
				elog(ERROR, "file '%s' has incompatible schema definition",
					 fname);
			for (j=0; j < schema._num_fields; j++)
			{
				if (arrowTypeIsEqual(&schema.fields[j],
									 &stemp->fields[j], 0))
					elog(ERROR, "file '%s' has incompatible schema definition",
						 fname);
			}
		}
	}

	/* makes a command to define foreign table */
	initStringInfo(&cmd);
	appendStringInfo(&cmd, "CREATE FOREIGN TABLE %s (\n",
					 quote_identifier(stmt->remote_schema));
	for (j=0; j < schema._num_fields; j++)
	{
		ArrowField *field = &schema.fields[j];
		const char *type_name = arrowTypeToPGTypeName(field);

		if (j > 0)
			appendStringInfo(&cmd, ",\n");
		if (!field->name || field->_name_len == 0)
		{
			elog(NOTICE, "field %d has no name, so \"__col%02d\" is used",
				 j+1, j+1);
			appendStringInfo(&cmd, "  __col%02d  %s", j+1, type_name);
		}
		else
			appendStringInfo(&cmd, "  %s %s",
							 quote_identifier(field->name), type_name);
	}
	appendStringInfo(&cmd,
					 "\n"
					 ") SERVER %s\n"
					 "  OPTIONS (", stmt->server_name);
	foreach (lc, stmt->options)
	{
		DefElem	   *defel = lfirst(lc);

		if (lc != list_head(stmt->options))
			appendStringInfo(&cmd, ",\n           ");
		appendStringInfo(&cmd, "%s '%s'",
						 defel->defname,
						 strVal(defel->arg));
	}
	appendStringInfo(&cmd, ")");

	return list_make1(cmd.data);
}

/*
 * ArrowIsForeignScanParallelSafe
 */
static bool
ArrowIsForeignScanParallelSafe(PlannerInfo *root,
							   RelOptInfo *rel,
							   RangeTblEntry *rte)
{
	return true;
}

/*
 * ArrowEstimateDSMForeignScan 
 */
static Size
ArrowEstimateDSMForeignScan(ForeignScanState *node,
							ParallelContext *pcxt)
{
	//elog(INFO, "pid=%u ArrowEstimateDSMForeignScan", getpid());
	return MAXALIGN(sizeof(pg_atomic_uint32));
}

/*
 * ArrowInitializeDSMForeignScan
 */
void
ExecInitDSMArrowFdw(ArrowFdwState *af_state, pg_atomic_uint32 *rbatch_index)
{
	pg_atomic_init_u32(rbatch_index, 0);
	af_state->rbatch_index = rbatch_index;
}

static void
ArrowInitializeDSMForeignScan(ForeignScanState *node,
							  ParallelContext *pcxt,
							  void *coordinate)
{
	ExecInitDSMArrowFdw((ArrowFdwState *)node->fdw_state,
						(pg_atomic_uint32 *) coordinate);
}

/*
 * ArrowReInitializeDSMForeignScan
 */
void
ExecReInitDSMArrowFdw(ArrowFdwState *af_state)
{
	pg_atomic_write_u32(af_state->rbatch_index, 0);
}


static void
ArrowReInitializeDSMForeignScan(ForeignScanState *node,
								ParallelContext *pcxt,
								void *coordinate)
{
	ExecReInitDSMArrowFdw((ArrowFdwState *)node->fdw_state);
}

/*
 * ArrowInitializeWorkerForeignScan
 */
void
ExecInitWorkerArrowFdw(ArrowFdwState *af_state,
					   pg_atomic_uint32 *rbatch_index)
{
	af_state->rbatch_index = rbatch_index;
}

static void
ArrowInitializeWorkerForeignScan(ForeignScanState *node,
								 shm_toc *toc,
								 void *coordinate)
{
	ExecInitWorkerArrowFdw((ArrowFdwState *)node->fdw_state,
						   (pg_atomic_uint32 *) coordinate);
}

/*
 * ArrowShutdownForeignScan
 */
void
ExecShutdownArrowFdw(ArrowFdwState *af_state)
{
	/* nothing to do */
}

static void
ArrowShutdownForeignScan(ForeignScanState *node)
{
	ExecShutdownArrowFdw((ArrowFdwState *)node->fdw_state);
}

/*
 * ArrowPlanForeignModify
 */
static List *
ArrowPlanForeignModify(PlannerInfo *root,
					   ModifyTable *plan,
					   Index resultRelation,
					   int subplan_index)
{
	RangeTblEntry  *rte = planner_rt_fetch(resultRelation, root);
	ForeignTable   *ft = GetForeignTable(rte->relid);
	List		   *filesList;
	bool			writable;

	if (plan->operation != CMD_INSERT)
		elog(ERROR, "not a supported operation on arrow_fdw foreign tables");

	filesList = __arrowFdwExtractFilesList(ft->options,
										   NULL,
										   &writable,
										   NULL);
	if (!writable)
		elog(ERROR, "arrow_fdw: foreign table \"%s\" is not writable",
			 get_rel_name(rte->relid));
	Assert(list_length(filesList) == 1);

	return NIL;
}

/*
 * ArrowBeginForeignModify
 */
static void
ArrowBeginForeignModify(ModifyTableState *mtstate,
						ResultRelInfo *rrinfo,
						List *fdw_private,
						int subplan_index,
						int eflags)
{
	Relation		frel = rrinfo->ri_RelationDesc;
	ForeignTable   *ft = GetForeignTable(RelationGetRelid(frel));
	List		   *filesList = arrowFdwExtractFilesList(ft->options);
	const char	   *filename;
	File			filp;

	Assert(list_length(filesList) == 1);
	filename = strVal(linitial(filesList));

	LockRelation(frel, ShareRowExclusiveLock);

	filp = PathNameOpenFile(filename, O_RDWR | PG_BINARY);
	if (filp < 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not open file \"%s\": %m", filename)));
	rrinfo->ri_FdwState = createArrowWriteState(frel, filp);
}

/*
 * ArrowExecForeignInsert
 */
static TupleTableSlot *
ArrowExecForeignInsert(EState *estate,
					   ResultRelInfo *rrinfo,
					   TupleTableSlot *slot,
					   TupleTableSlot *planSlot)
{
	Relation		frel = rrinfo->ri_RelationDesc;
	TupleDesc		tupdesc = RelationGetDescr(frel);
	arrowWriteState *aw_state = rrinfo->ri_FdwState;
	SQLtable	   *table = &aw_state->sql_table;
	MemoryContext	oldcxt;
	size_t			usage = 0;
	int				j;

	slot_getallattrs(slot);
	oldcxt = MemoryContextSwitchTo(aw_state->memcxt);
	for (j=0; j < tupdesc->natts; j++)
	{
		Form_pg_attribute attr = tupleDescAttr(tupdesc, j);
		SQLfield   *column = &table->columns[j];
		Datum		datum = slot->tts_values[j];
		bool		isnull = slot->tts_isnull[j];

		if (isnull)
		{
			usage += sql_field_put_value(column, NULL, 0);
		}
		else if (attr->attbyval)
		{
			Assert(column->sql_type.pgsql.typbyval);
			usage += sql_field_put_value(column, (char *)&datum, attr->attlen);
		}
		else if (attr->attlen == -1)
		{
			int		vl_len = VARSIZE_ANY_EXHDR(datum);
			char   *vl_ptr = VARDATA_ANY(datum);

			Assert(column->sql_type.pgsql.typlen == -1);
			usage += sql_field_put_value(column, vl_ptr, vl_len);
		}
		else
		{
			elog(ERROR, "Bug? unsupported type format");
		}
	}
	table->nitems++;
	MemoryContextSwitchTo(oldcxt);

	/*
	 * If usage exceeds the threshold of record-batch size, make a redo-log
	 * on demand, and write out the buffer.
	 */
	if (usage > table->segment_sz)
		writeOutArrowRecordBatch(aw_state, false);

	return slot;
}

/*
 * ArrowEndForeignModify
 */
static void
ArrowEndForeignModify(EState *estate,
					  ResultRelInfo *rrinfo)
{
	arrowWriteState *aw_state = rrinfo->ri_FdwState;

	writeOutArrowRecordBatch(aw_state, true);
}

/*
 * ArrowExplainForeignModify
 */
static void
ArrowExplainForeignModify(ModifyTableState *mtstate,
						  ResultRelInfo *rinfo,
						  List *fdw_private,
						  int subplan_index,
						  struct ExplainState *es)
{
	/* print something */
}

/*
 * handler of Arrow_Fdw
 */
Datum
pgstrom_arrow_fdw_handler(PG_FUNCTION_ARGS)
{
	PG_RETURN_POINTER(&pgstrom_arrow_fdw_routine);
}
PG_FUNCTION_INFO_V1(pgstrom_arrow_fdw_handler);

/*
 * arrowTypeIsEqual
 */
static bool
arrowTypeIsEqual(ArrowField *a, ArrowField *b, int depth)
{
	int		j;

	if (a->type.node.tag != b->type.node.tag)
		return false;
	switch (a->type.node.tag)
	{
		case ArrowNodeTag__Int:
			if (a->type.Int.bitWidth != b->type.Int.bitWidth)
				return false;
			break;
		case ArrowNodeTag__FloatingPoint:
			{
				ArrowPrecision	p1 = a->type.FloatingPoint.precision;
				ArrowPrecision	p2 = b->type.FloatingPoint.precision;

				if (p1 != p2)
					return false;
			}
			break;
		case ArrowNodeTag__Utf8:
		case ArrowNodeTag__Binary:
		case ArrowNodeTag__Bool:
			break;

		case ArrowNodeTag__Decimal:
			if (a->type.Decimal.precision != b->type.Decimal.precision ||
				a->type.Decimal.scale != b->type.Decimal.scale)
				return false;
			break;

		case ArrowNodeTag__Date:
			if (a->type.Date.unit != b->type.Date.unit)
				return false;
			break;

		case ArrowNodeTag__Time:
			if (a->type.Time.unit != b->type.Time.unit)
				return false;
			break;

		case ArrowNodeTag__Timestamp:
			if (a->type.Timestamp.unit != b->type.Timestamp.unit ||
				a->type.Timestamp.timezone != NULL ||
				b->type.Timestamp.timezone != NULL)
				return false;
			break;

		case ArrowNodeTag__Interval:
			if (a->type.Interval.unit != b->type.Interval.unit)
				return false;
			break;

		case ArrowNodeTag__Struct:
			if (depth > 0)
				elog(ERROR, "arrow: nested composite types are not supported");
			if (a->_num_children != b->_num_children)
				return false;
			for (j=0; j < a->_num_children; j++)
			{
				if (!arrowTypeIsEqual(&a->children[j],
									  &b->children[j],
									  depth + 1))
					return false;
			}
			break;

		case ArrowNodeTag__List:
			if (depth > 0)
				elog(ERROR, "arrow_fdw: nested array types are not supported");
			if (a->_num_children != 1 || b->_num_children != 1)
				elog(ERROR, "Bug? List of arrow type is corrupted.");
			if (!arrowTypeIsEqual(&a->children[0],
								  &b->children[0],
								  depth + 1))
				return false;
			break;

		default:
			elog(ERROR, "'%s' of arrow type is not supported",
				 a->type.node.tagName);
	}
	return true;
}

static Oid
arrowTypeToPGTypeOid(ArrowField *field, int *typmod)
{
	ArrowType  *t = &field->type;

	*typmod = -1;
	switch (t->node.tag)
	{
		case ArrowNodeTag__Int:
			switch (t->Int.bitWidth)
			{
				case 16:
					return INT2OID;
				case 32:
					return INT4OID;
				case 64:
					return INT8OID;
				default:
					elog(ERROR, "Arrow.%s is not supported",
						 arrowTypeName(field));
					break;
			}
			break;
		case ArrowNodeTag__FloatingPoint:
			switch (t->FloatingPoint.precision)
			{
				case ArrowPrecision__Half:
					return FLOAT2OID;
				case ArrowPrecision__Single:
					return FLOAT4OID;
				case ArrowPrecision__Double:
					return FLOAT8OID;
				default:
					elog(ERROR, "Arrow.%s is not supported",
						 arrowTypeName(field));
			}
			break;
		case ArrowNodeTag__Utf8:
			return TEXTOID;
		case ArrowNodeTag__Binary:
			return BYTEAOID;
		case ArrowNodeTag__Bool:
			return BOOLOID;
		case ArrowNodeTag__Decimal:
			return NUMERICOID;
		case ArrowNodeTag__Date:
			return DATEOID;
		case ArrowNodeTag__Time:
			return TIMEOID;
		case ArrowNodeTag__Timestamp:
			if (t->Timestamp.timezone)
				elog(ERROR, "Timestamp with timezone is not supported");
			return TIMESTAMPOID;
		case ArrowNodeTag__Interval:
			return INTERVALOID;
		case ArrowNodeTag__List:
			if (field->_num_children != 1)
				elog(ERROR, "arrow_fdw: corrupted List type definition");
			else
			{
				ArrowField *child = &field->children[0];
				Oid		type_oid;
				Oid		elem_oid;
				int		elem_mod;

				elem_oid = arrowTypeToPGTypeOid(child, &elem_mod);
				type_oid = get_array_type(elem_oid);
				if (!OidIsValid(type_oid))
					elog(ERROR, "array of arrow::%s type is not defined",
						 arrowTypeName(field));
				return type_oid;
			}
			break;

		case ArrowNodeTag__Struct:
			{
				Relation	rel;
				ScanKeyData	skey[2];
				SysScanDesc	sscan;
				HeapTuple	tup;
				Oid			type_oid = InvalidOid;

				/*
				 * lookup composite type definition from pg_class
				 * At least, nattrs == _num_children
				 */
				rel = table_open(RelationRelationId, AccessShareLock);
				ScanKeyInit(&skey[0],
							Anum_pg_class_relkind,
							BTEqualStrategyNumber, F_CHAREQ,
							CharGetDatum(RELKIND_COMPOSITE_TYPE));
				ScanKeyInit(&skey[1],
							Anum_pg_class_relnatts,
							BTEqualStrategyNumber, F_INT2EQ,
							Int16GetDatum(field->_num_children));

				sscan = systable_beginscan(rel, InvalidOid, false,
										   NULL, 2, skey);
				while (!OidIsValid(type_oid) &&
					   HeapTupleIsValid(tup = systable_getnext(sscan)))
				{
					Form_pg_class relForm = (Form_pg_class) GETSTRUCT(tup);
					TupleDesc	tupdesc;
					int			j;
					bool		compatible = true;

					if (pg_namespace_aclcheck(relForm->relnamespace,
											  GetUserId(),
											  ACL_USAGE) != ACLCHECK_OK)
						continue;

					if (pg_type_aclcheck(relForm->reltype,
										 GetUserId(),
										 ACL_USAGE) != ACLCHECK_OK)
						continue;

					tupdesc = lookup_rowtype_tupdesc(relForm->reltype, -1);
					Assert(tupdesc->natts == field->_num_children);
					for (j=0; compatible && j < tupdesc->natts; j++)
					{
						Form_pg_attribute attr = tupleDescAttr(tupdesc, j);
						ArrowField  *child = &field->children[j];
						Oid			typoid;
						int			typmod;

						typoid = arrowTypeToPGTypeOid(child, &typmod);
						if (typoid != attr->atttypid)
							compatible = false;
					}
					ReleaseTupleDesc(tupdesc);

					if (compatible)
						type_oid = relForm->reltype;
				}
				systable_endscan(sscan);
				table_close(rel, AccessShareLock);

				if (!OidIsValid(type_oid))
					elog(ERROR, "arrow::%s is not supported",
						 arrowTypeName(field));
				return type_oid;
			}
			break;
		case ArrowNodeTag__FixedSizeBinary:
			if (t->FixedSizeBinary.byteWidth < 1 ||
				t->FixedSizeBinary.byteWidth > BLCKSZ)
				elog(ERROR, "arrow_fdw: %s with byteWidth=%d is not supported",
					 t->node.tagName,
					 t->FixedSizeBinary.byteWidth);
			*typmod = t->FixedSizeBinary.byteWidth;
			return BPCHAROID;
		default:
			elog(ERROR, "arrow_fdw: type '%s' is not supported",
				 field->type.node.tagName);
	}
	return InvalidOid;
}

static const char *
arrowTypeToPGTypeName(ArrowField *field)
{
	Oid			typoid;
	int			typmod;
	HeapTuple	tup;
	Form_pg_type type;
	char	   *schema;
	char	   *result;

	typoid = arrowTypeToPGTypeOid(field, &typmod);
	if (!OidIsValid(typoid))
		return NULL;
	tup = SearchSysCache1(TYPEOID, ObjectIdGetDatum(typoid));
	if (!HeapTupleIsValid(tup))
		elog(ERROR, "cache lookup failed for type %u", typoid);
	type = (Form_pg_type) GETSTRUCT(tup);
	schema = get_namespace_name(type->typnamespace);
	if (typmod < 0)
		result = psprintf("%s.%s",
						  quote_identifier(schema),
						  quote_identifier(NameStr(type->typname)));
	else
		result = psprintf("%s.%s(%d)",
						  quote_identifier(schema),
						  quote_identifier(NameStr(type->typname)),
						  typmod);
	ReleaseSysCache(tup);

	return result;
}

/*
 * arrowTypeIsConvertible
 */
static bool
arrowTypeIsConvertible(Oid type_oid, int typemod)
{
	HeapTuple		tup;
	Form_pg_type	typeForm;
	bool			retval = false;

	switch (type_oid)
	{
		case INT2OID:		/* Int16 */
		case INT4OID:		/* Int32 */
		case INT8OID:		/* Int64 */
		case FLOAT2OID:		/* FP16 */
		case FLOAT4OID:		/* FP32 */
		case FLOAT8OID:		/* FP64 */
		case TEXTOID:		/* Utf8 */
		case BYTEAOID:		/* Binary */
		case BOOLOID:		/* Bool */
		case NUMERICOID:	/* Decimal */
		case DATEOID:		/* Date */
		case TIMEOID:		/* Time */
		case TIMESTAMPOID:	/* Timestamp */
		case INTERVALOID:	/* Interval */
		case BPCHAROID:		/* FixedSizeBinary */
			return true;
		default:
			tup = SearchSysCache1(TYPEOID, ObjectIdGetDatum(type_oid));
			if (!HeapTupleIsValid(tup))
				elog(ERROR, "cache lookup failed for type %u", type_oid);
			typeForm = (Form_pg_type) GETSTRUCT(tup);

			if (typeForm->typlen == -1 && OidIsValid(typeForm->typelem))
			{
				retval = arrowTypeIsConvertible(typeForm->typelem, typemod);
			}
			else if (typeForm->typtype == TYPTYPE_COMPOSITE)
			{
				Relation	rel;
				TupleDesc	tupdesc;
				int			j;

				rel = relation_open(typeForm->typrelid, AccessShareLock);
				tupdesc = RelationGetDescr(rel);
				for (j=0; j < tupdesc->natts; j++)
				{
					Form_pg_attribute	attr = tupleDescAttr(tupdesc, j);

					if (!arrowTypeIsConvertible(attr->atttypid,
												attr->atttypmod))
						break;
				}
				if (j >= tupdesc->natts)
					retval = true;
				relation_close(rel, AccessShareLock);
			}
			ReleaseSysCache(tup);
	}
	return retval;
}

/*
 * arrowFieldLength
 */
static size_t
arrowFieldLength(ArrowField *field, int64 nitems)
{
	ArrowType  *type = &field->type;
	size_t		length = 0;

	switch (type->node.tag)
	{
		case ArrowNodeTag__Int:
			switch (type->Int.bitWidth)
			{
				case sizeof(cl_short) * BITS_PER_BYTE:
					length = sizeof(cl_short) * nitems;
					break;
				case sizeof(cl_int) * BITS_PER_BYTE:
					length = sizeof(cl_int) * nitems;
					break;
				case sizeof(cl_long) * BITS_PER_BYTE:
					length = sizeof(cl_long) * nitems;
					break;
				default:
					elog(ERROR, "Not a supported Int width: %d",
						 type->Int.bitWidth);
			}
			break;
		case ArrowNodeTag__FloatingPoint:
			switch (type->FloatingPoint.precision)
			{
				case ArrowPrecision__Half:
					length = sizeof(cl_short) * nitems;
					break;
				case ArrowPrecision__Single:
					length = sizeof(cl_float) * nitems;
					break;
				case ArrowPrecision__Double:
					length = sizeof(cl_double) * nitems;
					break;
				default:
					elog(ERROR, "Not a supported FloatingPoint precision");
			}
			break;
		case ArrowNodeTag__Utf8:
		case ArrowNodeTag__Binary:
		case ArrowNodeTag__List:
			length = sizeof(cl_uint) * (nitems + 1);
			break;
		case ArrowNodeTag__Bool:
			length = BITMAPLEN(nitems);
			break;
		case ArrowNodeTag__Decimal:
			length = sizeof(int128) * nitems;
			break;
		case ArrowNodeTag__Date:
			switch (type->Date.unit)
			{
				case ArrowDateUnit__Day:
					length = sizeof(cl_int) * nitems;
					break;
				case ArrowDateUnit__MilliSecond:
					length = sizeof(cl_long) * nitems;
					break;
				default:
					elog(ERROR, "Not a supported Date unit");
			}
			break;
		case ArrowNodeTag__Time:
			switch (type->Time.unit)
			{
				case ArrowTimeUnit__Second:
				case ArrowTimeUnit__MilliSecond:
					length = sizeof(cl_int) * nitems;
					break;
				case ArrowTimeUnit__MicroSecond:
				case ArrowTimeUnit__NanoSecond:
					length = sizeof(cl_long) * nitems;
					break;
				default:
					elog(ERROR, "Not a supported Time unit");
			}
			break;
		case ArrowNodeTag__Timestamp:
			length = sizeof(cl_long) * nitems;
			break;
		case ArrowNodeTag__Interval:
			switch (type->Interval.unit)
			{
				case ArrowIntervalUnit__Year_Month:
					length = sizeof(cl_uint) * nitems;
					break;
				case ArrowIntervalUnit__Day_Time:
					length = sizeof(cl_long) * nitems;
					break;
				default:
					elog(ERROR, "Not a supported Interval unit");
			}
			break;
		case ArrowNodeTag__Struct:	//to be supported later
			length = 0;		/* only nullmap */
			break;
		case ArrowNodeTag__FixedSizeBinary:
			length = (size_t)type->FixedSizeBinary.byteWidth * nitems;
			break;
		default:
			elog(ERROR, "Arrow Type '%s' is not supported now",
				 type->node.tagName);
			break;
	}
	return length;
}

/*
 * arrowSchemaCompatibilityCheck
 */
static bool
__arrowSchemaCompatibilityCheck(TupleDesc tupdesc,
								RecordBatchFieldState *rb_fstate)
{
	int		j;

	for (j=0; j < tupdesc->natts; j++)
	{
		RecordBatchFieldState *fstate = &rb_fstate[j];
		Form_pg_attribute attr = tupleDescAttr(tupdesc, j);

		if (!fstate->children)
		{
			/* shortcur, it should be a scalar built-in type */
			Assert(fstate->num_children == 0);
			if (attr->atttypid != fstate->atttypid)
				return false;
		}
		else
		{
			Form_pg_type	typ;
			HeapTuple		tup;
			bool			type_is_ok = true;

			tup = SearchSysCache1(TYPEOID, ObjectIdGetDatum(attr->atttypid));
			if (!HeapTupleIsValid(tup))
				elog(ERROR, "cache lookup failed for type %u", attr->atttypid);
			typ = (Form_pg_type) GETSTRUCT(tup);
			if (typ->typlen == -1 && OidIsValid(typ->typelem) &&
				fstate->num_children == 1)
			{
				/* Arrow::List */
				RecordBatchFieldState *cstate = &fstate->children[0];

				if (attr->atttypid != cstate->atttypid)
					type_is_ok = false;
				else
				{
					/*
					 * overwrite typoid / typmod because a same arrow file
					 * can be reused, and it may be on behalf of different
					 * user defined data type.
					 */
					fstate->atttypid = attr->atttypid;
					fstate->atttypmod = attr->atttypmod;
				}
			}
			else if (typ->typlen == -1 && OidIsValid(typ->typrelid))
			{
				/* Arrow::Struct */
				TupleDesc	sdesc = lookup_rowtype_tupdesc(attr->atttypid,
														   attr->atttypmod);
				if (sdesc->natts == fstate->num_children &&
					__arrowSchemaCompatibilityCheck(sdesc, fstate->children))
				{
					/* see comment above */
					fstate->atttypid = attr->atttypid;
					fstate->atttypmod = attr->atttypmod;
				}
				else
				{
					type_is_ok = false;
				}
				DecrTupleDescRefCount(sdesc);

			}
			else
			{
				/* unknown */
				type_is_ok = false;
			}
			ReleaseSysCache(tup);
			if (!type_is_ok)
				return false;
		}
	}
	return true;
}

static bool
arrowSchemaCompatibilityCheck(TupleDesc tupdesc, RecordBatchState *rb_state)
{
	if (tupdesc->natts != rb_state->ncols)
		return false;
	return __arrowSchemaCompatibilityCheck(tupdesc, rb_state->columns);
}

/*
 * pg_XXX_arrow_ref
 */
static Datum
pg_varlena_arrow_ref(kern_data_store *kds,
					 kern_colmeta *cmeta, size_t index)
{
	cl_uint	   *offset = (cl_uint *)
		((char *)kds + __kds_unpack(cmeta->values_offset));
	char	   *extra = (char *)kds + __kds_unpack(cmeta->extra_offset);
	size_t		extra_len = __kds_unpack(cmeta->extra_length);
	cl_uint		len = offset[index+1] - offset[index];
	struct varlena *res;

	if (offset[index] > offset[index + 1] || offset[index+1] > extra_len)
		elog(ERROR, "corrupted arrow file? offset points out of extra buffer");

	res = palloc(VARHDRSZ + len);
	SET_VARSIZE(res, VARHDRSZ + len);
	memcpy(VARDATA(res), extra + offset[index], len);

	return PointerGetDatum(res);
}

static Datum
pg_bpchar_arrow_ref(kern_data_store *kds,
					kern_colmeta *cmeta, size_t index)
{
	cl_char	   *values = ((char *)kds + __kds_unpack(cmeta->values_offset));
	cl_int		unitsz = cmeta->atttypmod - VARHDRSZ;
	struct varlena *res;

	if (unitsz <= 0)
		elog(ERROR, "CHAR(%d) is not expected", unitsz);
	res = palloc(VARHDRSZ + unitsz);
	memcpy((char *)res + VARHDRSZ, values + unitsz * index, unitsz);
	SET_VARSIZE(res, VARHDRSZ + unitsz);

	return PointerGetDatum(res);
}

static Datum
pg_bool_arrow_ref(kern_data_store *kds,
				  kern_colmeta *cmeta, size_t index)
{
	char   *bitmap = (char *)kds + __kds_unpack(cmeta->values_offset);

	return BoolGetDatum(att_isnull(index, bitmap));
}

static Datum
pg_int2_arrow_ref(kern_data_store *kds,
				  kern_colmeta *cmeta, size_t index)
{
	cl_short   *values = (cl_short *)
		((char *)kds + __kds_unpack(cmeta->values_offset));
	return values[index];
}

static Datum
pg_int4_arrow_ref(kern_data_store *kds,
				  kern_colmeta *cmeta, size_t index)
{
	cl_int	   *values = (cl_int *)
		((char *)kds + __kds_unpack(cmeta->values_offset));
	return values[index];
}

static Datum
pg_int8_arrow_ref(kern_data_store *kds,
				  kern_colmeta *cmeta, size_t index)
{
	cl_long	   *values = (cl_long *)
		((char *)kds + __kds_unpack(cmeta->values_offset));
	return values[index];
}

static Datum
pg_numeric_arrow_ref(kern_data_store *kds,
					 kern_colmeta *cmeta, size_t index)
{
	char	   *result = palloc0(sizeof(struct NumericData));
	char	   *base = (char *)kds + __kds_unpack(cmeta->values_offset);
	int			dscale = cmeta->attopts.decimal.scale;
	Int128_t	decimal;

	decimal.ival = ((int128 *)base)[index];

	while (dscale > 0 && decimal.ival % 10 == 0)
	{
		decimal.ival /= 10;
		dscale--;
	}
	pg_numeric_to_varlena(result, dscale, decimal);

	return PointerGetDatum(result);
}

static Datum
pg_date_arrow_ref(kern_data_store *kds,
				  kern_colmeta *cmeta, size_t index)
{
	char	   *base = (char *)kds + __kds_unpack(cmeta->values_offset);
	DateADT		dt;

	switch (cmeta->attopts.date.unit)
	{
		case ArrowDateUnit__Day:
			dt = ((cl_uint *)base)[index];
			break;
		case ArrowDateUnit__MilliSecond:
			dt = ((cl_ulong *)base)[index] / 1000;
			break;
		default:
			elog(ERROR, "Bug? unexpected unit of Date type");
	}
	/* convert UNIX epoch to PostgreSQL epoch */
	dt -= (POSTGRES_EPOCH_JDATE - UNIX_EPOCH_JDATE);
	return DateADTGetDatum(dt);
}

static Datum
pg_time_arrow_ref(kern_data_store *kds,
				  kern_colmeta *cmeta, size_t index)
{
	char	   *base = (char *)kds + __kds_unpack(cmeta->values_offset);
	TimeADT		tm;

	switch (cmeta->attopts.time.unit)
	{
		case ArrowTimeUnit__Second:
			tm = ((cl_long)((cl_int *)base)[index]) * 1000000L;
			break;
		case ArrowTimeUnit__MilliSecond:
			tm = ((cl_long)((cl_int *)base)[index]) * 1000L;
			break;
		case ArrowTimeUnit__MicroSecond:
			tm = ((cl_long *)base)[index];
			break;
		case ArrowTimeUnit__NanoSecond:
			tm = ((cl_ulong *)base)[index] / 1000L;
			break;
		default:
			elog(ERROR, "Bug? unexpected unit of Time type");
	}
	return TimeADTGetDatum(tm);
}

static Datum
pg_timestamp_arrow_ref(kern_data_store *kds,
					   kern_colmeta *cmeta, size_t index)
{
	char	   *base = (char *)kds + __kds_unpack(cmeta->values_offset);
	Timestamp	ts;

	switch (cmeta->attopts.time.unit)
	{
		case ArrowTimeUnit__Second:
			ts = ((cl_ulong *)base)[index] * 1000000UL;
			break;
		case ArrowTimeUnit__MilliSecond:
			ts = ((cl_ulong *)base)[index] * 1000UL;
			break;
		case ArrowTimeUnit__MicroSecond:
			ts = ((cl_ulong *)base)[index];
			break;
		case ArrowTimeUnit__NanoSecond:
			ts = ((cl_ulong *)base)[index] / 1000UL;
			break;
		default:
			elog(ERROR, "Bug? unexpected unit of Timestamp type");
	}
	/* convert UNIX epoch to PostgreSQL epoch */
	ts -= (POSTGRES_EPOCH_JDATE -
		   UNIX_EPOCH_JDATE) * USECS_PER_DAY;
	return TimestampGetDatum(ts);
}

static Datum
pg_interval_arrow_ref(kern_data_store *kds,
					  kern_colmeta *cmeta, size_t index)
{
	char	   *base = (char *)kds + __kds_unpack(cmeta->values_offset);
	Interval   *iv = palloc0(sizeof(Interval));

	switch (cmeta->attopts.interval.unit)
	{
		case ArrowIntervalUnit__Year_Month:
			/* 32bit: number of months */
			iv->month = ((cl_uint *)base)[index];
			break;
		case ArrowIntervalUnit__Day_Time:
			/* 32bit+32bit: number of days and milliseconds */
			iv->day = (int32)(((cl_uint *)base)[2 * index]);
			iv->time = (TimeOffset)(((cl_uint *)base)[2 * index + 1]) * 1000;
			break;
		default:
			elog(ERROR, "Bug? unexpected unit of Interval type");
	}
	return PointerGetDatum(iv);
}

static Datum
pg_array_arrow_ref(kern_data_store *kds,
				   kern_colmeta *smeta,
				   cl_uint start, cl_uint end)
{
	ArrayType  *res;
	size_t		sz;
	cl_uint		i, nitems = end - start;
	cl_uint	   *offset;
	bits8	   *nullmap = NULL;
	char	   *base;
	size_t		usage;

	/* allocation of the result buffer */
	if (smeta->nullmap_offset != 0)
		sz = ARR_OVERHEAD_WITHNULLS(1, nitems);
	else
		sz = ARR_OVERHEAD_NONULLS(1);

	if (smeta->attlen > 0)
	{
		sz += TYPEALIGN(smeta->attalign,
						smeta->attlen) * nitems;
	}
	else if (smeta->attlen == -1)
	{
		if (smeta->values_offset == 0)
			elog(ERROR, "Bug? corrupted kernel column metadata");
		offset = (cl_uint *)((char *)kds + __kds_unpack(smeta->values_offset));
		sz += (MAXALIGN(VARHDRSZ * nitems) +		/* space for varlena */
			   MAXALIGN(sizeof(cl_uint) * nitems) +	/* space for alignment */
			   offset[end] - offset[start]);
	}
	else
		elog(ERROR, "Bug? corrupted kernel column metadata");

	res = palloc0(sz);
	res->ndim = 1;
	if (smeta->nullmap_offset != 0)
	{
		res->dataoffset = ARR_OVERHEAD_WITHNULLS(1, nitems);
		nullmap = ARR_NULLBITMAP(res);
	}
	res->elemtype = smeta->atttypid;
	ARR_DIMS(res)[0] = nitems;
	ARR_LBOUND(res)[0] = 1;

	base = ARR_DATA_PTR(res);
	usage = 0;
	for (i=0; i < nitems; i++)
	{
		Datum	datum;
		bool	isnull;

		pg_datum_arrow_ref(kds, smeta, start+i, &datum, &isnull);
		if (isnull)
		{
			if (!nullmap)
				elog(ERROR, "Bug? element item should not be NULL");
		}
		else if (smeta->attlen > 0)
		{
			if (nullmap)
				nullmap[i>>3] |= (1<<(i&7));
			usage = TYPEALIGN(smeta->attalign, usage);
			memcpy(base + usage, &datum, smeta->attlen);
			usage += smeta->attlen;
		}
		else if (smeta->attlen == -1)
		{
			cl_int		vl_len = VARSIZE(datum);

			if (nullmap)
				nullmap[i>>3] |= (1<<(i&7));
			usage = TYPEALIGN(smeta->attalign, usage);
			memcpy(base + usage, DatumGetPointer(datum), vl_len);
			usage += vl_len;

			pfree(DatumGetPointer(datum));
		}
		else
			elog(ERROR, "Bug? corrupted kernel column metadata");
	}
	SET_VARSIZE(res, (base + usage) - (char *)res);

	return PointerGetDatum(res);
}

/*
 * pg_datum_arrow_ref
 */
static void
pg_datum_arrow_ref(kern_data_store *kds,
				   kern_colmeta *cmeta,
				   size_t index,
				   Datum *p_datum,
				   bool *p_isnull)
{
	Datum		datum = 0;
	bool		isnull = true;

	if (cmeta->nullmap_offset != 0)
	{
		size_t	nullmap_offset = __kds_unpack(cmeta->nullmap_offset);
		uint8  *nullmap = (uint8 *)kds + nullmap_offset;

		if (att_isnull(index, nullmap))
			goto out;
	}

	if (cmeta->atttypkind == TYPE_KIND__ARRAY)
	{
		kern_colmeta   *smeta;
		cl_uint		   *offset;

		if (cmeta->num_subattrs != 1 ||
			cmeta->idx_subattrs < kds->ncols ||
			cmeta->idx_subattrs >= kds->nr_colmeta)
			elog(ERROR, "Bug? corrupted kernel column metadata");
		smeta = &kds->colmeta[cmeta->idx_subattrs];
		if (cmeta->values_offset == 0)
			goto out;		/* not loaded? always NULL */
		offset = (cl_uint *)((char *)kds + __kds_unpack(cmeta->values_offset));
		datum = pg_array_arrow_ref(kds, smeta,
								   offset[index],
								   offset[index+1]);
		isnull = false;
	}
	else if (cmeta->atttypkind == TYPE_KIND__COMPOSITE)
	{
		TupleDesc	tupdesc = lookup_rowtype_tupdesc(cmeta->atttypid, -1);
		Datum	   *sub_values = alloca(sizeof(Datum) * tupdesc->natts);
		bool	   *sub_isnull = alloca(sizeof(bool)  * tupdesc->natts);
		HeapTuple	htup;
		int			j;

		if (tupdesc->natts != cmeta->num_subattrs)
			elog(ERROR, "Struct definition is conrrupted?");
		if (cmeta->idx_subattrs < kds->ncols ||
			cmeta->idx_subattrs + cmeta->num_subattrs > kds->nr_colmeta)
			elog(ERROR, "Bug? strange kernel column metadata");

		for (j=0; j < tupdesc->natts; j++)
		{
			kern_colmeta *sub_meta = &kds->colmeta[cmeta->idx_subattrs + j];

			pg_datum_arrow_ref(kds, sub_meta, index,
							   sub_values + j,
							   sub_isnull + j);
		}
		htup = heap_form_tuple(tupdesc, sub_values, sub_isnull);

		ReleaseTupleDesc(tupdesc);

		datum = PointerGetDatum(htup->t_data);
		isnull = false;
	}
	else if (cmeta->values_offset == 0)
	{
		/* not loaded, always null */
		goto out;
	}
	else
	{
		Assert(cmeta->atttypkind == TYPE_KIND__BASE);
		switch (cmeta->atttypid)
		{
			case INT2OID:
			case FLOAT2OID:
				datum = pg_int2_arrow_ref(kds, cmeta, index);
				break;
			case INT4OID:
			case FLOAT4OID:
				datum = pg_int4_arrow_ref(kds, cmeta, index);
				break;
			case INT8OID:
			case FLOAT8OID:
				datum = pg_int8_arrow_ref(kds, cmeta, index);
				break;
			case TEXTOID:
			case BYTEAOID:
				datum = pg_varlena_arrow_ref(kds, cmeta, index);
				break;
			case BPCHAROID:
				datum = pg_bpchar_arrow_ref(kds, cmeta, index);
				break;
			case BOOLOID:
				datum = pg_bool_arrow_ref(kds, cmeta, index);
				break;
			case NUMERICOID:
				datum = pg_numeric_arrow_ref(kds, cmeta, index);
				break;
			case DATEOID:
				datum = pg_date_arrow_ref(kds, cmeta, index);
				break;
			case TIMEOID:
				datum = pg_time_arrow_ref(kds, cmeta, index);
				break;
			case TIMESTAMPOID:
				datum = pg_timestamp_arrow_ref(kds, cmeta, index);
				break;
			case INTERVALOID:
				datum = pg_interval_arrow_ref(kds, cmeta, index);
				break;
			default:
				elog(ERROR, "Bug? unexpected datum type: %u",
					 cmeta->atttypid);
				break;
		}
		isnull = false;
	}
out:
	*p_datum  = datum;
	*p_isnull = isnull;
}

/*
 * KDS_fetch_tuple_arrow
 */
bool
KDS_fetch_tuple_arrow(TupleTableSlot *slot,
					  kern_data_store *kds,
					  size_t index)
{
	Datum  *values = slot->tts_values;
	bool   *isnull = slot->tts_isnull;
	int		j;

	if (index >= kds->nitems)
		return false;
	ExecStoreAllNullTuple(slot);
	for (j=0; j < kds->ncols; j++)
	{
		kern_colmeta   *cmeta = &kds->colmeta[j];

		pg_datum_arrow_ref(kds, cmeta,
						   index,
						   values + j,
						   isnull + j);
	}
	return true;
}

/*
 * arrowFdwExtractFilesList
 */
static List *
__arrowFdwExtractFilesList(List *options_list,
						   int *p_parallel_nworkers,
						   bool *p_writable,
						   int *p_pinning)
{
	ListCell   *lc;
	List	   *filesList = NIL;
	char	   *dir_path = NULL;
	char	   *dir_suffix = NULL;
	int			parallel_nworkers = -1;
	bool		writable = false;	/* default: read-only */
	int			pinning = -1;		/* default: not pinning */

	foreach (lc, options_list)
	{
		DefElem	   *defel = lfirst(lc);

		Assert(IsA(defel->arg, String));
		if (strcmp(defel->defname, "file") == 0)
		{
			char   *temp = strVal(defel->arg);
			filesList = lappend(filesList, makeString(pstrdup(temp)));
		}
		else if (strcmp(defel->defname, "files") == 0)
		{
			char   *temp = pstrdup(strVal(defel->arg));
			char   *saveptr;
			char   *tok, *pos;

			while ((tok = strtok_r(temp, ",", &saveptr)) != NULL)
			{
				while (isspace(*tok))
					tok++;
				pos = tok + strlen(tok) - 1;
				while (pos >= tok && isspace(*pos))
					*pos-- = '\0';

				filesList = lappend(filesList, makeString(pstrdup(tok)));

				temp = NULL;
			}
		}
		else if (strcmp(defel->defname, "dir") == 0)
		{
			dir_path = strVal(defel->arg);
		}
		else if (strcmp(defel->defname, "suffix") == 0)
		{
			dir_suffix = strVal(defel->arg);
		}
		else if (strcmp(defel->defname, "parallel_workers") == 0)
		{
			if (parallel_nworkers >= 0)
				elog(ERROR, "'parallel_workers' appeared twice");
			parallel_nworkers = pg_atoi(strVal(defel->arg), sizeof(int), '\0');
		}
		else if (strcmp(defel->defname, "writable") == 0)
		{
			writable = defGetBoolean(defel);
		}
		else if (strcmp(defel->defname, "pinning") == 0)
		{
			int		dindex = -1;

			pinning = pg_atoi(strVal(defel->arg), sizeof(int), '\0');
			if (pinning < 0)
				elog(ERROR, "arrow: pinning=%d is out of range", pinning);
			for (dindex = -1; dindex < numDevAttrs; dindex++)
			{
				if (devAttrs[dindex].DEV_ID == pinning)
					break;
			}
			if (dindex >= numDevAttrs)
			{
				elog(NOTICE, "arrow: pinning=%d is not a valid GPU device id. Ignored", pinning);
				pinning = -1;
			}
		}
		else
			elog(ERROR, "arrow: unknown option (%s)", defel->defname);
	}
	if (dir_suffix && !dir_path)
		elog(ERROR, "arrow: cannot use 'suffix' option without 'dir'");

	if (writable)
	{
		if (dir_path)
			elog(ERROR, "arrow: 'dir_path' and 'writable' options are exclusive");
		if (list_length(filesList) == 0)
			elog(ERROR, "arrow: 'writable' needs a backend file specified by 'file' option");
		if (list_length(filesList) > 1)
			elog(ERROR, "arrow: 'writable' cannot use multiple backend files");
	}

	if (dir_path)
	{
		struct dirent *dentry;
		DIR	   *dir;
		char   *temp;

		dir = AllocateDir(dir_path);
		while ((dentry = ReadDir(dir, dir_path)) != NULL)
		{
			if (strcmp(dentry->d_name, ".") == 0 ||
				strcmp(dentry->d_name, "..") == 0)
				continue;
			if (dir_suffix)
			{
				int		dlen = strlen(dentry->d_name);
				int		slen = strlen(dir_suffix);
				int		diff;

				if (dlen < 2 + slen)
					continue;
				diff = dlen - slen;
				if (dentry->d_name[diff-1] != '.' ||
					strcmp(dentry->d_name + diff, dir_suffix) != 0)
					continue;
			}
			temp = psprintf("%s/%s", dir_path, dentry->d_name);
			filesList = lappend(filesList, makeString(temp));
		}
		FreeDir(dir);
	}

	if (filesList == NIL)
		elog(ERROR, "arrow: no files are on behalf of the foreign table");
	foreach (lc, filesList)
	{
		Value  *val = lfirst(lc);

		if (access(strVal(val), F_OK | R_OK) != 0)
			elog(ERROR, "arrow: permission error at '%s': %m", strVal(val));
	}

	/* other properties */
	if (p_parallel_nworkers)
		*p_parallel_nworkers = parallel_nworkers;
	if (p_writable)
		*p_writable = writable;
	if (p_pinning)
		*p_pinning = pinning;

	return filesList;
}

static List *
arrowFdwExtractFilesList(List *options_list)
{
	return __arrowFdwExtractFilesList(options_list, NULL, NULL, NULL);
}


/*
 * validator of Arrow_Fdw
 */
Datum
pgstrom_arrow_fdw_validator(PG_FUNCTION_ARGS)
{
	List	   *options_list = untransformRelOptions(PG_GETARG_DATUM(0));
	Oid			catalog = PG_GETARG_OID(1);

	if (catalog == ForeignTableRelationId)
	{
		List	   *filesList;
		ListCell   *lc;

		filesList = arrowFdwExtractFilesList(options_list);
		foreach (lc, filesList)
		{
			ArrowFileInfo	af_info;
			char		   *fname = strVal(lfirst(lc));

			readArrowFile(fname, &af_info);
			elog(DEBUG2, "%s", dumpArrowNode((ArrowNode *)&af_info.footer));
		}
	}
	else if (options_list != NIL)
	{
		const char	   *label;
		char			temp[80];

		switch (catalog)
		{
			case ForeignDataWrapperRelationId:
				label = "FOREIGN DATA WRAPPER";
				break;
			case ForeignServerRelationId:
				label = "SERVER";
				break;
			case UserMappingRelationId:
				label = "USER MAPPING";
				break;
			case AttributeRelationId:
				label = "attribute of FOREIGN TABLE";
				break;
			default:
				snprintf(temp, sizeof(temp),
						 "[unexpected object catalog=%u]", catalog);
				label = temp;
				break;
		}
		elog(ERROR, "Arrow_Fdw does not support any options for %s", label);
	}
	PG_RETURN_VOID();
}
PG_FUNCTION_INFO_V1(pgstrom_arrow_fdw_validator);

/*
 * pgstrom_arrow_fdw_precheck_schema
 */
static void
arrow_fdw_precheck_schema(Relation rel)
{
	TupleDesc		tupdesc = RelationGetDescr(rel);
	ForeignTable   *ft = GetForeignTable(RelationGetRelid(rel));
	List		   *filesList;
	ListCell	   *lc;
	int				j;

	/* check schema definition is supported by Apache Arrow */
	for (j=0; j < tupdesc->natts; j++)
	{
		Form_pg_attribute	attr = tupleDescAttr(tupdesc, j);

		if (!arrowTypeIsConvertible(attr->atttypid,
									attr->atttypmod))
			elog(ERROR, "column %s of foreign table %s has %s type that is not convertible any supported Apache Arrow types",
				 NameStr(attr->attname),
				 RelationGetRelationName(rel),
				 format_type_be(attr->atttypid));
	}

	filesList = arrowFdwExtractFilesList(ft->options);
	foreach (lc, filesList)
	{
		const char *fname = strVal(lfirst(lc));
		File		fdesc;
		List	   *rb_cached = NIL;
		ListCell   *cell;

		fdesc = PathNameOpenFile(fname, O_RDONLY | PG_BINARY);
		if (fdesc < 0)
			elog(ERROR, "failed to open '%s'", fname);
		/* check schema compatibility */
		rb_cached = arrowLookupOrBuildMetadataCache(fdesc);
		foreach (cell, rb_cached)
		{
			RecordBatchState *rb_state = lfirst(cell);

			if (!arrowSchemaCompatibilityCheck(tupdesc, rb_state))
				elog(ERROR, "arrow file '%s' on behalf of the foreign table '%s' has incompatible schema definition",
					 fname, RelationGetRelationName(rel));
		}
		list_free(rb_cached);
	}
}

Datum
pgstrom_arrow_fdw_precheck_schema(PG_FUNCTION_ARGS)
{
	EventTriggerData   *trigdata;
	
	if (!CALLED_AS_EVENT_TRIGGER(fcinfo))
		elog(ERROR, "%s: must be called as EventTrigger",
			 __FUNCTION__);
	trigdata = (EventTriggerData *) fcinfo->context;
	if (strcmp(trigdata->event, "ddl_command_end") != 0)
		elog(ERROR, "%s: must be called on ddl_command_end event",
			 __FUNCTION__);
	if (strcmp(trigdata->tag, "CREATE FOREIGN TABLE") == 0)
	{
		CreateStmt	   *stmt = (CreateStmt *)trigdata->parsetree;
		Relation		rel;

		rel = relation_openrv_extended(stmt->relation, AccessShareLock, true);
		if (!rel)
			PG_RETURN_NULL();
		if (rel->rd_rel->relkind == RELKIND_FOREIGN_TABLE &&
			GetFdwRoutineForRelation(rel, false) == &pgstrom_arrow_fdw_routine)
		{
			arrow_fdw_precheck_schema(rel);
		}
		relation_close(rel, AccessShareLock);
	}
	else if (strcmp(trigdata->tag, "ALTER FOREIGN TABLE") == 0)
	{
		AlterTableStmt *stmt = (AlterTableStmt *)trigdata->parsetree;
		Relation		rel;
		ListCell	   *lc;
		bool			has_schema_change = false;

		rel = relation_openrv_extended(stmt->relation, AccessShareLock, true);
		if (!rel)
			PG_RETURN_NULL();
		if (rel->rd_rel->relkind == RELKIND_FOREIGN_TABLE &&
			GetFdwRoutineForRelation(rel, false) == &pgstrom_arrow_fdw_routine)
		{
			foreach (lc, stmt->cmds)
			{
				AlterTableCmd  *cmd = lfirst(lc);

				if (cmd->subtype == AT_AddColumn ||
					cmd->subtype == AT_DropColumn ||
					cmd->subtype == AT_AlterColumnType)
				{
					has_schema_change = true;
					break;
				}
			}
			if (has_schema_change)
				arrow_fdw_precheck_schema(rel);
		}
		relation_close(rel, AccessShareLock);
	}
	else
	{
		elog(NOTICE, "%s was called on %s, ignored",
			 __FUNCTION__, trigdata->tag);
	}
	PG_RETURN_NULL();
}
PG_FUNCTION_INFO_V1(pgstrom_arrow_fdw_precheck_schema);

/*
 * arrowInvalidateMetadataCache
 *
 * NOTE: caller must have lock_slots[] with EXCLUSIVE mode
 */
static uint64
arrowInvalidateMetadataCache(arrowMetadataCache *mcache, bool detach_lru)
{
	arrowMetadataCache *mtemp;
	dlist_node	   *dnode;
	uint64			released = 0;

	while (!dlist_is_empty(&mcache->siblings))
	{
		dnode = dlist_pop_head_node(&mcache->siblings);
		mtemp = dlist_container(arrowMetadataCache, chain, dnode);
		Assert(dlist_is_empty(&mtemp->siblings) &&
			   !mtemp->lru_chain.prev && !mtemp->lru_chain.next);
		dlist_delete(&mtemp->chain);
		released += MAXALIGN(offsetof(arrowMetadataCache,
									  fstate[mtemp->nfields]));
		pfree(mtemp);
	}
	released += MAXALIGN(offsetof(arrowMetadataCache,
								  fstate[mcache->nfields]));
	if (detach_lru)
	{
		SpinLockAcquire(&arrow_metadata_state->lru_lock);
		dlist_delete(&mcache->lru_chain);
		SpinLockRelease(&arrow_metadata_state->lru_lock);
	}
	dlist_delete(&mcache->chain);
	pfree(mcache);

	return pg_atomic_sub_fetch_u64(&arrow_metadata_state->consumed,
								   released);
}

/*
 * copyMetadataFieldCache - copy for nested structure
 */
static int
copyMetadataFieldCache(RecordBatchFieldState *dest_curr,
					   RecordBatchFieldState *dest_tail,
					   int nattrs,
					   RecordBatchFieldState *columns)
{
	RecordBatchFieldState *dest_next = dest_curr + nattrs;
	int		j, k, nslots = nattrs;

	if (dest_next > dest_tail)
		return -1;

	for (j=0; j < nattrs; j++)
	{
		dest_curr[j] = columns[j];
		if (dest_curr[j].num_children == 0)
			Assert(dest_curr[j].children == NULL);
		else
		{
			dest_curr[j].children = dest_next;
			k = copyMetadataFieldCache(dest_next,
									   dest_tail,
									   columns[j].num_children,
									   columns[j].children);
			if (k < 0)
				return -1;
			dest_next += k;
			nslots += k;
		}
	}
	return nslots;
}

/*
 * makeRecordBatchStateFromCache
 *   - setup RecordBatchState from arrowMetadataCache
 */
static RecordBatchState *
makeRecordBatchStateFromCache(arrowMetadataCache *mcache, File fdesc)
{
	RecordBatchState   *rbstate;

	rbstate = palloc0(offsetof(RecordBatchState,
							   columns[mcache->nfields]));
	rbstate->fdesc = fdesc;
	memcpy(&rbstate->stat_buf, &mcache->stat_buf, sizeof(struct stat));
	rbstate->rb_index  = mcache->rb_index;
	rbstate->rb_offset = mcache->rb_offset;
	rbstate->rb_length = mcache->rb_length;
	rbstate->rb_nitems = mcache->rb_nitems;
	rbstate->ncols = mcache->ncols;
	copyMetadataFieldCache(rbstate->columns,
						   rbstate->columns + mcache->nfields,
						   mcache->ncols,
						   mcache->fstate);
	return rbstate;
}

/*
 * arrowReclaimMetadataCache
 */
static void
arrowReclaimMetadataCache(void)
{
	arrowMetadataCache *mcache;
	LWLock	   *lock = NULL;
	dlist_node *dnode;
	uint32		lru_hash;
	uint32		lru_index;
	uint64		consumed;

	consumed = pg_atomic_read_u64(&arrow_metadata_state->consumed);
	if (consumed <= arrow_metadata_cache_size)
		return;

	SpinLockAcquire(&arrow_metadata_state->lru_lock);
	if (dlist_is_empty(&arrow_metadata_state->lru_list))
	{
		SpinLockRelease(&arrow_metadata_state->lru_lock);
		return;
	}
	dnode = dlist_tail_node(&arrow_metadata_state->lru_list);
	mcache = dlist_container(arrowMetadataCache, lru_chain, dnode);
	lru_hash = mcache->hash;
	SpinLockRelease(&arrow_metadata_state->lru_lock);

	do {
		lru_index = lru_hash % ARROW_METADATA_HASH_NSLOTS;
		lock = &arrow_metadata_state->lock_slots[lru_index];

		LWLockAcquire(lock, LW_EXCLUSIVE);
		SpinLockAcquire(&arrow_metadata_state->lru_lock);
		if (dlist_is_empty(&arrow_metadata_state->lru_list))
		{
			SpinLockRelease(&arrow_metadata_state->lru_lock);
			LWLockRelease(lock);
			break;
		}
		dnode = dlist_tail_node(&arrow_metadata_state->lru_list);
		mcache = dlist_container(arrowMetadataCache, lru_chain, dnode);
		if (mcache->hash == lru_hash)
		{
			dlist_delete(&mcache->lru_chain);
			memset(&mcache->lru_chain, 0, sizeof(dlist_node));
			SpinLockRelease(&arrow_metadata_state->lru_lock);
			consumed = arrowInvalidateMetadataCache(mcache, false);
		}
		else
		{
			/* LRU-tail was referenced by someone, try again */
			lru_hash = mcache->hash;
            SpinLockRelease(&arrow_metadata_state->lru_lock);
		}
		LWLockRelease(lock);
	} while (consumed > arrow_metadata_cache_size);
}

/*
 * __arrowBuildMetadataCache
 *
 * NOTE: caller must have exclusive lock on arrow_metadata_state->lock_slots[]
 */
static arrowMetadataCache *
__arrowBuildMetadataCache(List *rb_state_list, uint32 hash)
{
	arrowMetadataCache *mcache = NULL;
	arrowMetadataCache *mtemp;
	dlist_node *dnode;
	Size		sz, consumed = 0;
	int			nfields;
	ListCell   *lc;

	foreach (lc, rb_state_list)
	{
		RecordBatchState *rbstate = lfirst(lc);

		if (!mcache)
			nfields = RecordBatchFieldCount(rbstate);
		else
			Assert(nfields == RecordBatchFieldCount(rbstate));

		sz = offsetof(arrowMetadataCache, fstate[nfields]);
		mtemp = MemoryContextAllocZero(TopSharedMemoryContext, sz);
		if (!mtemp)
		{
			/* !!out of memory!! */
			if (mcache)
			{
				while (!dlist_is_empty(&mcache->siblings))
				{
					dnode = dlist_pop_head_node(&mcache->siblings);
					mtemp = dlist_container(arrowMetadataCache,
											chain, dnode);
					pfree(mtemp);
				}
				pfree(mcache);
			}
			return NULL;
		}

		dlist_init(&mtemp->siblings);
		memcpy(&mtemp->stat_buf, &rbstate->stat_buf, sizeof(struct stat));
		mtemp->hash      = hash;
		mtemp->rb_index  = rbstate->rb_index;
        mtemp->rb_offset = rbstate->rb_offset;
        mtemp->rb_length = rbstate->rb_length;
        mtemp->rb_nitems = rbstate->rb_nitems;
        mtemp->ncols     = rbstate->ncols;
		mtemp->nfields   =
			copyMetadataFieldCache(mtemp->fstate,
								   mtemp->fstate + nfields,
								   rbstate->ncols,
								   rbstate->columns);
		Assert(mtemp->nfields == nfields);

		if (!mcache)
			mcache = mtemp;
		else
			dlist_push_tail(&mcache->siblings, &mtemp->chain);
		consumed += MAXALIGN(sz);
	}
	pg_atomic_add_fetch_u64(&arrow_metadata_state->consumed, consumed);

	return mcache;
}


/*
 * checkArrowRecordBatchIsVisible
 *
 * NOTE: It must be called under shared lock on lock_slots[]
 */
static bool
checkArrowRecordBatchIsVisible(RecordBatchState *rbstate,
							   dlist_head *mvcc_slot)
{
	dlist_iter		iter;

	dlist_foreach(iter, mvcc_slot)
	{
		arrowWriteMVCCLog  *mvcc = dlist_container(arrowWriteMVCCLog,
												   chain, iter.cur);
		if (mvcc->key.st_dev == rbstate->stat_buf.st_dev &&
			mvcc->key.st_ino == rbstate->stat_buf.st_ino &&
			mvcc->record_batch == rbstate->rb_index)
		{
			if (TransactionIdIsCurrentTransactionId(mvcc->xid))
				return true;
			else
				return false;
		}
	}
	return true;
}

/*
 * arrowLookupOrBuildMetadataCache
 */
List *
arrowLookupOrBuildMetadataCache(File fdesc)
{
	MetadataCacheKey key;
	struct stat	stat_buf;
	uint32		index;
	LWLock	   *lock;
	dlist_head *hash_slot;
	dlist_head *mvcc_slot;
	dlist_iter	iter1, iter2;
	bool		has_exclusive = false;
	List	   *results = NIL;

	if (fstat(FileGetRawDesc(fdesc), &stat_buf) != 0)
		elog(ERROR, "failed on fstat('%s'): %m", FilePathName(fdesc));

	memset(&key, 0, sizeof(key));
	key.st_dev	= stat_buf.st_dev;
	key.st_ino	= stat_buf.st_ino;
	key.hash = hash_any((unsigned char *)&key,
						offsetof(MetadataCacheKey, hash));
	index = key.hash % ARROW_METADATA_HASH_NSLOTS;
	lock = &arrow_metadata_state->lock_slots[index];
	hash_slot = &arrow_metadata_state->hash_slots[index];
	mvcc_slot = &arrow_metadata_state->mvcc_slots[index];

	LWLockAcquire(lock, LW_SHARED);
retry:
	dlist_foreach(iter1, hash_slot)
	{
		arrowMetadataCache *mcache
			= dlist_container(arrowMetadataCache, chain, iter1.cur);
		if (mcache->stat_buf.st_dev == stat_buf.st_dev &&
			mcache->stat_buf.st_ino == stat_buf.st_ino)
		{
			RecordBatchState *rbstate;

			Assert(mcache->hash == key.hash);
			if (mcache->stat_buf.st_mtime < stat_buf.st_mtime ||
				mcache->stat_buf.st_ctime < stat_buf.st_ctime)
			{
				char	buf1[64], buf2[64], buf3[64], buf4[64];

				if (!has_exclusive)
				{
					LWLockRelease(lock);
					LWLockAcquire(lock, LW_EXCLUSIVE);
					has_exclusive = true;
					goto retry;
				}
				elog(DEBUG2, "arrow_fdw: metadata cache for '%s' (m=%s c=%s) is older than the latest file (m=%s c=%s), so invalidated",
					 FilePathName(fdesc),
					 ctime_r(&mcache->stat_buf.st_mtime, buf1),
					 ctime_r(&mcache->stat_buf.st_ctime, buf2),
					 ctime_r(&stat_buf.st_mtime, buf3),
					 ctime_r(&stat_buf.st_ctime, buf4));
				arrowInvalidateMetadataCache(mcache, true);
				break;
			}
			/*
			 * Ok, arrow file metadata cache found and still valid
			 */
			rbstate = makeRecordBatchStateFromCache(mcache, fdesc);
			if (checkArrowRecordBatchIsVisible(rbstate, mvcc_slot))
				results = list_make1(rbstate);
			dlist_foreach (iter2, &mcache->siblings)
			{
				arrowMetadataCache *__mcache
					= dlist_container(arrowMetadataCache, chain, iter2.cur);
				rbstate = makeRecordBatchStateFromCache(__mcache, fdesc);
				if (checkArrowRecordBatchIsVisible(rbstate, mvcc_slot))
					results = lappend(results, rbstate);
			}
			SpinLockAcquire(&arrow_metadata_state->lru_lock);
			dlist_move_head(&arrow_metadata_state->lru_list,
							&mcache->lru_chain);
			SpinLockRelease(&arrow_metadata_state->lru_lock);
			LWLockRelease(lock);

			return results;
		}
	}

	/*
	 * Hmm... no valid metadata cache was not found, so build a new entry
	 * under the exclusive lock on the arrow file.
	 */
	if (!has_exclusive)
	{
		LWLockRelease(lock);
		LWLockAcquire(lock, LW_EXCLUSIVE);
		has_exclusive = true;
		goto retry;
	}
	else
	{
		ArrowFileInfo	af_info;
		arrowMetadataCache *mcache;
		List		   *rb_state_any = NIL;

		readArrowFileDesc(FileGetRawDesc(fdesc), &af_info);
		if (af_info.dictionaries != NULL)
			elog(ERROR, "DictionaryBatch is not supported");
		Assert(af_info.footer._num_dictionaries == 0);

		if (af_info.recordBatches == NULL)
			elog(DEBUG2, "arrow file '%s' contains no RecordBatch",
				 FilePathName(fdesc));
		for (index = 0; index < af_info.footer._num_recordBatches; index++)
		{
			RecordBatchState *rb_state;
			ArrowBlock       *block
				= &af_info.footer.recordBatches[index];
			ArrowRecordBatch *rbatch
				= &af_info.recordBatches[index].body.recordBatch;

			rb_state = makeRecordBatchState(&af_info.footer.schema,
											block, rbatch);
			rb_state->fdesc = fdesc;
			memcpy(&rb_state->stat_buf, &stat_buf, sizeof(struct stat));
			rb_state->rb_index = index;

			if (checkArrowRecordBatchIsVisible(rb_state, mvcc_slot))
				results = lappend(results, rb_state);
			rb_state_any = lappend(rb_state_any, rb_state);
		}
		/* try to build a metadata cache for further references */
		mcache = __arrowBuildMetadataCache(rb_state_any, key.hash);
		if (mcache)
		{
			dlist_push_head(hash_slot, &mcache->chain);
			SpinLockAcquire(&arrow_metadata_state->lru_lock);
            dlist_push_head(&arrow_metadata_state->lru_list,
							&mcache->lru_chain);
			SpinLockRelease(&arrow_metadata_state->lru_lock);
		}
	}
	LWLockRelease(lock);
	/*
	 * reclaim unreferenced metadata cache entries based on LRU, if shared-
	 * memory consumption exceeds the configured threshold.
	 */
	arrowReclaimMetadataCache();

	return results;
}

/*
 * setupArrowSQLbufferSchema
 */
static void
__setupArrowSQLbufferField(SQLtable *table,
						   SQLfield *column,
						   const char *attname,
						   Oid atttypid,
						   int atttypmod)
{
	HeapTuple		tup;
	Form_pg_type	typ;
	const char	   *typnamespace;

	tup = SearchSysCache1(TYPEOID, ObjectIdGetDatum(atttypid));
	if (!HeapTupleIsValid(tup))
		elog(ERROR, "cache lookup failed for type: %u", atttypid);
	typ = (Form_pg_type) GETSTRUCT(tup);
	typnamespace = get_namespace_name(typ->typnamespace);

	table->numFieldNodes++;
    table->numBuffers += assignArrowTypePgSQL(column,
											  attname,
											  atttypid,
											  atttypmod,
											  NameStr(typ->typname),
											  typnamespace,
											  typ->typlen,
											  typ->typbyval,
											  typ->typtype,
											  typ->typalign,
											  typ->typrelid,
											  typ->typelem);
	if (OidIsValid(typ->typelem))
	{
		/* array type */
		char		elem_name[NAMEDATALEN+10];

		snprintf(elem_name, sizeof(elem_name), "_%s[]", attname);
		column->element = palloc0(sizeof(SQLfield));
		__setupArrowSQLbufferField(table,
								   column->element,
								   elem_name,
								   typ->typelem,
								   -1);
	}
	else if (OidIsValid(typ->typrelid))
	{
		/* composite type */
		TupleDesc	tupdesc = lookup_rowtype_tupdesc(atttypid, atttypmod);
		int			j;

		column->nfields = tupdesc->natts;
		column->subfields = palloc0(sizeof(SQLfield) * tupdesc->natts);
		for (j=0; j < tupdesc->natts; j++)
		{
			Form_pg_attribute sattr = tupleDescAttr(tupdesc, j);
			__setupArrowSQLbufferField(table,
									   &column->subfields[j],
									   NameStr(sattr->attname),
									   sattr->atttypid,
									   sattr->atttypmod);
		}
		ReleaseTupleDesc(tupdesc);
	}
	else if (typ->typtype == 'e')
	{
		elog(ERROR, "Enum type is not supported right now");
	}
	ReleaseSysCache(tup);
}

static void
setupArrowSQLbufferSchema(SQLtable *table, TupleDesc tupdesc)
{
	int		j;

	table->nfields = tupdesc->natts;
	for (j=0; j < tupdesc->natts; j++)
	{
		Form_pg_attribute attr = tupleDescAttr(tupdesc, j);
		__setupArrowSQLbufferField(table,
								   &table->columns[j],
								   NameStr(attr->attname),
								   attr->atttypid,
								   attr->atttypmod);
	}
	table->segment_sz = (size_t)arrow_record_batch_size_kb << 10;
}

static void
setupArrowSQLbufferBatches(SQLtable *table)
{
	ArrowFileInfo af_info;
	struct stat	stat_buf;
	MetadataCacheKey key;
	uint32		index;
	int			i, nitems;
	loff_t		pos = 0;

	if (fstat(table->fdesc, &stat_buf) != 0)
		elog(ERROR, "failed on fstat('%s'): %m", table->filename);
	memset(&key, 0, sizeof(key));
	key.st_dev = stat_buf.st_dev;
	key.st_ino = stat_buf.st_ino;
	key.hash = hash_any((unsigned char *)&key,
						offsetof(MetadataCacheKey, hash));
	index = key.hash % ARROW_METADATA_HASH_NSLOTS;

	LWLockAcquire(&arrow_metadata_state->lock_slots[index], LW_SHARED);
	readArrowFileDesc(table->fdesc, &af_info);
	LWLockRelease(&arrow_metadata_state->lock_slots[index]);

	/* restore DictionaryBatches already in the file */
	nitems = af_info.footer._num_dictionaries;
	table->numDictionaries = nitems;
	if (nitems > 0)
	{
		table->dictionaries = palloc(sizeof(ArrowBlock) * nitems);
		memcpy(table->dictionaries,
			   af_info.footer.dictionaries,
			   sizeof(ArrowBlock) * nitems);
		for (i=0; i < nitems; i++)
		{
			ArrowBlock *block = &table->dictionaries[i];

			pos = Max(pos, ARROWALIGN(block->offset +
									  block->metaDataLength +
									  block->bodyLength));
		}
	}
	else
		table->dictionaries = NULL;

	/* restore RecordBatches already in the file */
	nitems = af_info.footer._num_recordBatches;
	table->numRecordBatches = nitems;
	if (nitems > 0)
	{
		table->recordBatches = palloc(sizeof(ArrowBlock) * nitems);
		memcpy(table->recordBatches,
			   af_info.footer.recordBatches,
			   sizeof(ArrowBlock) * nitems);
		for (i=0; i < nitems; i++)
		{
			ArrowBlock *block = &table->recordBatches[i];

			pos = Max(pos, ARROWALIGN(block->offset +
									  block->metaDataLength +
									  block->bodyLength));
		}
	}
	else
		table->recordBatches = NULL;

	if (lseek(table->fdesc, pos, SEEK_SET) < 0)
		elog(ERROR, "failed on lseek('%s',%lu): %m",
			 table->filename, pos);
}

/*
 * createArrowWriteState
 */
static arrowWriteState *
createArrowWriteState(Relation frel, File file)
{
	TupleDesc		tupdesc = RelationGetDescr(frel);
	arrowWriteState *aw_state;
	SQLtable	   *table;
	struct stat		stat_buf;
	MetadataCacheKey key;

	if (fstat(FileGetRawDesc(file), &stat_buf) != 0)
		elog(ERROR, "failed on fstat('%s'): %m", FilePathName(file));
	memset(&key, 0, sizeof(key));
	key.st_dev = stat_buf.st_dev;
	key.st_ino = stat_buf.st_ino;
	key.hash = hash_any((unsigned char *)&key,
						offsetof(MetadataCacheKey, hash));
	aw_state = palloc0(offsetof(arrowWriteState,
								sql_table.columns[tupdesc->natts]));
	aw_state->memcxt = CurrentMemoryContext;
	aw_state->file = file;
	aw_state->key = key;
	aw_state->hash = key.hash;
	aw_state->redo_log_written = false;
	table = &aw_state->sql_table;
	table->filename = FilePathName(file);
	table->fdesc = FileGetRawDesc(file);
	setupArrowSQLbufferSchema(table, tupdesc);
	setupArrowSQLbufferBatches(table);

	return aw_state;
}

/*
 * createArrowWriteRedoLog
 */
static void
createArrowWriteRedoLog(arrowWriteState *aw_state, off_t filesize)
{
	SQLtable	   *table = &aw_state->sql_table;
	arrowWriteRedoLog *redo;
	TransactionId	curr_xid = GetCurrentTransactionId();
	CommandId		curr_cid = GetCurrentCommandId(true);
	dlist_iter		iter;
	size_t			main_sz;

	dlist_foreach(iter, &arrow_write_redo_list)
	{
		redo = dlist_container(arrowWriteRedoLog, chain, iter.cur);

		if (redo->key.st_dev == aw_state->key.st_dev &&
			redo->key.st_ino == aw_state->key.st_ino &&
			redo->xid    == curr_xid &&
			redo->cid    <= curr_cid)
		{
			Assert(redo->key.hash == aw_state->key.hash);
			return;		/* nothing to do */
		}
	}

	if (filesize == 0)
	{
		main_sz = MAXALIGN(offsetof(arrowWriteRedoLog, footer_backup));
		redo = MemoryContextAllocZero(CacheMemoryContext,
									  main_sz + strlen(table->filename) + 1);
		redo->key    = aw_state->key;
		redo->xid    = curr_xid;
		redo->cid    = curr_cid;
		redo->pathname = (char *)redo + main_sz;
		strcpy(redo->pathname, table->filename);
		redo->is_truncate = false;
	}
	else
	{
		ssize_t			nbytes;
		off_t			offset;
		char			temp[100];

		/* make backup image of the Footer section */
		nbytes = sizeof(int32) + 6;		/* = strlen("ARROW1") */
		offset = lseek(table->fdesc, -nbytes, SEEK_END);
		if (offset < 0)
			elog(ERROR, "failed on lseek(2): %m");
		if (__read(table->fdesc, temp, nbytes) != nbytes)
			elog(ERROR, "failed on read(2): %m");
		offset -= *((int32 *)temp);

		nbytes = filesize - offset;
		if (nbytes <= 0)
			elog(ERROR, "strange apache arrow format");
		main_sz = MAXALIGN(offsetof(arrowWriteRedoLog, footer_backup[nbytes]));
		redo = MemoryContextAllocZero(CacheMemoryContext,
									  main_sz + strlen(table->filename) + 1);
		redo->key    = aw_state->key;
		redo->xid    = curr_xid;
		redo->cid    = curr_cid;
		redo->pathname = (char *)redo + main_sz;
		strcpy(redo->pathname, table->filename);
		PG_TRY();
		{
			if (lseek(table->fdesc, -nbytes, SEEK_END) < 0)
				elog(ERROR, "failed on lseek(2): %m");
			if (__read(table->fdesc, redo->footer_backup, nbytes) != nbytes)
				elog(ERROR, "failed on read(2): %m");
			if (lseek(table->fdesc, -nbytes, SEEK_END) < 0)
				elog(ERROR, "failed on lseek(2): %m");
			redo->footer_offset = offset;
			redo->footer_length = nbytes;
		}
		PG_CATCH();
		{
			pfree(redo);
			PG_RE_THROW();
		}
		PG_END_TRY();
	}
	elog(INFO, "REDO log on [%s] st_dev=%u/st_ino=%u/xid=%u/cid=%u offset=%lu length=%zu",
		 redo->pathname, (uint32)redo->key.st_dev, (uint32)redo->key.st_ino, (uint32)redo->xid, (uint32)redo->cid, (uint64)redo->footer_offset, (uint64)redo->footer_length);

	dlist_push_head(&arrow_write_redo_list, &redo->chain);
}

/*
 * writeOutArrowRecordBatch
 */
static void
writeOutArrowRecordBatch(arrowWriteState *aw_state, bool with_footer)
{
	SQLtable   *table = &aw_state->sql_table;
	int			index = aw_state->hash % ARROW_METADATA_HASH_NSLOTS;
	struct stat	stat_buf;
	ssize_t		nbytes;
	arrowWriteMVCCLog *mvcc = NULL;

	if (table->nitems > 0)
	{
		mvcc = MemoryContextAllocZero(TopSharedMemoryContext,
									  sizeof(arrowWriteMVCCLog));
		mvcc->key = aw_state->key;
		mvcc->xid = GetCurrentTransactionId();
		mvcc->cid = GetCurrentCommandId(true);
	}

	PG_TRY();
	{
		LWLockAcquire(&arrow_metadata_state->lock_slots[index],
					  LW_EXCLUSIVE);
		/* get the latest state of the file */
		if (fstat(table->fdesc, &stat_buf) != 0)
			elog(ERROR, "failed on fstat('%s'): %m", table->filename);
		/* make a REDO log entry */
		if (!aw_state->redo_log_written)
		{
			createArrowWriteRedoLog(aw_state, stat_buf.st_size);
			aw_state->redo_log_written = true;
		}
		/* write out an empty arrow file */
		if (stat_buf.st_size == 0)
		{
			Assert(lseek(table->fdesc, 0, SEEK_END) == 0);
			nbytes = __write(table->fdesc, "ARROW1\0\0", 8);
			if (nbytes != 8)
				elog(ERROR, "failed on __write('%s'): %m", table->filename);
			writeArrowSchema(table);
		}
		if (table->nitems > 0)
		{
			mvcc->record_batch = writeArrowRecordBatch(table);
			dlist_push_tail(&arrow_metadata_state->mvcc_slots[index],
							&mvcc->chain);
			elog(INFO, "arrow mvcc: st_dev=%u, st_ino=%u, xid=%u, cid=%u, record_batch=%u",
				 (uint32)mvcc->key.st_dev, (uint32)mvcc->key.st_ino, (uint32)mvcc->xid, (uint32)mvcc->cid, mvcc->record_batch);
		}
		if (with_footer)
			writeArrowFooter(table);
		LWLockRelease(&arrow_metadata_state->lock_slots[index]);
	}
	PG_CATCH();
	{
		if (mvcc)
			pfree(mvcc);
		PG_RE_THROW();
	}
	PG_END_TRY();
}

/*
 * TRUNCATE support
 */
static void
__arrowExecTruncateRelation(Relation frel)
{
	TupleDesc	tupdesc = RelationGetDescr(frel);
	Oid			frel_oid = RelationGetRelid(frel);
	ForeignTable *ft = GetForeignTable(frel_oid);
	arrowWriteRedoLog *redo;
	struct stat	stat_buf;
	MetadataCacheKey key;
	List	   *filesList;
	SQLtable   *table;
	const char *path_name;
	const char *dir_name;
	const char *file_name;
	size_t		main_sz;
	int			dirfd = -1;
	int			fdesc = -1;
	int			nbytes;
	char		path[MAXPGPATH];
	bool		writable;

	filesList = __arrowFdwExtractFilesList(ft->options,
										   NULL,
										   &writable,
										   NULL);
	if (!writable)
		elog(ERROR, "arrow_fdw: foreign table \"%s\" is not writable",
			 RelationGetRelationName(frel));
	Assert(list_length(filesList) == 1);
	path_name = strVal(linitial(filesList));
	if (stat(path_name, &stat_buf) != 0)
		elog(ERROR, "failed on stat('%s'): %m", path_name);
	memset(&key, 0, sizeof(key));
	key.st_dev = stat_buf.st_dev;
	key.st_ino = stat_buf.st_ino;
	key.hash = hash_any((unsigned char *)&key,
						offsetof(MetadataCacheKey, hash));
	/* build SQLtable to write out schema */
	table = palloc0(offsetof(SQLtable, columns[tupdesc->natts]));
	setupArrowSQLbufferSchema(table, tupdesc);

	/* create REDO log entry */
	main_sz = MAXALIGN(offsetof(arrowWriteRedoLog, footer_backup));
	redo = MemoryContextAllocZero(CacheMemoryContext,
								  main_sz + strlen(path_name) + 1);
	redo->key    = key;
	redo->xid    = GetCurrentTransactionId();
	redo->cid    = GetCurrentCommandId(true);
	redo->pathname = (char *)redo + main_sz;
	strcpy(redo->pathname, path_name);
	redo->is_truncate = true;

	/* open directory of the arrow file (must have write permission) */
	dir_name = dirname(pstrdup(path_name));
	file_name = basename(pstrdup(path_name));
	dirfd = open(dir_name, O_RDONLY | O_DIRECTORY);
	if (dirfd < 0)
	{
		pfree(redo);
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not open directory '%s': %m", dir_name)));
	}

	PG_TRY();
	{
		/* create an empty file */
	retry:
		redo->suffix = random();
		snprintf(path, sizeof(path), "%s.%u.backup",
				 file_name, redo->suffix);
		fdesc = openat(dirfd, path, O_RDWR | O_CREAT | O_EXCL, 0600);
		if (fdesc < 0)
		{
			if (errno == EEXIST)
				goto retry;
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not create a file '%s' at '%s': %m",
							path, dir_name)));
		}

		/* write out an empty arrow file */
		table->filename = path;
		table->fdesc = fdesc;
		nbytes = __write(table->fdesc, "ARROW1\0\0", 8);
		if (nbytes != 8)
			elog(ERROR, "failed on __write('%s'): %m", path);
		writeArrowSchema(table);
		writeArrowFooter(table);

		/* swap two files atomically */
		if (renameat2(dirfd, file_name,
					  dirfd, path, RENAME_EXCHANGE) != 0)
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("failed to swap '%s' and '%s' at '%s': %m",
							file_name, path, dir_name)));
	}
	PG_CATCH();
	{
		if (fdesc >= 0)
		{
			unlinkat(dirfd, path, 0);
			close(fdesc);
		}
		close(dirfd);
		pfree(redo);
		PG_RE_THROW();
	}
	PG_END_TRY();
	close(fdesc);
	close(dirfd);

	/* save the REDO log entry */
	dlist_push_head(&arrow_write_redo_list, &redo->chain);
}

/*
 * pgstrom_arrow_fdw_truncate
 */
Datum
pgstrom_arrow_fdw_truncate(PG_FUNCTION_ARGS)
{
	Oid			frel_oid = PG_GETARG_OID(0);
	Relation	frel;
	FdwRoutine *routine;

	frel = table_open(frel_oid, AccessExclusiveLock);
	if (frel->rd_rel->relkind != RELKIND_FOREIGN_TABLE)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("\"%s\" is not arrow_fdw foreign table",
						RelationGetRelationName(frel))));
	routine = GetFdwRoutineForRelation(frel, false);
	if (memcmp(routine, &pgstrom_arrow_fdw_routine, sizeof(FdwRoutine)) != 0)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("\"%s\" is not arrow_fdw foreign table",
						RelationGetRelationName(frel))));
	__arrowExecTruncateRelation(frel);

	table_close(frel, NoLock);

	PG_RETURN_VOID();
}
PG_FUNCTION_INFO_V1(pgstrom_arrow_fdw_truncate);

static void
__applyArrowTruncateRedoLog(arrowWriteRedoLog *redo, bool is_commit)
{
	char		backup[MAXPGPATH];

	snprintf(backup, MAXPGPATH, "%s.%u.backup",
			 redo->pathname, redo->suffix);
	if (is_commit)
	{
		elog(INFO, "unlink [%s]", backup);
		if (unlink(backup) != 0)
			ereport(WARNING,
					(errcode_for_file_access(),
					 errmsg("could not remove truncated file \"%s\": %m",
							backup),
					 errhint("remove the \"%s\" manually", backup)));
	}
	else
	{
		elog(INFO, "rename [%s]->[%s]", backup, redo->pathname);
		if (rename(backup, redo->pathname) != 0)
			ereport(WARNING,
					(errcode_for_file_access(),
					 errmsg("could not restore backup file \"%s\": %m",
							backup),
					 errhint("please restore \"%s\" to \"%s\" manually",
							 backup, redo->pathname)));
	}
}

static void
__applyArrowInsertRedoLog(arrowWriteRedoLog *redo, bool is_commit)
{
	int		fdesc;
	
	if (is_commit)
		return;

	/* special case, if it was an empty file */
	if (redo->footer_offset == 0 &&
		redo->footer_length == 0)
	{
		if (truncate(redo->pathname, 0) != 0)
			ereport(WARNING,
					(errcode_for_file_access(),
					 errmsg("failed on truncate('%s'): %m", redo->pathname),
					 errdetail("could not apply REDO image, therefore, garbages are still remained")));
		return;
	}

	fdesc = open(redo->pathname, O_RDWR);
	if (fdesc < 0)
	{
		ereport(WARNING,
				(errcode_for_file_access(),
				 errmsg("failed on open('%s'): %m", redo->pathname),
				 errdetail("could not apply REDO image, therefore, arrow file might be corrupted")));
	}
	else if (lseek(fdesc, redo->footer_offset, SEEK_SET) < 0)
	{
		ereport(WARNING,
				(errcode_for_file_access(),
				 errmsg("failed on lseek('%s'): %m", redo->pathname),
				 errdetail("could not apply REDO image, therefore, arrow file might be corrupted")));
	}
	else if (__write(fdesc,
					 redo->footer_backup,
					 redo->footer_length) != redo->footer_length)
	{
		ereport(WARNING,
				(errcode_for_file_access(),
				 errmsg("failed on write('%s'): %m", redo->pathname),
				 errdetail("could not apply REDO image, therefore, arrow file might be corrupted")));
	}
	else if (ftruncate(fdesc, (redo->footer_offset +
							   redo->footer_length)) != 0)
	{
		ereport(WARNING,
				(errcode_for_file_access(),
				 errmsg("failed on ftruncate('%s'): %m", redo->pathname),
				 errdetail("could not apply REDO image, therefore, arrow file might be corrupted")));
	}
	close(fdesc);

	elog(NOTICE, "arrow_fdw: REDO log applied (xid=%u, cid=%u, file=[%s], offset=%zu, length=%zu)", redo->xid, redo->cid, redo->pathname, redo->footer_offset, redo->footer_length);
}

static void
__cleanupArrowWriteMVCCLog(TransactionId curr_xid, dlist_head *mvcc_slot)
{
	dlist_mutable_iter iter;

	dlist_foreach_modify(iter, mvcc_slot)
	{
		arrowWriteMVCCLog *mvcc = dlist_container(arrowWriteMVCCLog,
												  chain, iter.cur);
		if (mvcc->xid == curr_xid)
		{
			dlist_delete(&mvcc->chain);
			elog(INFO, "mvcc st_dev=%u, st_ino=%u, xid=%u, cid=%u, record_batch=%u", (uint32)mvcc->key.st_dev, (uint32)mvcc->key.st_ino, (uint32)mvcc->xid, (uint32)mvcc->cid, mvcc->record_batch);
			pfree(mvcc);
		}
	}
}

/*
 * __arrowFdwXactCallback
 */
static void
__arrowFdwXactCallback(TransactionId curr_xid, bool is_commit)
{
	arrowWriteRedoLog  *redo;
	dlist_mutable_iter	iter;
	CommandId			curr_cid = InvalidCommandId;
	uint32				index;
	bool				locked[ARROW_METADATA_HASH_NSLOTS];
	LWLock			   *locks[ARROW_METADATA_HASH_NSLOTS];
	uint32				lcount = 0;

	if (curr_xid == InvalidTransactionId ||
		dlist_is_empty(&arrow_write_redo_list))
		return;

	memset(locked, 0, sizeof(locked));
	dlist_foreach_modify(iter, &arrow_write_redo_list)
	{
		redo = dlist_container(arrowWriteRedoLog, chain, iter.cur);
		if (redo->xid != curr_xid)
			continue;
		if (curr_cid != InvalidCommandId &&
			curr_cid < redo->cid)
			elog(WARNING, "Bug? Order of REDO log is not be correct. ABORT transaction might generate wrong image restored.");

		index = redo->key.hash % ARROW_METADATA_HASH_NSLOTS;
		if (!locked[index])
		{
			LWLock	   *lock = &arrow_metadata_state->lock_slots[index];
			dlist_head *slot = &arrow_metadata_state->mvcc_slots[index];

			LWLockAcquire(lock, LW_EXCLUSIVE);
			__cleanupArrowWriteMVCCLog(curr_xid, slot);
			locked[index] = true;
			locks[lcount++] = lock;
		}
		if (redo->is_truncate)
			__applyArrowTruncateRedoLog(redo, is_commit);
		else
			__applyArrowInsertRedoLog(redo, is_commit);

		dlist_delete(&redo->chain);
		pfree(redo);
	}

	for (index=0; index < lcount; index++)
		LWLockRelease(locks[index]);
}

/*
 * arrowFdwXactCallback
 */
static void
arrowFdwXactCallback(XactEvent event, void *arg)
{
	TransactionId	curr_xid = GetCurrentTransactionIdIfAny();

	if (event == XACT_EVENT_COMMIT)
		__arrowFdwXactCallback(curr_xid, true);
	else if (event == XACT_EVENT_ABORT)
		__arrowFdwXactCallback(curr_xid, false);
}

/*
 * arrowFdwSubXactCallback
 */
static void
arrowFdwSubXactCallback(SubXactEvent event, SubTransactionId mySubid,
						SubTransactionId parentSubid, void *arg)
{
	TransactionId	curr_xid = GetCurrentTransactionIdIfAny();

	if (event == SUBXACT_EVENT_COMMIT_SUB)
		__arrowFdwXactCallback(curr_xid, true);
	else if (event == SUBXACT_EVENT_ABORT_SUB)
		__arrowFdwXactCallback(curr_xid, false);
}

/*
 * pgstrom_startup_arrow_fdw
 */
static void
pgstrom_startup_arrow_fdw(void)
{
	bool	found;
	int		i;

	if (shmem_startup_next)
		(*shmem_startup_next)();

	arrow_metadata_state =
		ShmemInitStruct("arrow_metadata_state",
						MAXALIGN(sizeof(arrowMetadataState)),
						&found);
	if (!IsUnderPostmaster)
	{
		SpinLockInit(&arrow_metadata_state->lru_lock);
		dlist_init(&arrow_metadata_state->lru_list);
		pg_atomic_init_u64(&arrow_metadata_state->consumed, 0UL);
		for (i=0; i < ARROW_METADATA_HASH_NSLOTS; i++)
		{
			LWLockInitialize(&arrow_metadata_state->lock_slots[i], -1);
			dlist_init(&arrow_metadata_state->hash_slots[i]);
			dlist_init(&arrow_metadata_state->mvcc_slots[i]);
		}
	}
}

/*
 * pgstrom_init_arrow_fdw
 */
void
pgstrom_init_arrow_fdw(void)
{
	FdwRoutine *r = &pgstrom_arrow_fdw_routine;

	memset(r, 0, sizeof(FdwRoutine));
	NodeSetTag(r, T_FdwRoutine);
	/* SCAN support */
	r->GetForeignRelSize			= ArrowGetForeignRelSize;
	r->GetForeignPaths				= ArrowGetForeignPaths;
	r->GetForeignPlan				= ArrowGetForeignPlan;
	r->BeginForeignScan				= ArrowBeginForeignScan;
	r->IterateForeignScan			= ArrowIterateForeignScan;
	r->ReScanForeignScan			= ArrowReScanForeignScan;
	r->EndForeignScan				= ArrowEndForeignScan;
	/* EXPLAIN support */
	r->ExplainForeignScan			= ArrowExplainForeignScan;
	/* ANALYZE support */
	r->AnalyzeForeignTable			= ArrowAnalyzeForeignTable;
	/* IMPORT FOREIGN SCHEMA support */
	r->ImportForeignSchema			= ArrowImportForeignSchema;
	/* CPU Parallel support */
	r->IsForeignScanParallelSafe	= ArrowIsForeignScanParallelSafe;
	r->EstimateDSMForeignScan		= ArrowEstimateDSMForeignScan;
	r->InitializeDSMForeignScan		= ArrowInitializeDSMForeignScan;
	r->ReInitializeDSMForeignScan	= ArrowReInitializeDSMForeignScan;
	r->InitializeWorkerForeignScan	= ArrowInitializeWorkerForeignScan;
	r->ShutdownForeignScan			= ArrowShutdownForeignScan;
	/* INSERT/DELETE support */
	r->PlanForeignModify			= ArrowPlanForeignModify;
	r->BeginForeignModify			= ArrowBeginForeignModify;
	r->ExecForeignInsert			= ArrowExecForeignInsert;
	r->EndForeignModify				= ArrowEndForeignModify;
	r->ExplainForeignModify			= ArrowExplainForeignModify;

	/*
	 * Turn on/off arrow_fdw
	 */
	DefineCustomBoolVariable("arrow_fdw.enabled",
							 "Enables the planner's use of Arrow_Fdw",
							 NULL,
							 &arrow_fdw_enabled,
							 true,
							 PGC_USERSET,
							 GUC_NOT_IN_SAMPLE,
							 NULL, NULL, NULL);

	/*
	 * Configurations for arrow_fdw metadata cache
	 */
	DefineCustomIntVariable("arrow_fdw.metadata_cache_size",
							"size of shared metadata cache for arrow files",
							NULL,
							&arrow_metadata_cache_size_kb,
							131072,		/* 128MB */
							32768,		/* 32MB */
							INT_MAX,
							PGC_POSTMASTER,
							GUC_NOT_IN_SAMPLE | GUC_UNIT_KB,
							NULL, NULL, NULL);
	arrow_metadata_cache_size = (size_t)arrow_metadata_cache_size_kb << 10;

	/*
	 * Debug option to hint number of rows
	 */
	DefineCustomStringVariable("arrow_fdw.debug_row_numbers_hint",
							   "override number of rows estimation for arrow_fdw foreign tables",
							   NULL,
							   &arrow_debug_row_numbers_hint,
							   NULL,
							   PGC_USERSET,
							   GUC_NOT_IN_SAMPLE | GUC_NO_SHOW_ALL,
							   NULL, NULL, NULL);

	/*
	 * Limit of RecordBatch size for writing
	 */
	DefineCustomIntVariable("arrow_fdw.record_batch_size",
							"maximum size of record batch on writing",
							NULL,
							&arrow_record_batch_size_kb,
							256 * 1024,		/* default: 256MB */
							4 * 1024,		/* min: 4MB */
							2048 * 1024,	/* max: 2GB */
							PGC_USERSET,
							GUC_NOT_IN_SAMPLE | GUC_UNIT_KB,
							NULL, NULL, NULL);

	/* shared memory size */
	RequestAddinShmemSpace(MAXALIGN(sizeof(arrowMetadataState)));
	shmem_startup_next = shmem_startup_hook;
	shmem_startup_hook = pgstrom_startup_arrow_fdw;

	/* arrow transaction callback */
	RegisterXactCallback(arrowFdwXactCallback, NULL);
	RegisterSubXactCallback(arrowFdwSubXactCallback, NULL);

	/* misc init */
	dlist_init(&arrow_write_redo_list);
}
