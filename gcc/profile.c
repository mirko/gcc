/* Calculate branch probabilities, and basic block execution counts.
   Copyright (C) 1990, 1991, 1992, 1993, 1994, 1996, 1997, 1998, 1999,
   2000, 2001  Free Software Foundation, Inc.
   Contributed by James E. Wilson, UC Berkeley/Cygnus Support;
   based on some ideas from Dain Samples of UC Berkeley.
   Further mangling by Bob Manson, Cygnus Support.

This file is part of GCC.

GCC is free software; you can redistribute it and/or modify it under
the terms of the GNU General Public License as published by the Free
Software Foundation; either version 2, or (at your option) any later
version.

GCC is distributed in the hope that it will be useful, but WITHOUT ANY
WARRANTY; without even the implied warranty of MERCHANTABILITY or
FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
for more details.

You should have received a copy of the GNU General Public License
along with GCC; see the file COPYING.  If not, write to the Free
Software Foundation, 59 Temple Place - Suite 330, Boston, MA
02111-1307, USA.  */

/* Generate basic block profile instrumentation and auxiliary files.
   Profile generation is optimized, so that not all arcs in the basic
   block graph need instrumenting. First, the BB graph is closed with
   one entry (function start), and one exit (function exit).  Any
   ABNORMAL_EDGE cannot be instrumented (because there is no control
   path to place the code). We close the graph by inserting fake
   EDGE_FAKE edges to the EXIT_BLOCK, from the sources of abnormal
   edges that do not go to the exit_block. We ignore such abnormal
   edges.  Naturally these fake edges are never directly traversed,
   and so *cannot* be directly instrumented.  Some other graph
   massaging is done. To optimize the instrumentation we generate the
   BB minimal span tree, only edges that are not on the span tree
   (plus the entry point) need instrumenting. From that information
   all other edge counts can be deduced.  By construction all fake
   edges must be on the spanning tree. We also attempt to place
   EDGE_CRITICAL edges on the spanning tree.

   The auxiliary file generated is <dumpbase>.bbg. The format is
   described in full in gcov-io.h.  */

/* ??? Register allocation should use basic block execution counts to
   give preference to the most commonly executed blocks.  */

/* ??? Should calculate branch probabilities before instrumenting code, since
   then we can use arc counts to help decide which arcs to instrument.  */

#include "config.h"
#include "system.h"
#include "coretypes.h"
#include "tm.h"
#include "rtl.h"
#include "tree.h"
#include "flags.h"
#include "insn-config.h"
#include "output.h"
#include "regs.h"
#include "expr.h"
#include "function.h"
#include "toplev.h"
#include "ggc.h"
#include "hard-reg-set.h"
#include "basic-block.h"
#include "gcov-io.h"
#include "target.h"
#include "profile.h"
#include "libfuncs.h"
#include "langhooks.h"
#include "hashtab.h"

/* Additional information about the edges we need.  */
struct edge_info {
  unsigned int count_valid : 1;
  
  /* Is on the spanning tree.  */
  unsigned int on_tree : 1;
  
  /* Pretend this edge does not exist (it is abnormal and we've
     inserted a fake to compensate).  */
  unsigned int ignore : 1;
};

struct bb_info {
  unsigned int count_valid : 1;

  /* Number of successor and predecessor edges.  */
  gcov_type succ_count;
  gcov_type pred_count;
};

struct function_list
{
  struct function_list *next; 	/* next function */
  const char *name; 		/* function name */
  unsigned cfg_checksum;	/* function checksum */
  unsigned n_counter_sections;	/* number of counter sections */
  struct counter_section counter_sections[MAX_COUNTER_SECTIONS];
  				/* the sections */
};


/* Counts information for a function.  */
typedef struct counts_entry
{
  /* We hash by  */
  char *function_name;
  unsigned section;
  
  /* Store  */
  unsigned checksum;
  unsigned n_counts;
  gcov_type *counts;
  unsigned merged;
  gcov_type max_counter;
  gcov_type max_counter_sum;

  /* Workspace */
  struct counts_entry *chain;
  
} counts_entry_t;

static struct function_list *functions_head = 0;
static struct function_list **functions_tail = &functions_head;

#define EDGE_INFO(e)  ((struct edge_info *) (e)->aux)
#define BB_INFO(b)  ((struct bb_info *) (b)->aux)

/* Keep all basic block indexes nonnegative in the gcov output.  Index 0
   is used for entry block, last block exit block.  */
#define BB_TO_GCOV_INDEX(bb)  ((bb) == ENTRY_BLOCK_PTR ? 0		\
			       : ((bb) == EXIT_BLOCK_PTR		\
				  ? last_basic_block + 1 : (bb)->index + 1))

/* Instantiate the profile info structure.  */

struct profile_info profile_info;

/* Name and file pointer of the output file for the basic block graph.  */

static char *bbg_file_name;

/* Name and file pointer of the input file for the arc count data.  */

static char *da_file_name;

/* The name of the count table. Used by the edge profiling code.  */
static GTY(()) rtx profiler_label;

/* Collect statistics on the performance of this pass for the entire source
   file.  */

static int total_num_blocks;
static int total_num_edges;
static int total_num_edges_ignored;
static int total_num_edges_instrumented;
static int total_num_blocks_created;
static int total_num_passes;
static int total_num_times_called;
static int total_hist_br_prob[20];
static int total_num_never_executed;
static int total_num_branches;

/* Forward declarations.  */
static void find_spanning_tree PARAMS ((struct edge_list *));
static rtx gen_edge_profiler PARAMS ((int));
static void instrument_edges PARAMS ((struct edge_list *));
static void compute_branch_probabilities PARAMS ((void));
static hashval_t htab_counts_entry_hash PARAMS ((const void *));
static int htab_counts_entry_eq PARAMS ((const void *, const void *));
static void htab_counts_entry_del PARAMS ((void *));
static void read_counts_file PARAMS ((const char *));
static gcov_type * get_exec_counts PARAMS ((void));
static unsigned compute_checksum PARAMS ((void));
static basic_block find_group PARAMS ((basic_block));
static void union_groups PARAMS ((basic_block, basic_block));
static void set_purpose PARAMS ((tree, tree));
static rtx label_for_tag PARAMS ((unsigned));
static tree build_counter_section_fields PARAMS ((void));
static tree build_counter_section_value PARAMS ((unsigned, unsigned));
static tree build_counter_section_data_fields PARAMS ((void));
static tree build_counter_section_data_value PARAMS ((unsigned, unsigned));
static tree build_function_info_fields PARAMS ((void));
static tree build_function_info_value PARAMS ((struct function_list *));
static tree build_gcov_info_fields PARAMS ((tree));
static tree build_gcov_info_value PARAMS ((void));


/* Add edge instrumentation code to the entire insn chain.

   F is the first insn of the chain.
   NUM_BLOCKS is the number of basic blocks found in F.  */

static void
instrument_edges (el)
     struct edge_list *el;
{
  int num_instr_edges = 0;
  int num_edges = NUM_EDGES (el);
  basic_block bb;
  struct section_info *section_info;
  remove_fake_edges ();

  FOR_BB_BETWEEN (bb, ENTRY_BLOCK_PTR, NULL, next_bb)
    {
      edge e = bb->succ;
      while (e)
	{
	  struct edge_info *inf = EDGE_INFO (e);
	  if (!inf->ignore && !inf->on_tree)
	    {
	      if (e->flags & EDGE_ABNORMAL)
		abort ();
	      if (rtl_dump_file)
		fprintf (rtl_dump_file, "Edge %d to %d instrumented%s\n",
			 e->src->index, e->dest->index,
			 EDGE_CRITICAL_P (e) ? " (and split)" : "");
	      insert_insn_on_edge (
			 gen_edge_profiler (total_num_edges_instrumented
					    + num_instr_edges++), e);
	      rebuild_jump_labels (e->insns);
	    }
	  e = e->succ_next;
	}
    }

  section_info = find_counters_section (GCOV_TAG_ARC_COUNTS);
  section_info->n_counters_now = num_instr_edges;
  total_num_edges_instrumented += num_instr_edges;
  section_info->n_counters = total_num_edges_instrumented;

  total_num_blocks_created += num_edges;
  if (rtl_dump_file)
    fprintf (rtl_dump_file, "%d edges instrumented\n", num_instr_edges);
}

static hashval_t
htab_counts_entry_hash (of)
     const void *of;
{
  const counts_entry_t *entry = of;

  return htab_hash_string (entry->function_name) ^ entry->section;
}

static int
htab_counts_entry_eq (of1, of2)
     const void *of1;
     const void *of2;
{
  const counts_entry_t *entry1 = of1;
  const counts_entry_t *entry2 = of2;

  return !strcmp (entry1->function_name, entry2->function_name)
    && entry1->section == entry2->section;
}

static void
htab_counts_entry_del (of)
     void *of;
{
  counts_entry_t *entry = of;

  free (entry->function_name);
  free (entry->counts);
  free (entry);
}

static htab_t counts_hash = NULL;

static void
read_counts_file (const char *name)
{
  char *function_name_buffer = NULL;
  unsigned version, ix, checksum;
  counts_entry_t *summaried = NULL;
  unsigned seen_summary = 0;
  
  if (!gcov_open (name, 1))
    {
      warning ("file %s not found, execution counts assumed to be zero", name);
      return;
    }
  
  if (gcov_read_unsigned () != GCOV_DATA_MAGIC)
    {
      warning ("`%s' is not a gcov data file", name);
      gcov_close ();
      return;
    }
  else if ((version = gcov_read_unsigned ()) != GCOV_VERSION)
    {
      char v[4], e[4];
      unsigned required = GCOV_VERSION;
      
      for (ix = 4; ix--; required >>= 8, version >>= 8)
	{
	  v[ix] = version;
	  e[ix] = required;
	}
      warning ("`%s' is version `%.4s', expected version `%.4s'", name, v, e);
      gcov_close ();
      return;
    }
  
  counts_hash = htab_create (10,
			     htab_counts_entry_hash, htab_counts_entry_eq,
			     htab_counts_entry_del);
  while (!gcov_is_eof ())
    {
      unsigned tag, length;
      unsigned long offset;
      int error;
      
      tag = gcov_read_unsigned ();
      length = gcov_read_unsigned ();
      offset = gcov_position ();
      if (tag == GCOV_TAG_FUNCTION)
	{
	  const char *string = gcov_read_string ();
	  free (function_name_buffer);
	  function_name_buffer = string ? xstrdup (string) : NULL;
	  checksum = gcov_read_unsigned ();
	  if (seen_summary)
	    {
	      /* We have already seen a summary, this means that this
		 new function begins a new set of program runs. We
		 must unlink the summaried chain.  */
	      counts_entry_t *entry, *chain;
	      
	      for (entry = summaried; entry; entry = chain)
		{
		  chain = entry->chain;
		  
		  entry->max_counter_sum += entry->max_counter;
		  entry->chain = NULL;
		}
	      summaried = NULL;
	      seen_summary = 0;
	    }
	}
      else if (tag == GCOV_TAG_PROGRAM_SUMMARY)
	{
	  counts_entry_t *entry;
	  struct gcov_summary summary;
	  
	  gcov_read_summary (&summary);
	  seen_summary = 1;
	  for (entry = summaried; entry; entry = entry->chain)
	    {
	      entry->merged += summary.runs;
	      if (entry->max_counter < summary.arc_sum_max)
		entry->max_counter = summary.arc_sum_max;
	    }
	}
      else if (GCOV_TAG_IS_SUBTAG (GCOV_TAG_FUNCTION, tag)
	       && function_name_buffer)
	{
	  counts_entry_t **slot, *entry, elt;
	  unsigned n_counts = length / 8;
	  unsigned ix;

	  elt.function_name = function_name_buffer;
	  elt.section = tag;

	  slot = (counts_entry_t **) htab_find_slot
	    (counts_hash, &elt, INSERT);
	  entry = *slot;
	  if (!entry)
	    {
	      *slot = entry = xmalloc (sizeof (counts_entry_t));
	      entry->function_name = xstrdup (function_name_buffer);
	      entry->section = tag;
	      entry->checksum = checksum;
	      entry->n_counts = n_counts;
	      entry->counts = xcalloc (n_counts, sizeof (gcov_type));
	    }
	  else if (entry->checksum != checksum || entry->n_counts != n_counts)
	    {
	      warning ("profile mismatch for `%s'", function_name_buffer);
	      htab_delete (counts_hash);
	      break;
	    }
	  
	  /* This should always be true for a just allocated entry,
	     and always false for an existing one. Check this way, in
	     case the gcov file is corrupt.  */
	  if (!entry->chain || summaried != entry)
	    {
	      entry->chain = summaried;
	      summaried = entry;
	    }
	  for (ix = 0; ix != n_counts; ix++)
	    entry->counts[ix] += gcov_read_counter ();
	}
      gcov_seek (offset, length);
      if ((error = gcov_is_error ()))
	{
	  warning (error < 0 ? "`%s' has overflowed" : "`%s' is corrupted",
		   name);
	  htab_delete (counts_hash);
	  break;
	}
    }

  free (function_name_buffer);
  gcov_close ();
}

/* Computes hybrid profile for all matching entries in da_file.
   Sets max_counter_in_program as a side effect.  */

static gcov_type *
get_exec_counts ()
{
  unsigned num_edges = 0;
  basic_block bb;
  const char *name = IDENTIFIER_POINTER (DECL_ASSEMBLER_NAME (current_function_decl));
  counts_entry_t *entry, elt;

  profile_info.max_counter_in_program = 0;
  profile_info.count_profiles_merged = 0;

  /* No hash table, no counts. */
  if (!counts_hash)
    return NULL;

  /* Count the edges to be (possibly) instrumented.  */
  FOR_BB_BETWEEN (bb, ENTRY_BLOCK_PTR, NULL, next_bb)
    {
      edge e;
      for (e = bb->succ; e; e = e->succ_next)
	if (!EDGE_INFO (e)->ignore && !EDGE_INFO (e)->on_tree)
	  num_edges++;
    }

  elt.function_name = (char *) name;
  elt.section = GCOV_TAG_ARC_COUNTS;
  entry = htab_find (counts_hash, &elt);
  if (!entry)
    {
      warning ("No profile for function '%s' found.", name);
      return NULL;
    }
  
  if (entry->checksum != profile_info.current_function_cfg_checksum
      || num_edges != entry->n_counts)
    {
      warning ("profile mismatch for `%s'", current_function_name);
      return NULL;
    }

  profile_info.count_profiles_merged = entry->merged;
  profile_info.max_counter_in_program = entry->max_counter_sum;

  if (rtl_dump_file)
    {
      fprintf(rtl_dump_file, "Merged %i profiles with maximal count %i.\n",
	      profile_info.count_profiles_merged,
	      (int)profile_info.max_counter_in_program);
    }

  return entry->counts;
}


/* Compute the branch probabilities for the various branches.
   Annotate them accordingly.  */

static void
compute_branch_probabilities ()
{
  basic_block bb;
  int i;
  int num_edges = 0;
  int changes;
  int passes;
  int hist_br_prob[20];
  int num_never_executed;
  int num_branches;
  gcov_type *exec_counts = get_exec_counts ();
  int exec_counts_pos = 0;

  /* Attach extra info block to each bb.  */

  alloc_aux_for_blocks (sizeof (struct bb_info));
  FOR_BB_BETWEEN (bb, ENTRY_BLOCK_PTR, NULL, next_bb)
    {
      edge e;

      for (e = bb->succ; e; e = e->succ_next)
	if (!EDGE_INFO (e)->ignore)
	  BB_INFO (bb)->succ_count++;
      for (e = bb->pred; e; e = e->pred_next)
	if (!EDGE_INFO (e)->ignore)
	  BB_INFO (bb)->pred_count++;
    }

  /* Avoid predicting entry on exit nodes.  */
  BB_INFO (EXIT_BLOCK_PTR)->succ_count = 2;
  BB_INFO (ENTRY_BLOCK_PTR)->pred_count = 2;

  /* For each edge not on the spanning tree, set its execution count from
     the .da file.  */

  /* The first count in the .da file is the number of times that the function
     was entered.  This is the exec_count for block zero.  */

  FOR_BB_BETWEEN (bb, ENTRY_BLOCK_PTR, NULL, next_bb)
    {
      edge e;
      for (e = bb->succ; e; e = e->succ_next)
	if (!EDGE_INFO (e)->ignore && !EDGE_INFO (e)->on_tree)
	  {
	    num_edges++;
	    if (exec_counts)
	      {
		e->count = exec_counts[exec_counts_pos++];
	      }
	    else
	      e->count = 0;

	    EDGE_INFO (e)->count_valid = 1;
	    BB_INFO (bb)->succ_count--;
	    BB_INFO (e->dest)->pred_count--;
	    if (rtl_dump_file)
	      {
		fprintf (rtl_dump_file, "\nRead edge from %i to %i, count:",
			 bb->index, e->dest->index);
		fprintf (rtl_dump_file, HOST_WIDEST_INT_PRINT_DEC,
			 (HOST_WIDEST_INT) e->count);
	      }
	  }
    }

  if (rtl_dump_file)
    fprintf (rtl_dump_file, "\n%d edge counts read\n", num_edges);

  /* For every block in the file,
     - if every exit/entrance edge has a known count, then set the block count
     - if the block count is known, and every exit/entrance edge but one has
     a known execution count, then set the count of the remaining edge

     As edge counts are set, decrement the succ/pred count, but don't delete
     the edge, that way we can easily tell when all edges are known, or only
     one edge is unknown.  */

  /* The order that the basic blocks are iterated through is important.
     Since the code that finds spanning trees starts with block 0, low numbered
     edges are put on the spanning tree in preference to high numbered edges.
     Hence, most instrumented edges are at the end.  Graph solving works much
     faster if we propagate numbers from the end to the start.

     This takes an average of slightly more than 3 passes.  */

  changes = 1;
  passes = 0;
  while (changes)
    {
      passes++;
      changes = 0;
      FOR_BB_BETWEEN (bb, EXIT_BLOCK_PTR, NULL, prev_bb)
	{
	  struct bb_info *bi = BB_INFO (bb);
	  if (! bi->count_valid)
	    {
	      if (bi->succ_count == 0)
		{
		  edge e;
		  gcov_type total = 0;

		  for (e = bb->succ; e; e = e->succ_next)
		    total += e->count;
		  bb->count = total;
		  bi->count_valid = 1;
		  changes = 1;
		}
	      else if (bi->pred_count == 0)
		{
		  edge e;
		  gcov_type total = 0;

		  for (e = bb->pred; e; e = e->pred_next)
		    total += e->count;
		  bb->count = total;
		  bi->count_valid = 1;
		  changes = 1;
		}
	    }
	  if (bi->count_valid)
	    {
	      if (bi->succ_count == 1)
		{
		  edge e;
		  gcov_type total = 0;

		  /* One of the counts will be invalid, but it is zero,
		     so adding it in also doesn't hurt.  */
		  for (e = bb->succ; e; e = e->succ_next)
		    total += e->count;

		  /* Seedgeh for the invalid edge, and set its count.  */
		  for (e = bb->succ; e; e = e->succ_next)
		    if (! EDGE_INFO (e)->count_valid && ! EDGE_INFO (e)->ignore)
		      break;

		  /* Calculate count for remaining edge by conservation.  */
		  total = bb->count - total;

		  if (! e)
		    abort ();
		  EDGE_INFO (e)->count_valid = 1;
		  e->count = total;
		  bi->succ_count--;

		  BB_INFO (e->dest)->pred_count--;
		  changes = 1;
		}
	      if (bi->pred_count == 1)
		{
		  edge e;
		  gcov_type total = 0;

		  /* One of the counts will be invalid, but it is zero,
		     so adding it in also doesn't hurt.  */
		  for (e = bb->pred; e; e = e->pred_next)
		    total += e->count;

		  /* Seedgeh for the invalid edge, and set its count.  */
		  for (e = bb->pred; e; e = e->pred_next)
		    if (! EDGE_INFO (e)->count_valid && ! EDGE_INFO (e)->ignore)
		      break;

		  /* Calculate count for remaining edge by conservation.  */
		  total = bb->count - total + e->count;

		  if (! e)
		    abort ();
		  EDGE_INFO (e)->count_valid = 1;
		  e->count = total;
		  bi->pred_count--;

		  BB_INFO (e->src)->succ_count--;
		  changes = 1;
		}
	    }
	}
    }
  if (rtl_dump_file)
    dump_flow_info (rtl_dump_file);

  total_num_passes += passes;
  if (rtl_dump_file)
    fprintf (rtl_dump_file, "Graph solving took %d passes.\n\n", passes);

  /* If the graph has been correctly solved, every block will have a
     succ and pred count of zero.  */
  FOR_EACH_BB (bb)
    {
      if (BB_INFO (bb)->succ_count || BB_INFO (bb)->pred_count)
	abort ();
    }

  /* For every edge, calculate its branch probability and add a reg_note
     to the branch insn to indicate this.  */

  for (i = 0; i < 20; i++)
    hist_br_prob[i] = 0;
  num_never_executed = 0;
  num_branches = 0;

  FOR_BB_BETWEEN (bb, ENTRY_BLOCK_PTR, NULL, next_bb)
    {
      edge e;
      gcov_type total;
      rtx note;

      total = bb->count;
      if (total)
	{
	  for (e = bb->succ; e; e = e->succ_next)
	    {
		e->probability = (e->count * REG_BR_PROB_BASE + total / 2) / total;
		if (e->probability < 0 || e->probability > REG_BR_PROB_BASE)
		  {
		    error ("corrupted profile info: prob for %d-%d thought to be %d",
			   e->src->index, e->dest->index, e->probability);
		    e->probability = REG_BR_PROB_BASE / 2;
		  }
	    }
	  if (bb->index >= 0
	      && any_condjump_p (bb->end)
	      && bb->succ->succ_next)
	    {
	      int prob;
	      edge e;
	      int index;

	      /* Find the branch edge.  It is possible that we do have fake
		 edges here.  */
	      for (e = bb->succ; e->flags & (EDGE_FAKE | EDGE_FALLTHRU);
		   e = e->succ_next)
		continue; /* Loop body has been intentionally left blank.  */

	      prob = e->probability;
	      index = prob * 20 / REG_BR_PROB_BASE;

	      if (index == 20)
		index = 19;
	      hist_br_prob[index]++;

	      note = find_reg_note (bb->end, REG_BR_PROB, 0);
	      /* There may be already note put by some other pass, such
		 as builtin_expect expander.  */
	      if (note)
		XEXP (note, 0) = GEN_INT (prob);
	      else
		REG_NOTES (bb->end)
		  = gen_rtx_EXPR_LIST (REG_BR_PROB, GEN_INT (prob),
				       REG_NOTES (bb->end));
	      num_branches++;
	    }
	}
      /* Otherwise distribute the probabilities evenly so we get sane
	 sum.  Use simple heuristics that if there are normal edges,
	 give all abnormals frequency of 0, otherwise distribute the
	 frequency over abnormals (this is the case of noreturn
	 calls).  */
      else
	{
	  for (e = bb->succ; e; e = e->succ_next)
	    if (!(e->flags & (EDGE_COMPLEX | EDGE_FAKE)))
	      total ++;
	  if (total)
	    {
	      for (e = bb->succ; e; e = e->succ_next)
		if (!(e->flags & (EDGE_COMPLEX | EDGE_FAKE)))
		  e->probability = REG_BR_PROB_BASE / total;
		else
		  e->probability = 0;
	    }
	  else
	    {
	      for (e = bb->succ; e; e = e->succ_next)
		total ++;
	      for (e = bb->succ; e; e = e->succ_next)
		e->probability = REG_BR_PROB_BASE / total;
	    }
	  if (bb->index >= 0
	      && any_condjump_p (bb->end)
	      && bb->succ->succ_next)
	    num_branches++, num_never_executed;
	}
    }

  if (rtl_dump_file)
    {
      fprintf (rtl_dump_file, "%d branches\n", num_branches);
      fprintf (rtl_dump_file, "%d branches never executed\n",
	       num_never_executed);
      if (num_branches)
	for (i = 0; i < 10; i++)
	  fprintf (rtl_dump_file, "%d%% branches in range %d-%d%%\n",
		   (hist_br_prob[i] + hist_br_prob[19-i]) * 100 / num_branches,
		   5 * i, 5 * i + 5);

      total_num_branches += num_branches;
      total_num_never_executed += num_never_executed;
      for (i = 0; i < 20; i++)
	total_hist_br_prob[i] += hist_br_prob[i];

      fputc ('\n', rtl_dump_file);
      fputc ('\n', rtl_dump_file);
    }

  free_aux_for_blocks ();
  find_counters_section (GCOV_TAG_ARC_COUNTS)->present = 1;
}

/* Compute checksum for the current function.  We generate a CRC32.  */

static unsigned
compute_checksum ()
{
  unsigned chksum = 0;
  basic_block bb;
  
  FOR_EACH_BB (bb)
    {
      edge e = NULL;
      
      do
	{
	  unsigned value = BB_TO_GCOV_INDEX (e ? e->dest : bb);
	  unsigned ix;

	  /* No need to use all bits in value identically, nearly all
	     functions have less than 256 blocks.  */
	  value ^= value << 16;
	  value ^= value << 8;
	  
	  for (ix = 8; ix--; value <<= 1)
	    {
	      unsigned feedback;

	      feedback = (value ^ chksum) & 0x80000000 ? 0x04c11db7 : 0;
	      chksum <<= 1;
	      chksum ^= feedback;
	    }
	  
	  e = e ? e->succ_next : bb->succ;
	}
      while (e);
    }

  return chksum;
}

/* Instrument and/or analyze program behavior based on program flow graph.
   In either case, this function builds a flow graph for the function being
   compiled.  The flow graph is stored in BB_GRAPH.

   When FLAG_PROFILE_ARCS is nonzero, this function instruments the edges in
   the flow graph that are needed to reconstruct the dynamic behavior of the
   flow graph.

   When FLAG_BRANCH_PROBABILITIES is nonzero, this function reads auxiliary
   information from a data file containing edge count information from previous
   executions of the function being compiled.  In this case, the flow graph is
   annotated with actual execution counts, which are later propagated into the
   rtl for optimization purposes.

   Main entry point of this file.  */

void
branch_prob ()
{
  basic_block bb;
  unsigned i;
  unsigned num_edges, ignored_edges;
  struct edge_list *el;
  const char *name = IDENTIFIER_POINTER
    (DECL_ASSEMBLER_NAME (current_function_decl));

  profile_info.current_function_cfg_checksum = compute_checksum ();
  for (i = 0; i < profile_info.n_sections; i++)
    {
      profile_info.section_info[i].n_counters_now = 0;
      profile_info.section_info[i].present = 0;
    }

  if (rtl_dump_file)
    fprintf (rtl_dump_file, "CFG checksum is %u\n",
	profile_info.current_function_cfg_checksum);

  total_num_times_called++;

  flow_call_edges_add (NULL);
  add_noreturn_fake_exit_edges ();

  /* We can't handle cyclic regions constructed using abnormal edges.
     To avoid these we replace every source of abnormal edge by a fake
     edge from entry node and every destination by fake edge to exit.
     This keeps graph acyclic and our calculation exact for all normal
     edges except for exit and entrance ones.

     We also add fake exit edges for each call and asm statement in the
     basic, since it may not return.  */

  FOR_EACH_BB (bb)
    {
      int need_exit_edge = 0, need_entry_edge = 0;
      int have_exit_edge = 0, have_entry_edge = 0;
      rtx insn;
      edge e;

      /* Add fake edges from entry block to the call insns that may return
	 twice.  The CFG is not quite correct then, as call insn plays more
	 role of CODE_LABEL, but for our purposes, everything should be OK,
	 as we never insert code to the beginning of basic block.  */
      for (insn = bb->head; insn != NEXT_INSN (bb->end);
	   insn = NEXT_INSN (insn))
	{
	  if (GET_CODE (insn) == CALL_INSN
	      && find_reg_note (insn, REG_SETJMP, NULL))
	    {
	      if (GET_CODE (bb->head) == CODE_LABEL
		  || insn != NEXT_INSN (bb->head))
		{
		  e = split_block (bb, PREV_INSN (insn));
		  make_edge (ENTRY_BLOCK_PTR, e->dest, EDGE_FAKE);
		  break;
		}
	      else
		{
		  /* We should not get abort here, as call to setjmp should not
		     be the very first instruction of function.  */
		  if (bb == ENTRY_BLOCK_PTR)
		    abort ();
		  make_edge (ENTRY_BLOCK_PTR, bb, EDGE_FAKE);
		}
	    }
	}

      for (e = bb->succ; e; e = e->succ_next)
	{
	  if ((e->flags & (EDGE_ABNORMAL | EDGE_ABNORMAL_CALL))
	       && e->dest != EXIT_BLOCK_PTR)
	    need_exit_edge = 1;
	  if (e->dest == EXIT_BLOCK_PTR)
	    have_exit_edge = 1;
	}
      for (e = bb->pred; e; e = e->pred_next)
	{
	  if ((e->flags & (EDGE_ABNORMAL | EDGE_ABNORMAL_CALL))
	       && e->src != ENTRY_BLOCK_PTR)
	    need_entry_edge = 1;
	  if (e->src == ENTRY_BLOCK_PTR)
	    have_entry_edge = 1;
	}

      if (need_exit_edge && !have_exit_edge)
	{
	  if (rtl_dump_file)
	    fprintf (rtl_dump_file, "Adding fake exit edge to bb %i\n",
		     bb->index);
	  make_edge (bb, EXIT_BLOCK_PTR, EDGE_FAKE);
	}
      if (need_entry_edge && !have_entry_edge)
	{
	  if (rtl_dump_file)
	    fprintf (rtl_dump_file, "Adding fake entry edge to bb %i\n",
		     bb->index);
	  make_edge (ENTRY_BLOCK_PTR, bb, EDGE_FAKE);
	}
    }

  el = create_edge_list ();
  num_edges = NUM_EDGES (el);
  alloc_aux_for_edges (sizeof (struct edge_info));

  /* The basic blocks are expected to be numbered sequentially.  */
  compact_blocks ();

  ignored_edges = 0;
  for (i = 0 ; i < num_edges ; i++)
    {
      edge e = INDEX_EDGE (el, i);
      e->count = 0;

      /* Mark edges we've replaced by fake edges above as ignored.  */
      if ((e->flags & (EDGE_ABNORMAL | EDGE_ABNORMAL_CALL))
	  && e->src != ENTRY_BLOCK_PTR && e->dest != EXIT_BLOCK_PTR)
	{
	  EDGE_INFO (e)->ignore = 1;
	  ignored_edges++;
	}
    }

#ifdef ENABLE_CHECKING
  verify_flow_info ();
#endif

  /* Create spanning tree from basic block graph, mark each edge that is
     on the spanning tree.  We insert as many abnormal and critical edges
     as possible to minimize number of edge splits necessary.  */

  find_spanning_tree (el);

  /* Fake edges that are not on the tree will not be instrumented, so
     mark them ignored.  */
  for (i = 0; i < num_edges; i++)
    {
      edge e = INDEX_EDGE (el, i);
      struct edge_info *inf = EDGE_INFO (e);
      if ((e->flags & EDGE_FAKE) && !inf->ignore && !inf->on_tree)
	{
	  inf->ignore = 1;
	  ignored_edges++;
	}
    }

  total_num_blocks += n_basic_blocks + 2;
  if (rtl_dump_file)
    fprintf (rtl_dump_file, "%d basic blocks\n", n_basic_blocks);

  total_num_edges += num_edges;
  if (rtl_dump_file)
    fprintf (rtl_dump_file, "%d edges\n", num_edges);

  total_num_edges_ignored += ignored_edges;
  if (rtl_dump_file)
    fprintf (rtl_dump_file, "%d ignored edges\n", ignored_edges);

  /* Create a .bbg file from which gcov can reconstruct the basic block
     graph.  First output the number of basic blocks, and then for every
     edge output the source and target basic block numbers.
     NOTE: The format of this file must be compatible with gcov.  */

  if (!gcov_is_error ())
    {
      long offset;
      const char *file = DECL_SOURCE_FILE (current_function_decl);
      unsigned line = DECL_SOURCE_LINE (current_function_decl);
      
      /* Announce function */
      offset = gcov_write_tag (GCOV_TAG_FUNCTION);
      gcov_write_string (name);
      gcov_write_unsigned (profile_info.current_function_cfg_checksum);
      gcov_write_string (file);
      gcov_write_unsigned (line);
      gcov_write_length (offset);

      /* Basic block flags */
      offset = gcov_write_tag (GCOV_TAG_BLOCKS);
      for (i = 0; i != (unsigned) (n_basic_blocks + 2); i++)
	gcov_write_unsigned (0);
      gcov_write_length (offset);
      
      /* Arcs */
      FOR_BB_BETWEEN (bb, ENTRY_BLOCK_PTR, EXIT_BLOCK_PTR, next_bb)
	{
	  edge e;

	  offset = gcov_write_tag (GCOV_TAG_ARCS);
	  gcov_write_unsigned (BB_TO_GCOV_INDEX (bb));
	  
	  for (e = bb->succ; e; e = e->succ_next)
	    {
	      struct edge_info *i = EDGE_INFO (e);
	      if (!i->ignore)
		{
		  unsigned flag_bits = 0;
		  
		  if (i->on_tree)
		    flag_bits |= GCOV_ARC_ON_TREE;
		  if (e->flags & EDGE_FAKE)
		    flag_bits |= GCOV_ARC_FAKE;
		  if (e->flags & EDGE_FALLTHRU)
		    flag_bits |= GCOV_ARC_FALLTHROUGH;

		  gcov_write_unsigned (BB_TO_GCOV_INDEX (e->dest));
		  gcov_write_unsigned (flag_bits);
	        }
	    }

	  gcov_write_length (offset);
	}

      /* Output line number information about each basic block for
     	 GCOV utility.  */
      {
	char const *prev_file_name = NULL;
	
	FOR_EACH_BB (bb)
	  {
	    rtx insn = bb->head;
	    int ignore_next_note = 0;
	    
	    offset = 0;
	    
	    /* We are looking for line number notes.  Search backward
	       before basic block to find correct ones.  */
	    insn = prev_nonnote_insn (insn);
	    if (!insn)
	      insn = get_insns ();
	    else
	      insn = NEXT_INSN (insn);

	    while (insn != bb->end)
	      {
		if (GET_CODE (insn) == NOTE)
		  {
		     /* Must ignore the line number notes that immediately
		     	follow the end of an inline function to avoid counting
		     	it twice.  There is a note before the call, and one
		     	after the call.  */
		    if (NOTE_LINE_NUMBER (insn)
			== NOTE_INSN_REPEATED_LINE_NUMBER)
		      ignore_next_note = 1;
		    else if (NOTE_LINE_NUMBER (insn) <= 0)
		      /*NOP*/;
		    else if (ignore_next_note)
		      ignore_next_note = 0;
		    else
		      {
			if (!offset)
			  {
			    offset = gcov_write_tag (GCOV_TAG_LINES);
			    gcov_write_unsigned (BB_TO_GCOV_INDEX (bb));
			  }

			/* If this is a new source file, then output
			   the file's name to the .bb file.  */
			if (!prev_file_name
			    || strcmp (NOTE_SOURCE_FILE (insn),
				       prev_file_name))
			  {
			    prev_file_name = NOTE_SOURCE_FILE (insn);
			    gcov_write_unsigned (0);
			    gcov_write_string (prev_file_name);
			  }
			gcov_write_unsigned (NOTE_LINE_NUMBER (insn));
		      }
		  }
		insn = NEXT_INSN (insn);
	      }

	    if (offset)
	      {
		/* A file of NULL indicates the end of run.  */
		gcov_write_unsigned (0);
		gcov_write_string (NULL);
		gcov_write_length (offset);
	      }
	    if (gcov_is_error ())
	      warning ("error writing `%s'", bbg_file_name);
	  }
      }
    }

  if (flag_branch_probabilities)
    compute_branch_probabilities ();

  /* For each edge not on the spanning tree, add counting code as rtl.  */

  if (cfun->arc_profile && profile_arc_flag)
    {
      struct function_list *item;
      
      instrument_edges (el);

      /* Commit changes done by instrumentation.  */
      commit_edge_insertions_watch_calls ();
      allocate_reg_info (max_reg_num (), FALSE, FALSE);

      /* ??? Probably should re-use the existing struct function.  */
      item = xmalloc (sizeof (struct function_list));
      
      *functions_tail = item;
      functions_tail = &item->next;
      
      item->next = 0;
      item->name = xstrdup (name);
      item->cfg_checksum = profile_info.current_function_cfg_checksum;
      item->n_counter_sections = 0;
      for (i = 0; i < profile_info.n_sections; i++)
	if (profile_info.section_info[i].n_counters_now)
	  {
	    item->counter_sections[item->n_counter_sections].tag = 
		    profile_info.section_info[i].tag;
	    item->counter_sections[item->n_counter_sections].n_counters =
		    profile_info.section_info[i].n_counters_now;
	    item->n_counter_sections++;
	  }
    }

  remove_fake_edges ();
  free_aux_for_edges ();
  /* Re-merge split basic blocks and the mess introduced by
     insert_insn_on_edge.  */
  cleanup_cfg (profile_arc_flag ? CLEANUP_EXPENSIVE : 0);
  if (rtl_dump_file)
    dump_flow_info (rtl_dump_file);

  free_edge_list (el);
}

/* Union find algorithm implementation for the basic blocks using
   aux fields.  */

static basic_block
find_group (bb)
     basic_block bb;
{
  basic_block group = bb, bb1;

  while ((basic_block) group->aux != group)
    group = (basic_block) group->aux;

  /* Compress path.  */
  while ((basic_block) bb->aux != group)
    {
      bb1 = (basic_block) bb->aux;
      bb->aux = (void *) group;
      bb = bb1;
    }
  return group;
}

static void
union_groups (bb1, bb2)
     basic_block bb1, bb2;
{
  basic_block bb1g = find_group (bb1);
  basic_block bb2g = find_group (bb2);

  /* ??? I don't have a place for the rank field.  OK.  Lets go w/o it,
     this code is unlikely going to be performance problem anyway.  */
  if (bb1g == bb2g)
    abort ();

  bb1g->aux = bb2g;
}

/* This function searches all of the edges in the program flow graph, and puts
   as many bad edges as possible onto the spanning tree.  Bad edges include
   abnormals edges, which can't be instrumented at the moment.  Since it is
   possible for fake edges to form a cycle, we will have to develop some
   better way in the future.  Also put critical edges to the tree, since they
   are more expensive to instrument.  */

static void
find_spanning_tree (el)
     struct edge_list *el;
{
  int i;
  int num_edges = NUM_EDGES (el);
  basic_block bb;

  /* We use aux field for standard union-find algorithm.  */
  FOR_BB_BETWEEN (bb, ENTRY_BLOCK_PTR, NULL, next_bb)
    bb->aux = bb;

  /* Add fake edge exit to entry we can't instrument.  */
  union_groups (EXIT_BLOCK_PTR, ENTRY_BLOCK_PTR);

  /* First add all abnormal edges to the tree unless they form a cycle. Also
     add all edges to EXIT_BLOCK_PTR to avoid inserting profiling code behind
     setting return value from function.  */
  for (i = 0; i < num_edges; i++)
    {
      edge e = INDEX_EDGE (el, i);
      if (((e->flags & (EDGE_ABNORMAL | EDGE_ABNORMAL_CALL | EDGE_FAKE))
	   || e->dest == EXIT_BLOCK_PTR
	   )
	  && !EDGE_INFO (e)->ignore
	  && (find_group (e->src) != find_group (e->dest)))
	{
	  if (rtl_dump_file)
	    fprintf (rtl_dump_file, "Abnormal edge %d to %d put to tree\n",
		     e->src->index, e->dest->index);
	  EDGE_INFO (e)->on_tree = 1;
	  union_groups (e->src, e->dest);
	}
    }

  /* Now insert all critical edges to the tree unless they form a cycle.  */
  for (i = 0; i < num_edges; i++)
    {
      edge e = INDEX_EDGE (el, i);
      if ((EDGE_CRITICAL_P (e))
	  && !EDGE_INFO (e)->ignore
	  && (find_group (e->src) != find_group (e->dest)))
	{
	  if (rtl_dump_file)
	    fprintf (rtl_dump_file, "Critical edge %d to %d put to tree\n",
		     e->src->index, e->dest->index);
	  EDGE_INFO (e)->on_tree = 1;
	  union_groups (e->src, e->dest);
	}
    }

  /* And now the rest.  */
  for (i = 0; i < num_edges; i++)
    {
      edge e = INDEX_EDGE (el, i);
      if (find_group (e->src) != find_group (e->dest)
	  && !EDGE_INFO (e)->ignore)
	{
	  if (rtl_dump_file)
	    fprintf (rtl_dump_file, "Normal edge %d to %d put to tree\n",
		     e->src->index, e->dest->index);
	  EDGE_INFO (e)->on_tree = 1;
	  union_groups (e->src, e->dest);
	}
    }

  FOR_BB_BETWEEN (bb, ENTRY_BLOCK_PTR, NULL, next_bb)
    bb->aux = NULL;
}

/* Perform file-level initialization for branch-prob processing.  */

void
init_branch_prob (filename)
  const char *filename;
{
  int len = strlen (filename);
  int i;

  da_file_name = (char *) xmalloc (len + strlen (GCOV_DATA_SUFFIX) + 1);
  strcpy (da_file_name, filename);
  strcat (da_file_name, GCOV_DATA_SUFFIX);
  
  if (flag_branch_probabilities)
    read_counts_file (da_file_name);

  if (flag_test_coverage)
    {
      /* Open the bbg output file.  */
      bbg_file_name = (char *) xmalloc (len + strlen (GCOV_GRAPH_SUFFIX) + 1);
      strcpy (bbg_file_name, filename);
      strcat (bbg_file_name, GCOV_GRAPH_SUFFIX);
      if (!gcov_open (bbg_file_name, -1))
	error ("cannot open %s", bbg_file_name);
      gcov_write_unsigned (GCOV_GRAPH_MAGIC);
      gcov_write_unsigned (GCOV_VERSION);
    }

  if (profile_arc_flag)
    {
      /* Generate and save a copy of this so it can be shared.  */
      char buf[20];
      
      ASM_GENERATE_INTERNAL_LABEL (buf, "LPBX", 2);
      profiler_label = gen_rtx_SYMBOL_REF (Pmode, ggc_strdup (buf));
    }
  
  total_num_blocks = 0;
  total_num_edges = 0;
  total_num_edges_ignored = 0;
  total_num_edges_instrumented = 0;
  total_num_blocks_created = 0;
  total_num_passes = 0;
  total_num_times_called = 0;
  total_num_branches = 0;
  total_num_never_executed = 0;
  for (i = 0; i < 20; i++)
    total_hist_br_prob[i] = 0;
}

/* Performs file-level cleanup after branch-prob processing
   is completed.  */

void
end_branch_prob ()
{
  if (flag_test_coverage)
    {
      int error = gcov_close ();
      
      if (error)
	unlink (bbg_file_name);
#if SELF_COVERAGE
      /* If the compiler is instrumented, we should not
         unconditionally remove the counts file, because we might be
         recompiling ourselves. The .da files are all removed during
         copying the stage1 files.  */
      if (error)
#endif
	unlink (da_file_name);
    }

  if (rtl_dump_file)
    {
      fprintf (rtl_dump_file, "\n");
      fprintf (rtl_dump_file, "Total number of blocks: %d\n",
	       total_num_blocks);
      fprintf (rtl_dump_file, "Total number of edges: %d\n", total_num_edges);
      fprintf (rtl_dump_file, "Total number of ignored edges: %d\n",
	       total_num_edges_ignored);
      fprintf (rtl_dump_file, "Total number of instrumented edges: %d\n",
	       total_num_edges_instrumented);
      fprintf (rtl_dump_file, "Total number of blocks created: %d\n",
	       total_num_blocks_created);
      fprintf (rtl_dump_file, "Total number of graph solution passes: %d\n",
	       total_num_passes);
      if (total_num_times_called != 0)
	fprintf (rtl_dump_file, "Average number of graph solution passes: %d\n",
		 (total_num_passes + (total_num_times_called  >> 1))
		 / total_num_times_called);
      fprintf (rtl_dump_file, "Total number of branches: %d\n",
	       total_num_branches);
      fprintf (rtl_dump_file, "Total number of branches never executed: %d\n",
	       total_num_never_executed);
      if (total_num_branches)
	{
	  int i;

	  for (i = 0; i < 10; i++)
	    fprintf (rtl_dump_file, "%d%% branches in range %d-%d%%\n",
		     (total_hist_br_prob[i] + total_hist_br_prob[19-i]) * 100
		     / total_num_branches, 5*i, 5*i+5);
	}
    }
}

/* Find (and create if not present) a section with TAG.  */
struct section_info *
find_counters_section (tag)
     unsigned tag;
{
  unsigned i;

  for (i = 0; i < profile_info.n_sections; i++)
    if (profile_info.section_info[i].tag == tag)
      return profile_info.section_info + i;

  if (i == MAX_COUNTER_SECTIONS)
    abort ();

  profile_info.section_info[i].tag = tag;
  profile_info.section_info[i].present = 0;
  profile_info.section_info[i].n_counters = 0;
  profile_info.section_info[i].n_counters_now = 0;
  profile_info.n_sections++;

  return profile_info.section_info + i;
}

/* Set FIELDS as purpose to VALUE.  */
static void
set_purpose (value, fields)
     tree value;
     tree fields;
{
  tree act_field, act_value;
  
  for (act_field = fields, act_value = value;
       act_field;
       act_field = TREE_CHAIN (act_field), act_value = TREE_CHAIN (act_value))
    TREE_PURPOSE (act_value) = act_field;
}

/* Returns label for base of counters inside TAG section.  */
static rtx
label_for_tag (tag)
     unsigned tag;
{
  switch (tag)
    {
    case GCOV_TAG_ARC_COUNTS:
      return profiler_label;
    default:
      abort ();
    }
}

/* Creates fields of struct counter_section (in gcov-io.h).  */
static tree
build_counter_section_fields ()
{
  tree field, fields;

  /* tag */
  fields = build_decl (FIELD_DECL, NULL_TREE, unsigned_type_node);

  /* n_counters */
  field = build_decl (FIELD_DECL, NULL_TREE, unsigned_type_node);
  TREE_CHAIN (field) = fields;
  fields = field;

  return fields;
}

/* Creates value of struct counter_section (in gcov-io.h).  */
static tree
build_counter_section_value (tag, n_counters)
     unsigned tag;
     unsigned n_counters;
{
  tree value = NULL_TREE;

  /* tag */
  value = tree_cons (NULL_TREE,
		     convert (unsigned_type_node,
			      build_int_2 (tag, 0)),
		     value);
  
  /* n_counters */
  value = tree_cons (NULL_TREE,
		     convert (unsigned_type_node,
			      build_int_2 (n_counters, 0)),
		     value);

  return value;
}

/* Creates fields of struct counter_section_data (in gcov-io.h).  */
static tree
build_counter_section_data_fields ()
{
  tree field, fields, gcov_type, gcov_ptr_type;

  gcov_type = make_signed_type (GCOV_TYPE_SIZE);
  gcov_ptr_type =
	  build_pointer_type (build_qualified_type (gcov_type,
						    TYPE_QUAL_CONST));

  /* tag */
  fields = build_decl (FIELD_DECL, NULL_TREE, unsigned_type_node);

  /* n_counters */
  field = build_decl (FIELD_DECL, NULL_TREE, unsigned_type_node);
  TREE_CHAIN (field) = fields;
  fields = field;

  /* counters */
  field = build_decl (FIELD_DECL, NULL_TREE, gcov_ptr_type);
  TREE_CHAIN (field) = fields;
  fields = field;

  return fields;
}

/* Creates value of struct counter_section_data (in gcov-io.h).  */
static tree
build_counter_section_data_value (tag, n_counters)
     unsigned tag;
     unsigned n_counters;
{
  tree value = NULL_TREE, counts_table, gcov_type, gcov_ptr_type;

  gcov_type = make_signed_type (GCOV_TYPE_SIZE);
  gcov_ptr_type
    = build_pointer_type (build_qualified_type
			  (gcov_type, TYPE_QUAL_CONST));

  /* tag */
  value = tree_cons (NULL_TREE,
		     convert (unsigned_type_node,
			      build_int_2 (tag, 0)),
		     value);
  
  /* n_counters */
  value = tree_cons (NULL_TREE,
		     convert (unsigned_type_node,
			      build_int_2 (n_counters, 0)),
		     value);

  /* counters */
  if (n_counters)
    {
      tree gcov_type_array_type =
	      build_array_type (gcov_type,
				build_index_type (build_int_2 (n_counters - 1,
							       0)));
      counts_table =
	      build (VAR_DECL, gcov_type_array_type, NULL_TREE, NULL_TREE);
      TREE_STATIC (counts_table) = 1;
      DECL_NAME (counts_table) = get_identifier (XSTR (label_for_tag (tag), 0));
      assemble_variable (counts_table, 0, 0, 0);
      counts_table = build1 (ADDR_EXPR, gcov_ptr_type, counts_table);
    }
  else
    counts_table = null_pointer_node;

  value = tree_cons (NULL_TREE, counts_table, value);

  return value;
}

/* Creates fields for struct function_info type (in gcov-io.h).  */
static tree
build_function_info_fields ()
{
  tree field, fields, counter_section_fields, counter_section_type;
  tree counter_sections_ptr_type;
  tree string_type =
	  build_pointer_type (build_qualified_type (char_type_node,
						    TYPE_QUAL_CONST));
  /* name */
  fields = build_decl (FIELD_DECL, NULL_TREE, string_type);

  /* checksum */
  field = build_decl (FIELD_DECL, NULL_TREE, unsigned_type_node);
  TREE_CHAIN (field) = fields;
  fields = field;

  /* n_counter_sections */
  field = build_decl (FIELD_DECL, NULL_TREE, unsigned_type_node);
  TREE_CHAIN (field) = fields;
  fields = field;

  /* counter_sections */
  counter_section_fields = build_counter_section_fields ();
  counter_section_type = (*lang_hooks.types.make_type) (RECORD_TYPE);
  finish_builtin_struct (counter_section_type, "__counter_section",
			 counter_section_fields, NULL_TREE);
  counter_sections_ptr_type =
	  build_pointer_type
	  	(build_qualified_type (counter_section_type,
				       TYPE_QUAL_CONST));
  field = build_decl (FIELD_DECL, NULL_TREE, counter_sections_ptr_type);
  TREE_CHAIN (field) = fields;
  fields = field;

  return fields;
}

/* Creates value for struct function_info (in gcov-io.h).  */
static tree
build_function_info_value (function)
     struct function_list *function;
{
  tree value = NULL_TREE;
  size_t name_len = strlen (function->name);
  tree fname = build_string (name_len + 1, function->name);
  tree string_type =
	  build_pointer_type (build_qualified_type (char_type_node,
						    TYPE_QUAL_CONST));
  tree counter_section_fields, counter_section_type, counter_sections_value;
  tree counter_sections_ptr_type, counter_sections_array_type;
  unsigned i;

  /* name */
  TREE_TYPE (fname) =
	  build_array_type (char_type_node,
			    build_index_type (build_int_2 (name_len, 0)));
  value = tree_cons (NULL_TREE,
		     build1 (ADDR_EXPR,
			     string_type,
			     fname),
		     value);

  /* checksum */
  value = tree_cons (NULL_TREE,
		     convert (unsigned_type_node,
			      build_int_2 (function->cfg_checksum, 0)),
		     value);

  /* n_counter_sections */

  value = tree_cons (NULL_TREE,
		     convert (unsigned_type_node,
			      build_int_2 (function->n_counter_sections, 0)),
	    	    value);

  /* counter_sections */
  counter_section_fields = build_counter_section_fields ();
  counter_section_type = (*lang_hooks.types.make_type) (RECORD_TYPE);
  counter_sections_ptr_type =
	  build_pointer_type
	  	(build_qualified_type (counter_section_type,
				       TYPE_QUAL_CONST));
  counter_sections_array_type =
	  build_array_type (counter_section_type,
			    build_index_type (
      				build_int_2 (function->n_counter_sections - 1,
		  			     0)));

  counter_sections_value = NULL_TREE;
  for (i = 0; i < function->n_counter_sections; i++)
    {
      tree counter_section_value =
	      build_counter_section_value (function->counter_sections[i].tag,
					   function->counter_sections[i].n_counters);
      set_purpose (counter_section_value, counter_section_fields);
      counter_sections_value = tree_cons (NULL_TREE,
					  build (CONSTRUCTOR,
						 counter_section_type,
						 NULL_TREE,
						 nreverse (counter_section_value)),
					  counter_sections_value);
    }
  finish_builtin_struct (counter_section_type, "__counter_section",
			 counter_section_fields, NULL_TREE);

  if (function->n_counter_sections)
    {
      counter_sections_value = 
	      build (CONSTRUCTOR,
 		     counter_sections_array_type,
		     NULL_TREE,
		     nreverse (counter_sections_value)),
      counter_sections_value = build1 (ADDR_EXPR,
				       counter_sections_ptr_type,
				       counter_sections_value);
    }
  else
    counter_sections_value = null_pointer_node;

  value = tree_cons (NULL_TREE, counter_sections_value, value);

  return value;
}

/* Creates fields of struct gcov_info type (in gcov-io.h).  */
static tree
build_gcov_info_fields (gcov_info_type)
     tree gcov_info_type;
{
  tree field, fields;
  char *filename;
  int filename_len;
  tree string_type =
	  build_pointer_type (build_qualified_type (char_type_node,
						    TYPE_QUAL_CONST));
  tree function_info_fields, function_info_type, function_info_ptr_type;
  tree counter_section_data_fields, counter_section_data_type;
  tree counter_section_data_ptr_type;

  /* Version ident */
  fields = build_decl (FIELD_DECL, NULL_TREE, long_unsigned_type_node);

  /* next -- NULL */
  field = build_decl (FIELD_DECL, NULL_TREE,
		      build_pointer_type (build_qualified_type (gcov_info_type,
								TYPE_QUAL_CONST)));
  TREE_CHAIN (field) = fields;
  fields = field;
  
  /* Filename */
  filename = getpwd ();
  filename = (filename && da_file_name[0] != '/'
	      ? concat (filename, "/", da_file_name, NULL)
	      : da_file_name);
  filename_len = strlen (filename);
  if (filename != da_file_name)
    free (filename);

  field = build_decl (FIELD_DECL, NULL_TREE, string_type);
  TREE_CHAIN (field) = fields;
  fields = field;
  
  /* Workspace */
  field = build_decl (FIELD_DECL, NULL_TREE, long_integer_type_node);
  TREE_CHAIN (field) = fields;
  fields = field;

  /* number of functions */
  field = build_decl (FIELD_DECL, NULL_TREE, unsigned_type_node);
  TREE_CHAIN (field) = fields;
  fields = field;
      
  /* function_info table */
  function_info_fields = build_function_info_fields ();
  function_info_type = (*lang_hooks.types.make_type) (RECORD_TYPE);
  finish_builtin_struct (function_info_type, "__function_info",
			 function_info_fields, NULL_TREE);
  function_info_ptr_type =
	  build_pointer_type
	  	(build_qualified_type (function_info_type,
				       TYPE_QUAL_CONST));
  field = build_decl (FIELD_DECL, NULL_TREE, function_info_ptr_type);
  TREE_CHAIN (field) = fields;
  fields = field;
    
  /* n_counter_sections  */
  field = build_decl (FIELD_DECL, NULL_TREE, unsigned_type_node);
  TREE_CHAIN (field) = fields;
  fields = field;
  
  /* counter sections */
  counter_section_data_fields = build_counter_section_data_fields ();
  counter_section_data_type = (*lang_hooks.types.make_type) (RECORD_TYPE);
  finish_builtin_struct (counter_section_data_type, "__counter_section_data",
			 counter_section_data_fields, NULL_TREE);
  counter_section_data_ptr_type =
	  build_pointer_type
	  	(build_qualified_type (counter_section_data_type,
				       TYPE_QUAL_CONST));
  field = build_decl (FIELD_DECL, NULL_TREE, counter_section_data_ptr_type);
  TREE_CHAIN (field) = fields;
  fields = field;

  return fields;
}

/* Creates struct gcov_info value (in gcov-io.h).  */
static tree
build_gcov_info_value ()
{
  tree value = NULL_TREE;
  tree filename_string;
  char *filename;
  int filename_len;
  unsigned n_functions, i;
  struct function_list *item;
  tree string_type =
	  build_pointer_type (build_qualified_type (char_type_node,
						    TYPE_QUAL_CONST));
  tree function_info_fields, function_info_type, function_info_ptr_type;
  tree functions;
  tree counter_section_data_fields, counter_section_data_type;
  tree counter_section_data_ptr_type, counter_sections;

  /* Version ident */
  value = tree_cons (NULL_TREE,
		     convert (long_unsigned_type_node,
			      build_int_2 (GCOV_VERSION, 0)),
		     value);

  /* next -- NULL */
  value = tree_cons (NULL_TREE, null_pointer_node, value);
  
  /* Filename */
  filename = getpwd ();
  filename = (filename && da_file_name[0] != '/'
	      ? concat (filename, "/", da_file_name, NULL)
	      : da_file_name);
  filename_len = strlen (filename);
  filename_string = build_string (filename_len + 1, filename);
  if (filename != da_file_name)
    free (filename);
  TREE_TYPE (filename_string) =
	  build_array_type (char_type_node,
			    build_index_type (build_int_2 (filename_len, 0)));
  value = tree_cons (NULL_TREE,
		     build1 (ADDR_EXPR,
			     string_type,
		       	     filename_string),
		     value);
  
  /* Workspace */
  value = tree_cons (NULL_TREE,
		     convert (long_integer_type_node, integer_zero_node),
		     value);
      
  /* number of functions */
  n_functions = 0;
  for (item = functions_head; item != 0; item = item->next, n_functions++)
    continue;
  value = tree_cons (NULL_TREE,
		     convert (unsigned_type_node,
			      build_int_2 (n_functions, 0)),
		     value);

  /* function_info table */
  function_info_fields = build_function_info_fields ();
  function_info_type = (*lang_hooks.types.make_type) (RECORD_TYPE);
  function_info_ptr_type =
	  build_pointer_type (
		build_qualified_type (function_info_type,
	       			      TYPE_QUAL_CONST));
  functions = NULL_TREE;
  for (item = functions_head; item != 0; item = item->next)
    {
      tree function_info_value = build_function_info_value (item);
      set_purpose (function_info_value, function_info_fields);
      functions = tree_cons (NULL_TREE,
    			     build (CONSTRUCTOR,
			    	    function_info_type,
				    NULL_TREE,
				    nreverse (function_info_value)),
			     functions);
    }
  finish_builtin_struct (function_info_type, "__function_info",
			 function_info_fields, NULL_TREE);

  /* Create constructor for array.  */
  if (n_functions)
    {
      tree array_type;

      array_type = build_array_type (
			function_info_type,
   			build_index_type (build_int_2 (n_functions - 1, 0)));
      functions = build (CONSTRUCTOR,
      			 array_type,
			 NULL_TREE,
			 nreverse (functions));
      functions = build1 (ADDR_EXPR,
			  function_info_ptr_type,
			  functions);
    }
  else
    functions = null_pointer_node;

  value = tree_cons (NULL_TREE, functions, value);

  /* n_counter_sections  */
  value = tree_cons (NULL_TREE,
		     convert (unsigned_type_node,
			      build_int_2 (profile_info.n_sections, 0)),
		     value);
  
  /* counter sections */
  counter_section_data_fields = build_counter_section_data_fields ();
  counter_section_data_type = (*lang_hooks.types.make_type) (RECORD_TYPE);
  counter_sections = NULL_TREE;
  for (i = 0; i < profile_info.n_sections; i++)
    {
      tree counter_sections_value =
	      build_counter_section_data_value (
		profile_info.section_info[i].tag,
		profile_info.section_info[i].n_counters);
      set_purpose (counter_sections_value, counter_section_data_fields);
      counter_sections = tree_cons (NULL_TREE,
		       		    build (CONSTRUCTOR,
		       			   counter_section_data_type,
		       			   NULL_TREE,
		       			   nreverse (counter_sections_value)),
		       		    counter_sections);
    }
  finish_builtin_struct (counter_section_data_type, "__counter_section_data",
			 counter_section_data_fields, NULL_TREE);
  counter_section_data_ptr_type =
	  build_pointer_type
	  	(build_qualified_type (counter_section_data_type,
				       TYPE_QUAL_CONST));

  if (profile_info.n_sections)
    {
      counter_sections =
    	      build (CONSTRUCTOR,
    		     build_array_type (
	       			       counter_section_data_type,
		       		       build_index_type (build_int_2 (profile_info.n_sections - 1, 0))),
		     NULL_TREE,
		     nreverse (counter_sections));
      counter_sections = build1 (ADDR_EXPR,
				 counter_section_data_ptr_type,
				 counter_sections);
    }
  else
    counter_sections = null_pointer_node;
  value = tree_cons (NULL_TREE, counter_sections, value);

  return value;
}

/* Write out the structure which libgcc uses to locate all the arc
   counters.  The structures used here must match those defined in
   gcov-io.h.  Write out the constructor to call __gcov_init.  */

void
create_profiler ()
{
  tree gcov_info_fields, gcov_info_type, gcov_info_value, gcov_info;
  char name[20];
  char *ctor_name;
  tree ctor;
  rtx gcov_info_address;
  int save_flag_inline_functions = flag_inline_functions;
  unsigned i;

  for (i = 0; i < profile_info.n_sections; i++)
    if (profile_info.section_info[i].n_counters_now)
      break;
  if (i == profile_info.n_sections)
    return;
  
  gcov_info_type = (*lang_hooks.types.make_type) (RECORD_TYPE);
  gcov_info_fields = build_gcov_info_fields (gcov_info_type);
  gcov_info_value = build_gcov_info_value ();
  set_purpose (gcov_info_value, gcov_info_fields);
  finish_builtin_struct (gcov_info_type, "__gcov_info",
			 gcov_info_fields, NULL_TREE);

  gcov_info = build (VAR_DECL, gcov_info_type, NULL_TREE, NULL_TREE);
  DECL_INITIAL (gcov_info) =
	  build (CONSTRUCTOR, gcov_info_type, NULL_TREE,
		 nreverse (gcov_info_value));

  TREE_STATIC (gcov_info) = 1;
  ASM_GENERATE_INTERNAL_LABEL (name, "LPBX", 0);
  DECL_NAME (gcov_info) = get_identifier (name);
  
  /* Build structure.  */
  assemble_variable (gcov_info, 0, 0, 0);

  /* Build the constructor function to invoke __gcov_init.  */
  ctor_name = concat (IDENTIFIER_POINTER (get_file_function_name ('I')),
		      "_GCOV", NULL);
  ctor = build_decl (FUNCTION_DECL, get_identifier (ctor_name),
		     build_function_type (void_type_node, NULL_TREE));
  free (ctor_name);
  DECL_EXTERNAL (ctor) = 0;

  /* It can be a static function as long as collect2 does not have
     to scan the object file to find its ctor/dtor routine.  */
  TREE_PUBLIC (ctor) = ! targetm.have_ctors_dtors;
  TREE_USED (ctor) = 1;
  DECL_RESULT (ctor) = build_decl (RESULT_DECL, NULL_TREE, void_type_node);

  ctor = (*lang_hooks.decls.pushdecl) (ctor);
  rest_of_decl_compilation (ctor, 0, 1, 0);
  announce_function (ctor);
  current_function_decl = ctor;
  DECL_INITIAL (ctor) = error_mark_node;
  make_decl_rtl (ctor, NULL);
  init_function_start (ctor, input_filename, lineno);
  (*lang_hooks.decls.pushlevel) (0);
  expand_function_start (ctor, 0);
  cfun->arc_profile = 0;

  /* Actually generate the code to call __gcov_init.  */
  gcov_info_address = force_reg (Pmode,
				 gen_rtx_SYMBOL_REF (
					Pmode,
					IDENTIFIER_POINTER (
						DECL_NAME (gcov_info))));
  emit_library_call (gen_rtx_SYMBOL_REF (Pmode, "__gcov_init"),
		     LCT_NORMAL, VOIDmode, 1,
		     gcov_info_address, Pmode);

  expand_function_end (input_filename, lineno, 0);
  (*lang_hooks.decls.poplevel) (1, 0, 1);

  /* Since ctor isn't in the list of globals, it would never be emitted
     when it's considered to be 'safe' for inlining, so turn off
     flag_inline_functions.  */
  flag_inline_functions = 0;

  rest_of_compilation (ctor);

  /* Reset flag_inline_functions to its original value.  */
  flag_inline_functions = save_flag_inline_functions;

  if (! quiet_flag)
    fflush (asm_out_file);
  current_function_decl = NULL_TREE;

  if (targetm.have_ctors_dtors)
    (* targetm.asm_out.constructor) (XEXP (DECL_RTL (ctor), 0),
				     DEFAULT_INIT_PRIORITY);
}

/* Output instructions as RTL to increment the edge execution count.  */

static rtx
gen_edge_profiler (edgeno)
     int edgeno;
{
  enum machine_mode mode = mode_for_size (GCOV_TYPE_SIZE, MODE_INT, 0);
  rtx mem_ref, tmp;
  rtx sequence;

  start_sequence ();

  tmp = force_reg (Pmode, profiler_label);
  tmp = plus_constant (tmp, GCOV_TYPE_SIZE / BITS_PER_UNIT * edgeno);
  mem_ref = validize_mem (gen_rtx_MEM (mode, tmp));

  set_mem_alias_set (mem_ref, new_alias_set ());

  tmp = expand_simple_binop (mode, PLUS, mem_ref, const1_rtx,
			     mem_ref, 0, OPTAB_WIDEN);

  if (tmp != mem_ref)
    emit_move_insn (copy_rtx (mem_ref), tmp);

  sequence = get_insns ();
  end_sequence ();
  return sequence;
}

#include "gt-profile.h"
