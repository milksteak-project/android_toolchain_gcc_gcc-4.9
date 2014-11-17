/* Calculate branch probabilities, and basic block execution counts.
   Copyright (C) 2012. Free Software Foundation, Inc.
   Contributed by Dehao Chen (dehao@google.com)

This file is part of GCC.

GCC is free software; you can redistribute it and/or modify it under
the terms of the GNU General Public License as published by the Free
Software Foundation; either version 3, or (at your option) any later
version.

GCC is distributed in the hope that it will be useful, but WITHOUT ANY
WARRANTY; without even the implied warranty of MERCHANTABILITY or
FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
for more details.

You should have received a copy of the GNU General Public License
along with GCC; see the file COPYING3.  If not see
<http://www.gnu.org/licenses/>.  */

/* Read and annotate call graph profile from the auto profile data
   file.  */

#include <string.h>
#include <map>
#include <vector>
#include <set>

#include "config.h"
#include "system.h"
#include "coretypes.h"
#include "tree.h"
#include "flags.h"
#include "basic-block.h"
#include "diagnostic-core.h"
#include "gcov-io.h"
#include "input.h"
#include "profile.h"
#include "langhooks.h"
#include "opts.h"
#include "tree-pass.h"
#include "cfgloop.h"
#include "tree-ssa-alias.h"
#include "tree-cfg.h"
#include "tree-cfgcleanup.h"
#include "tree-ssa-operands.h"
#include "tree-into-ssa.h"
#include "internal-fn.h"
#include "is-a.h"
#include "gimple-expr.h"
#include "md5.h"
#include "gimple.h"
#include "gimple-iterator.h"
#include "gimple-ssa.h"
#include "cgraph.h"
#include "value-prof.h"
#include "coverage.h"
#include "params.h"
#include "l-ipo.h"
#include "ipa-utils.h"
#include "ipa-inline.h"
#include "output.h"
#include "dwarf2asm.h"
#include "tree-inline.h"
#include "auto-profile.h"

/* The following routines implements AutoFDO optimization.

   This optimization uses sampling profiles to annotate basic block counts
   and uses heuristics to estimate branch probabilities.

   There are three phases in AutoFDO:

   Phase 1: Read profile from the profile data file.
     The following info is read from the profile datafile:
	* string_table: a map between function name and its index.
	* autofdo_source_profile: a map from function_instance name to
	  function_instance. This is represented as a forest of
	  function_instances.
	* autofdo_module_profile: a map from module name to its
	  compilation/aux-module info.
	* WorkingSet: a histogram of how many instructions are covered for a
	given percentage of total cycles.

   Phase 2: Early inline.
     Early inline uses autofdo_source_profile to find if a callsite is:
	* inlined in the profiled binary.
	* callee body is hot in the profiling run.
     If both condition satisfies, early inline will inline the callsite
     regardless of the code growth.

   Phase 3: Annotate control flow graph.
     AutoFDO uses a separate pass to:
	* Annotate basic block count
	* Estimate branch probability

   After the above 3 phases, all profile is readily annotated on the GCC IR.
   AutoFDO tries to reuse all FDO infrastructure as much as possible to make
   use of the profile. E.g. it uses existing mechanism to calculate the basic
   block/edge frequency, as well as the cgraph node/edge count.
*/

#define DEFAULT_AUTO_PROFILE_FILE "fbdata.afdo"

namespace autofdo {

/* Represent a source location: (function_decl, lineno).  */
typedef std::pair<tree, unsigned> decl_lineno;

/* Represent an inline stack. vector[0] is the leaf node.  */
typedef std::vector<decl_lineno> inline_stack;

/* String array that stores function names.  */
typedef std::vector<const char *> string_vector;

/* Map from function name's index in string_table to target's
   execution count.  */
typedef std::map<unsigned, gcov_type> icall_target_map;

/* Set of gimple stmts. Used to track if the stmt has already been promoted
   to direct call.  */
typedef std::set<gimple> stmt_set;

/* Represent count info of an inline stack.  */
struct count_info
{
  /* Sampled count of the inline stack.  */
  gcov_type count;

  /* Map from indirect call target to its sample count.  */
  icall_target_map targets;

  /* Whether this inline stack is already used in annotation. 

     Each inline stack should only be used to annotate IR once.
     This will be enforced when instruction-level discriminator
     is supported.  */
  bool annotated;
};

/* operator< for "const char *".  */
struct string_compare
{
  bool operator() (const char *a, const char *b) const
    { return strcmp (a, b) < 0; }
};

/* Store a string array, indexed by string position in the array.  */
class string_table {
public:
  static string_table *create ();

  /* For a given string, returns its index.  */
  int get_index (const char *name) const;

  /* For a given decl, returns the index of the decl name.  */
  int get_index_by_decl (tree decl) const;

  /* For a given index, returns the string.  */
  const char *get_name (int index) const;

private:
  string_table () {}
  bool read ();

  typedef std::map<const char *, unsigned, string_compare> string_index_map;
  string_vector vector_;
  string_index_map map_;
};

/* Profile of a function instance:
     1. total_count of the function.
     2. head_count of the function (only valid when function is a top-level
	function_instance, i.e. it is the original copy instead of the
	inlined copy).
     3. map from source location (decl_lineno) of the inlined callsite to
	profile (count_info).
     4. map from callsite to callee function_instance.  */
class function_instance {
public:
  typedef std::vector<function_instance *> function_instance_stack;

  /* Read the profile and return a function_instance with head count as
     HEAD_COUNT. Recursively read callsites to create nested function_instances
     too. STACK is used to track the recursive creation process.  */
  static function_instance *read_function_instance (
      function_instance_stack *stack, gcov_type head_count);

  /* Recursively deallocate all callsites (nested function_instances).  */
  ~function_instance ();

  /* Accessors.  */
  int name () const { return name_; }
  gcov_type total_count () const { return total_count_; }
  gcov_type head_count () const { return head_count_; }

  /* Recursively traverse STACK starting from LEVEL to find the corresponding
     function_instance.  */
  function_instance *get_function_instance (const inline_stack &stack,
					    unsigned level);

  /* Store the profile info for LOC in INFO. Return TRUE if profile info
     is found.  */
  bool get_count_info (location_t loc, count_info *info) const;

  /* Read the inlined indirect call target profile for STMT and store it in
     MAP, return the total count for all inlined indirect calls.  */
  gcov_type find_icall_target_map (gimple stmt, icall_target_map *map) const;

  /* Sum of counts that is used during annotation.  */
  gcov_type total_annotated_count () const;

  /* Mark LOC as annotated.  */
  void mark_annotated (location_t loc);

private:
  function_instance (unsigned name, gcov_type head_count)
      : name_(name), total_count_(0), head_count_(head_count) {}

  /* Map from callsite decl_lineno (lineno in higher 16 bits, discriminator
     in lower 16 bits) to callee function_instance.  */
  typedef std::map<unsigned, function_instance *> callsite_map;
  /* Map from source location (decl_lineno) to profile (count_info).  */
  typedef std::map<unsigned, count_info> position_count_map;

  /* function_instance name index in the string_table.  */
  unsigned name_;

  /* Total sample count.  */
  gcov_type total_count_;

  /* Entry BB's sample count.  */
  gcov_type head_count_;

  /* Map from callsite location to callee function_instance.  */
  callsite_map callsites;

  /* Map from source location to count_info.  */
  position_count_map pos_counts;
};

/* Profile for all functions.  */
class autofdo_source_profile {
public:
  static autofdo_source_profile *create ()
    {
      autofdo_source_profile *map = new autofdo_source_profile ();
      if (map->read ())
	return map;
      delete map;
      return NULL;
    }

  ~autofdo_source_profile ();

  /* For a given DECL, returns the top-level function_instance.  */
  function_instance *get_function_instance_by_decl (tree decl) const;

  /* Find count_info for a given gimple STMT. If found, store the count_info
     in INFO and return true; otherwise return false.  */
  bool get_count_info (gimple stmt, count_info *info) const;

  /* Find total count of the callee of EDGE.  */
  gcov_type get_callsite_total_count (struct cgraph_edge *edge) const;

  /* Update value profile INFO for STMT from the inlined indirect callsite.
     Return true if INFO is updated.  */
  bool update_inlined_ind_target (gimple stmt, count_info *info);

  /* Mark LOC as annotated.  */
  void mark_annotated (location_t loc);

  /* Writes the profile annotation status for each function in an elf
     section.  */
  void write_annotated_count () const;

private:
  /* Map from function_instance name index (in string_table) to
     function_instance.  */
  typedef std::map<unsigned, function_instance *>
      name_function_instance_map;

  autofdo_source_profile () {}

  /* Read AutoFDO profile and returns TRUE on success.  */
  bool read ();

  /* Return the function_instance in the profile that correspond to the
     inline STACK.  */
  function_instance *get_function_instance_by_inline_stack (
      const inline_stack &stack) const;

  name_function_instance_map map_;
};

/* Module profile.  */
class autofdo_module_profile {
public:
  static autofdo_module_profile *create ()
    {
      autofdo_module_profile *map = new autofdo_module_profile ();
      if (map->read ())
	return map;
      delete map;
      return NULL;
    }

  /* For a given module NAME, returns this module's gcov_module_info.  */
  gcov_module_info *get_module(const char *name) const
    {
      name_target_map::const_iterator iter = map_.find (name);
      return iter == map_.end() ? NULL : iter->second.second;
    }

  /* For a given module NAME, returns this module's aux-modules.  */
  const string_vector *get_aux_modules(const char *name) const
    {
      name_target_map::const_iterator iter = map_.find (name);
      return iter == map_.end() ? NULL : &iter->second.first;
    }

private:
  autofdo_module_profile () {}
  bool read ();

  typedef std::pair<string_vector, gcov_module_info *> AuxInfo;
  typedef std::map<const char *, AuxInfo, string_compare> name_target_map;
  /* Map from module name to (aux_modules, gcov_module_info).  */
  name_target_map map_;
};


/* Store the strings read from the profile data file.  */
static string_table *afdo_string_table;
/* Store the AutoFDO source profile.  */
static autofdo_source_profile *afdo_source_profile;

/* Store the AutoFDO module profile.  */
static autofdo_module_profile *afdo_module_profile;

/* gcov_ctr_summary structure to store the profile_info.  */
static struct gcov_ctr_summary *afdo_profile_info;

/* Helper functions.  */

/* Return the original name of NAME: strip the suffix that starts
   with '.'  */

static const char *get_original_name (const char *name)
{
  char *ret = xstrdup (name);
  char *find = strchr (ret, '.');
  if (find != NULL)
    *find = 0;
  return ret;
}

/* Return the combined location, which is a 32bit integer in which
   higher 16 bits stores the line offset of LOC to the start lineno
   of DECL, The lower 16 bits stores the discrimnator.  */

static unsigned
get_combined_location (location_t loc, tree decl)
{
  return ((LOCATION_LINE (loc) - DECL_SOURCE_LINE (decl)) << 16)
	 | get_discriminator_from_locus (loc);
}

/* Return the function decl of a given lexical BLOCK.  */

static tree
get_function_decl_from_block (tree block)
{
  tree decl;

  if (LOCATION_LOCUS (BLOCK_SOURCE_LOCATION (block) == UNKNOWN_LOCATION))
    return NULL_TREE;

  for (decl = BLOCK_ABSTRACT_ORIGIN (block);
       decl && (TREE_CODE (decl) == BLOCK);
       decl = BLOCK_ABSTRACT_ORIGIN (decl))
    if (TREE_CODE (decl) == FUNCTION_DECL)
      break;
  return decl;
}

/* Store inline stack for STMT in STACK.  */

static void
get_inline_stack (location_t locus, inline_stack *stack)
{
  if (LOCATION_LOCUS (locus) == UNKNOWN_LOCATION)
    return;

  tree block = LOCATION_BLOCK (locus);
  if (block && TREE_CODE (block) == BLOCK)
    {
      int level = 0;
      for (block = BLOCK_SUPERCONTEXT (block);
	   block && (TREE_CODE (block) == BLOCK);
	   block = BLOCK_SUPERCONTEXT (block))
	{
	  location_t tmp_locus = BLOCK_SOURCE_LOCATION (block);
	  if (LOCATION_LOCUS (tmp_locus) == UNKNOWN_LOCATION)
	    continue;

	  tree decl = get_function_decl_from_block (block);
	  stack->push_back (std::make_pair (
	      decl, get_combined_location (locus, decl)));
	  locus = tmp_locus;
	  level++;
	}
    }
  stack->push_back (std::make_pair (
      current_function_decl,
      get_combined_location (locus, current_function_decl)));
}

/* Return STMT's combined location, which is a 32bit integer in which
   higher 16 bits stores the line offset of LOC to the start lineno
   of DECL, The lower 16 bits stores the discrimnator.  */

static unsigned
get_relative_location_for_stmt (gimple stmt)
{
  location_t locus = gimple_location (stmt);
  if (LOCATION_LOCUS (locus) == UNKNOWN_LOCATION)
    return UNKNOWN_LOCATION;

  for (tree block = gimple_block (stmt);
       block && (TREE_CODE (block) == BLOCK);
       block = BLOCK_SUPERCONTEXT (block))
    if (LOCATION_LOCUS (BLOCK_SOURCE_LOCATION (block)) != UNKNOWN_LOCATION)
      return get_combined_location (
	  locus, get_function_decl_from_block (block));
  return get_combined_location (locus, current_function_decl);
}

/* Return true if BB contains indirect call.  */

static bool
has_indirect_call (basic_block bb)
{
  gimple_stmt_iterator gsi;

  for (gsi = gsi_start_bb (bb); !gsi_end_p (gsi); gsi_next (&gsi))
    {
      gimple stmt = gsi_stmt (gsi);
      if (gimple_code (stmt) == GIMPLE_CALL
	  && (gimple_call_fn (stmt) == NULL
	      || TREE_CODE (gimple_call_fn (stmt)) != FUNCTION_DECL))
	return true;
    }
  return false;
}

/* Member functions for string_table.  */

string_table *
string_table::create ()
{
  string_table *map = new string_table();
  if (map->read ())
    return map;
  delete map;
  return NULL;
}

int
string_table::get_index (const char *name) const
{
  if (name == NULL)
    return -1;
  string_index_map::const_iterator iter = map_.find (name);
  if (iter == map_.end())
    return -1;
  else
    return iter->second;
}

int
string_table::get_index_by_decl (tree decl) const
{
  const char *name = get_original_name (
      IDENTIFIER_POINTER (DECL_ASSEMBLER_NAME (decl)));
  int ret = get_index (name);
  if (ret != -1)
    return ret;
  ret = get_index (lang_hooks.dwarf_name (decl, 0));
  if (ret != -1)
    return ret;
  if (DECL_ABSTRACT_ORIGIN (decl))
    return get_index_by_decl (DECL_ABSTRACT_ORIGIN (decl));
  else
    return -1;
}

const char *
string_table::get_name (int index) const
{
  gcc_assert (index > 0 && index < (int) vector_.size());
  return vector_[index];
}

bool
string_table::read ()
{
  if (gcov_read_unsigned () != GCOV_TAG_AFDO_FILE_NAMES)
    return false;
  /* Skip the length of the section.  */
  gcov_read_unsigned ();
  /* Read in the file name table.  */
  unsigned string_num = gcov_read_unsigned ();
  for (unsigned i = 0; i < string_num; i++)
    {
      vector_.push_back (get_original_name (gcov_read_string ()));
      map_[vector_.back()] = i;
    }
  return true;
}


/* Member functions for function_instance.  */

function_instance::~function_instance ()
{
  for (callsite_map::iterator iter = callsites.begin();
       iter != callsites.end(); ++iter)
    delete iter->second;
}

/* Recursively traverse STACK starting from LEVEL to find the corresponding
   function_instance.  */

function_instance *
function_instance::get_function_instance (
    const inline_stack &stack, unsigned level)
{
  if (level == 0)
    return this;
  callsite_map::const_iterator ret = callsites.find (stack[level].second);
  if (ret != callsites.end () && ret->second != NULL)
    return ret->second->get_function_instance (stack, level - 1);
  else
    return NULL;
}

/* Store the profile info for LOC in INFO. Return TRUE if profile info
   is found.  */

bool
function_instance::get_count_info (location_t loc, count_info *info) const
{
  position_count_map::const_iterator iter = pos_counts.find (loc);
  if (iter == pos_counts.end ())
    return false;
  *info = iter->second;
  return true;
}

/* Mark LOC as annotated.  */

void
function_instance::mark_annotated (location_t loc)
{
  position_count_map::iterator iter = pos_counts.find (loc);
  if (iter == pos_counts.end ())
    return;
  iter->second.annotated = true;
}

/* Read the inlinied indirect call target profile for STMT and store it in
   MAP, return the total count for all inlined indirect calls.  */

gcov_type
function_instance::find_icall_target_map (
    gimple stmt, icall_target_map *map) const
{
  gcov_type ret = 0;
  unsigned stmt_offset = get_relative_location_for_stmt (stmt);

  for (callsite_map::const_iterator iter = callsites.begin();
       iter != callsites.end(); ++iter)
    {
      unsigned callee = iter->second->name();
      /* Check if callsite location match the stmt.  */
      if (iter->first != stmt_offset)
	continue;
      struct cgraph_node *node = find_func_by_global_id (
	  (unsigned long long) afdo_string_table->get_name (callee), true);
      if (node == NULL)
	continue;
      if (!check_ic_target (stmt, node))
	continue;
      (*map)[callee] = iter->second->total_count ();
      ret += iter->second->total_count ();
    }
  return ret;
}

/* Read the profile and create a function_instance with head count as
   HEAD_COUNT. Recursively read callsites to create nested function_instances
   too. STACK is used to track the recursive creation process.  */

function_instance *
function_instance::read_function_instance (
    function_instance_stack *stack, gcov_type head_count)
{
  unsigned name = gcov_read_unsigned ();
  unsigned num_pos_counts = gcov_read_unsigned ();
  unsigned num_callsites = gcov_read_unsigned ();
  function_instance *s = new function_instance (name, head_count);
  stack->push_back(s);

  for (unsigned i = 0; i < num_pos_counts; i++)
    {
      unsigned offset = gcov_read_unsigned ();
      unsigned num_targets = gcov_read_unsigned ();
      gcov_type count = gcov_read_counter ();
      s->pos_counts[offset].count = count;
      for (unsigned j = 0; j < stack->size(); j++)
	(*stack)[j]->total_count_ += count;
      for (unsigned j = 0; j < num_targets; j++)
	{
	  /* Only indirect call target histogram is supported now.  */
	  gcov_read_unsigned ();
	  gcov_type target_idx = gcov_read_counter ();
	  s->pos_counts[offset].targets[target_idx] =
	      gcov_read_counter ();
	}
    }
  for (unsigned i = 0; i < num_callsites; i++) {
    unsigned offset = gcov_read_unsigned ();
    s->callsites[offset] = read_function_instance (stack, 0);
  }
  stack->pop_back();
  return s;
}

/* Sum of counts that is used during annotation.  */

gcov_type
function_instance::total_annotated_count () const
{
  gcov_type ret = 0;
  for (callsite_map::const_iterator iter = callsites.begin();
       iter != callsites.end(); ++iter)
    ret += iter->second->total_annotated_count ();
  for (position_count_map::const_iterator iter = pos_counts.begin();
       iter != pos_counts.end(); ++iter)
    if (iter->second.annotated)
      ret += iter->second.count;
  return ret;
}

void
autofdo_source_profile::write_annotated_count () const
{
  /* We store the annotation info as a string in the format of:

       function_name:total_count:annotated_count

     Because different modules may output the annotation info for a same
     function, we set the section as SECTION_MERGE so that we don't have
     replicated info in the final binary.  */
  switch_to_section (get_section (
      ".gnu.switches.text.annotation",
      SECTION_DEBUG | SECTION_MERGE | SECTION_STRINGS | (SECTION_ENTSIZE & 1),
      NULL));
  for (name_function_instance_map::const_iterator iter = map_.begin ();
       iter != map_.end (); ++iter)
    if (iter->second->total_count () > 0)
      {
	char buf[1024];
	snprintf (buf, 1024,
		  "%s:"HOST_WIDEST_INT_PRINT_DEC":"HOST_WIDEST_INT_PRINT_DEC,
		  afdo_string_table->get_name (iter->first),
		  iter->second->total_count (),
		  iter->second->total_annotated_count ());
	dw2_asm_output_nstring (buf, (size_t)-1, NULL);
      }
}


/* Member functions for autofdo_source_profile.  */

autofdo_source_profile::~autofdo_source_profile ()
{
  for (name_function_instance_map::const_iterator iter = map_.begin ();
       iter != map_.end (); ++iter)
    delete iter->second;
}

/* For a given DECL, returns the top-level function_instance.  */

function_instance *
autofdo_source_profile::get_function_instance_by_decl (tree decl) const
{
  int index = afdo_string_table->get_index_by_decl (decl);
  if (index == -1)
    return NULL;
  name_function_instance_map::const_iterator ret = map_.find (index);
  return ret == map_.end() ? NULL : ret->second;
}

/* Find count_info for a given gimple STMT. If found, store the count_info
   in INFO and return true; otherwise return false.  */

bool
autofdo_source_profile::get_count_info (gimple stmt, count_info *info) const
{
  if (LOCATION_LOCUS (gimple_location (stmt)) == cfun->function_end_locus)
    return false;

  inline_stack stack;
  get_inline_stack (gimple_location (stmt), &stack);
  if (stack.size () == 0)
    return false;
  const function_instance *s = get_function_instance_by_inline_stack (stack);
  if (s == NULL)
    return false;
  return s->get_count_info (stack[0].second, info);
}

void
autofdo_source_profile::mark_annotated (location_t loc) {
  inline_stack stack;
  get_inline_stack (loc, &stack);
  if (stack.size () == 0)
    return;
  function_instance *s = get_function_instance_by_inline_stack (stack);
  if (s == NULL)
    return;
  s->mark_annotated (stack[0].second);
}

/* Update value profile INFO for STMT from the inlined indirect callsite.
   Return true if INFO is updated.  */

bool
autofdo_source_profile::update_inlined_ind_target (
    gimple stmt, count_info *info)
{
  if (LOCATION_LOCUS (gimple_location (stmt)) == cfun->function_end_locus)
    return false;

  count_info old_info;
  get_count_info (stmt, &old_info);
  gcov_type total = 0;
  for (icall_target_map::const_iterator iter = old_info.targets.begin();
       iter != old_info.targets.end(); ++iter)
    total += iter->second;

  /* Program behavior changed, original promoted (and inlined) target is not
     hot any more. Will avoid promote the original target.

     To check if original promoted target is still hot, we check the total
     count of the unpromoted targets (stored in old_info). If it is no less
     than half of the callsite count (stored in INFO), the original promoted
     target is considered not hot any more.  */
  if (total >= info->count * 0.5)
    return false;

  inline_stack stack;
  get_inline_stack (gimple_location (stmt), &stack);
  if (stack.size () == 0)
    return false;
  const function_instance *s = get_function_instance_by_inline_stack (stack);
  if (s == NULL)
    return false;
  icall_target_map map;
  if (s->find_icall_target_map (stmt, &map) == 0)
    return false;
  for (icall_target_map::const_iterator iter = map.begin();
       iter != map.end(); ++iter)
    info->targets[iter->first] = iter->second;
  return true;
}

/* Find total count of the callee of EDGE.  */

gcov_type
autofdo_source_profile::get_callsite_total_count (
    struct cgraph_edge *edge) const
{
  inline_stack stack;
  stack.push_back (std::make_pair(edge->callee->decl, 0));
  get_inline_stack (gimple_location (edge->call_stmt), &stack);

  const function_instance *s = get_function_instance_by_inline_stack (stack);
  if (s == NULL)
    return 0;
  else
    return s->total_count ();
}

/* Read AutoFDO profile and returns TRUE on success.  */

bool
autofdo_source_profile::read ()
{
  if (gcov_read_unsigned () != GCOV_TAG_AFDO_FUNCTION)
    {
      inform (0, "Not expected TAG.");
      return false;
    }

  /* Skip the length of the section.  */
  gcov_read_unsigned ();

  /* Read in the function/callsite profile, and store it in local
     data structure.  */
  unsigned function_num = gcov_read_unsigned ();
  for (unsigned i = 0; i < function_num; i++)
    {
      function_instance::function_instance_stack stack;
      function_instance *s = function_instance::read_function_instance (
	  &stack, gcov_read_counter ());
      afdo_profile_info->sum_all += s->total_count ();
      map_[s->name ()] = s;
    }
  return true;
}

/* Return the function_instance in the profile that correspond to the
   inline STACK.  */

function_instance *
autofdo_source_profile::get_function_instance_by_inline_stack (
    const inline_stack &stack) const
{
  name_function_instance_map::const_iterator iter = map_.find (
      afdo_string_table->get_index_by_decl (
	  stack[stack.size() - 1].first));
  return iter == map_.end()
      ? NULL
      : iter->second->get_function_instance (stack, stack.size() - 1);
}


/* Member functions for autofdo_module_profile.  */

bool
autofdo_module_profile::read ()
{
  /* Read in the module info.  */
  if (gcov_read_unsigned () != GCOV_TAG_AFDO_MODULE_GROUPING)
    {
      inform (0, "Not expected TAG.");
      return false;
    }
  /* Skip the length of the section.  */
  gcov_read_unsigned ();

  /* Read in the file name table.  */
  unsigned total_module_num = gcov_read_unsigned ();
  for (unsigned i = 0; i < total_module_num; i++)
    {
      char *name = xstrdup (gcov_read_string ());
      unsigned total_num = 0;
      unsigned num_array[7];
      unsigned exported = gcov_read_unsigned ();
      unsigned lang = gcov_read_unsigned ();
      unsigned ggc_memory = gcov_read_unsigned ();
      for (unsigned j = 0; j < 7; j++)
	{
	  num_array[j] = gcov_read_unsigned ();
	  total_num += num_array[j];
	}
      gcov_module_info *module = XCNEWVAR (
	  gcov_module_info,
	  sizeof (gcov_module_info) + sizeof (char *) * total_num);
      
      std::pair<name_target_map::iterator, bool> ret = map_.insert(
	  name_target_map::value_type (name, AuxInfo()));
      gcc_assert (ret.second);
      ret.first->second.second = module;
      module->ident = i + 1;
      module->lang = lang;
      module->ggc_memory = ggc_memory;
      module->num_quote_paths = num_array[1];
      module->num_bracket_paths = num_array[2];
      module->num_system_paths = num_array[3];
      module->num_cpp_defines = num_array[4];
      module->num_cpp_includes = num_array[5];
      module->num_cl_args = num_array[6];
      module->source_filename = name;
      module->is_primary = strcmp (name, in_fnames[0]) == 0;
      module->flags = module->is_primary ? exported : 1;
      for (unsigned j = 0; j < num_array[0]; j++)
	ret.first->second.first.push_back (xstrdup (gcov_read_string ()));
      for (unsigned j = 0; j < total_num - num_array[0]; j++)
	module->string_array[j] = xstrdup (gcov_read_string ());
    }
  return true;
}

/* Read the profile from the profile file.  */

static void
read_profile (void)
{
  if (gcov_open (auto_profile_file, 1) == 0)
    error ("Cannot open profile file %s.", auto_profile_file);

  if (gcov_read_unsigned () != GCOV_DATA_MAGIC)
    error ("AutoFDO profile magic number does not mathch.");

  /* Skip the version number.  */
  gcov_read_unsigned ();

  /* Skip the empty integer.  */
  gcov_read_unsigned ();

  /* string_table.  */
  afdo_string_table = string_table::create ();
  if (afdo_string_table == NULL)
    error ("Cannot read string table from %s.", auto_profile_file);

  /* autofdo_source_profile.  */
  afdo_source_profile = autofdo_source_profile::create ();
  if (afdo_source_profile == NULL)
    error ("Cannot read function profile from %s.", auto_profile_file);

  /* autofdo_module_profile.  */
  afdo_module_profile = autofdo_module_profile::create ();
  if (afdo_module_profile == NULL)
    error ("Cannot read module profile from %s.", auto_profile_file);

  /* Read in the working set.  */
  if (gcov_read_unsigned () != GCOV_TAG_AFDO_WORKING_SET)
    error ("Cannot read working set from %s.", auto_profile_file);

  /* Skip the length of the section.  */
  gcov_read_unsigned ();
  gcov_working_set_t set[128];
  for (unsigned i = 0; i < 128; i++)
    {
      set[i].num_counters = gcov_read_unsigned ();
      set[i].min_counter = gcov_read_counter ();
    }
  add_working_set (set);
}

/* Read in the auxiliary modules for the current primary module.  */

static void
read_aux_modules (void)
{
  gcov_module_info *module = afdo_module_profile->get_module (in_fnames[0]);
  if (module == NULL)
    return;

  const string_vector *aux_modules =
      afdo_module_profile->get_aux_modules (in_fnames[0]);
  unsigned num_aux_modules = aux_modules ? aux_modules->size() : 0;

  module_infos = XCNEWVEC (gcov_module_info *, num_aux_modules + 1);
  module_infos[0] = module;
  primary_module_id = module->ident;
  record_module_name (module->ident, lbasename (in_fnames[0]));
  if (aux_modules == NULL)
    return;
  unsigned curr_module = 1, max_group = PARAM_VALUE (PARAM_MAX_LIPO_GROUP);
  for (string_vector::const_iterator iter = aux_modules->begin();
       iter != aux_modules->end(); ++iter)
    {
      gcov_module_info *aux_module = afdo_module_profile->get_module (*iter);
      if (aux_module == module)
	continue;
      if (aux_module == NULL)
	{
	  if (flag_opt_info)
	    inform (0, "aux module %s cannot be found.", *iter);
	  continue;
	}
      if ((aux_module->lang & GCOV_MODULE_LANG_MASK) !=
	  (module->lang & GCOV_MODULE_LANG_MASK))
	{
	  if (flag_opt_info)
	    inform (0, "Not importing %s: source language"
		    " different from primary module's source language", *iter);
	  continue;
	}
      if ((aux_module->lang & GCOV_MODULE_ASM_STMTS)
	   && flag_ripa_disallow_asm_modules)
	{
	  if (flag_opt_info)
	    inform (0, "Not importing %s: contains "
		    "assembler statements", *iter);
	  continue;
	}
      if (max_group != 0 && curr_module >= max_group)
	{
	  if (flag_opt_info)
	    inform (0, "Not importing %s: maximum group size reached", *iter);
	  continue;
	}
      if (incompatible_cl_args (module, aux_module))
	{
	  if (flag_opt_info)
	    inform (0, "Not importing %s: command-line"
		    " arguments not compatible with primary module", *iter);
	  continue;
	}
      module_infos[curr_module++] = aux_module;
      add_input_filename (*iter);
      record_module_name (aux_module->ident, lbasename (*iter));
    }
}

/* From AutoFDO profiles, find values inside STMT for that we want to measure
   histograms for indirect-call optimization.  */

static void
afdo_indirect_call (gimple_stmt_iterator *gsi, const icall_target_map &map)
{
  gimple stmt = gsi_stmt (*gsi);
  tree callee;

  if (map.size() == 0 || gimple_code (stmt) != GIMPLE_CALL
      || gimple_call_fndecl (stmt) != NULL_TREE)
    return;

  callee = gimple_call_fn (stmt);

  histogram_value hist = gimple_alloc_histogram_value (
      cfun, HIST_TYPE_INDIR_CALL_TOPN, stmt, callee);
  hist->n_counters = (GCOV_ICALL_TOPN_VAL << 2) + 1;
  hist->hvalue.counters =  XNEWVEC (gcov_type, hist->n_counters);
  gimple_add_histogram_value (cfun, stmt, hist);

  gcov_type total = 0;
  icall_target_map::const_iterator max_iter1 = map.end();
  icall_target_map::const_iterator max_iter2 = map.end();

  for (icall_target_map::const_iterator iter = map.begin();
       iter != map.end(); ++iter)
    {
      total += iter->second;
      if (max_iter1 == map.end() || max_iter1->second < iter->second)
	{
	  max_iter2 = max_iter1;
	  max_iter1 = iter;
	}
      else if (max_iter2 == map.end() || max_iter2->second < iter->second)
	max_iter2 = iter;
    }

  hist->hvalue.counters[0] = total;
  hist->hvalue.counters[1] = (unsigned long long)
      afdo_string_table->get_name (max_iter1->first);
  hist->hvalue.counters[2] = max_iter1->second;
  if (max_iter2 != map.end())
    {
      hist->hvalue.counters[3] = (unsigned long long)
	  afdo_string_table->get_name (max_iter2->first);
      hist->hvalue.counters[4] = max_iter2->second;
    }
  else
    {
      hist->hvalue.counters[3] = 0;
      hist->hvalue.counters[4] = 0;
    }
}

/* From AutoFDO profiles, find values inside STMT for that we want to measure
   histograms and adds them to list VALUES.  */

static void
afdo_vpt (gimple_stmt_iterator *gsi, const icall_target_map &map)
{
  afdo_indirect_call (gsi, map);
}

/* For a given BB, return its execution count. Add the location of annotated
   stmt to ANNOTATED. Attach value profile if a stmt is not in PROMOTED,
   because we only want to promot an indirect call once.  */

static gcov_type
afdo_get_bb_count (basic_block bb, const stmt_set &promoted)
{
  gimple_stmt_iterator gsi;
  edge e;
  edge_iterator ei;
  gcov_type max_count = 0;
  bool has_annotated = false;

  for (gsi = gsi_start_bb (bb); !gsi_end_p (gsi); gsi_next (&gsi))
    {
      count_info info;
      gimple stmt = gsi_stmt (gsi);
      if (stmt->code == GIMPLE_DEBUG)
	continue;
      if (afdo_source_profile->get_count_info (stmt, &info))
	{
	  if (info.annotated)
	    continue;
	  if (info.count > max_count)
	    max_count = info.count;
	  has_annotated = true;
	  if (info.targets.size() > 0 && promoted.find (stmt) == promoted.end ())
	    afdo_vpt (&gsi, info.targets);
	}
    }

  if (!has_annotated)
    return 0;

  for (gsi = gsi_start_bb (bb); !gsi_end_p (gsi); gsi_next (&gsi))
    afdo_source_profile->mark_annotated (gimple_location (gsi_stmt (gsi)));
  for (gsi = gsi_start_phis (bb); !gsi_end_p (gsi); gsi_next (&gsi))
    {
      gimple phi = gsi_stmt (gsi);
      size_t i;
      for (i = 0; i < gimple_phi_num_args (phi); i++)
	afdo_source_profile->mark_annotated (gimple_phi_arg_location (phi, i));
    }
  FOR_EACH_EDGE (e, ei, bb->succs)
    afdo_source_profile->mark_annotated (e->goto_locus);

  bb->flags |= BB_ANNOTATED;
  return max_count;
}

/* BB1 and BB2 are in an equivalent class iff:
   1. BB1 dominates BB2.
   2. BB2 post-dominates BB1.
   3. BB1 and BB2 are in the same loop nest.
   This function finds the equivalent class for each basic block, and
   stores a pointer to the first BB in its equivalent class. Meanwhile,
   set bb counts for the same equivalent class to be idenical.  */

static void
afdo_find_equiv_class (void)
{
  basic_block bb;

  FOR_ALL_BB_FN (bb, cfun)
    bb->aux = NULL;

  FOR_ALL_BB_FN (bb, cfun)
    {
      vec<basic_block> dom_bbs;
      basic_block bb1;
      int i;

      if (bb->aux != NULL)
	continue;
      bb->aux = bb;
      dom_bbs = get_all_dominated_blocks (CDI_DOMINATORS, bb);
      FOR_EACH_VEC_ELT (dom_bbs, i, bb1)
	if (bb1->aux == NULL
	    && dominated_by_p (CDI_POST_DOMINATORS, bb, bb1)
	    && bb1->loop_father == bb->loop_father)
	  {
	    bb1->aux = bb;
	    if (bb1->count > bb->count && (bb1->flags & BB_ANNOTATED) != 0)
	      {
		bb->count = MAX (bb->count, bb1->count);
		bb->flags |= BB_ANNOTATED;
	      }
	  }
      dom_bbs = get_all_dominated_blocks (CDI_POST_DOMINATORS, bb);
      FOR_EACH_VEC_ELT (dom_bbs, i, bb1)
	if (bb1->aux == NULL
	    && dominated_by_p (CDI_DOMINATORS, bb, bb1)
	    && bb1->loop_father == bb->loop_father)
	  {
	    bb1->aux = bb;
	    if (bb1->count > bb->count && (bb1->flags & BB_ANNOTATED) != 0)
	      {
		bb->count = MAX (bb->count, bb1->count);
		bb->flags |= BB_ANNOTATED;
	      }
	  }
    }
}

/* If a basic block's count is known, and only one of its in/out edges' count
   is unknown, its count can be calculated.
   Meanwhile, if all of the in/out edges' counts are known, then the basic
   block's unknown count can also be calculated.
   IS_SUCC is true if out edges of a basic blocks are examined.
   Return TRUE if any basic block/edge count is changed.  */

static bool
afdo_propagate_edge (bool is_succ)
{
  basic_block bb;
  bool changed = false;

  FOR_EACH_BB_FN (bb, cfun)
    {
      edge e, unknown_edge = NULL;
      edge_iterator ei;
      int num_unknown_edge = 0;
      gcov_type total_known_count = 0;

      FOR_EACH_EDGE (e, ei, is_succ ? bb->succs : bb->preds)
	if ((e->flags & EDGE_ANNOTATED) == 0)
	  num_unknown_edge ++, unknown_edge = e;
	else
	  total_known_count += e->count;

      if (num_unknown_edge == 0)
	{
	  if (total_known_count > bb->count)
	    {
	      bb->count = total_known_count;
	      changed = true;
	    }
	  if ((bb->flags & BB_ANNOTATED) == 0)
	    {
	      bb->flags |= BB_ANNOTATED;
	      changed = true;
	    }
	}
      else if (num_unknown_edge == 1
	       && (bb->flags & BB_ANNOTATED) != 0)
	{
	  if (bb->count >= total_known_count)
	    unknown_edge->count = bb->count - total_known_count;
	  else
	    unknown_edge->count = 0;
	  unknown_edge->flags |= EDGE_ANNOTATED;
	  changed = true;
	}
    }
  return changed;
}

/* Special propagation for circuit expressions. Because GCC translates
   control flow into data flow for circuit expressions. E.g.
   BB1:
   if (a && b)
     BB2
   else
     BB3

   will be translated into:

   BB1:
     if (a)
       goto BB.t1
     else
       goto BB.t3
   BB.t1:
     if (b)
       goto BB.t2
     else
       goto BB.t3
   BB.t2:
     goto BB.t3
   BB.t3:
     tmp = PHI (0 (BB1), 0 (BB.t1), 1 (BB.t2)
     if (tmp)
       goto BB2
     else
       goto BB3

   In this case, we need to propagate through PHI to determine the edge
   count of BB1->BB.t1, BB.t1->BB.t2.  */

static void
afdo_propagate_circuit (void)
{
  basic_block bb;
  FOR_ALL_BB_FN (bb, cfun)
    {
      gimple phi_stmt;
      tree cmp_rhs, cmp_lhs;
      gimple cmp_stmt = last_stmt (bb);
      edge e;
      edge_iterator ei;

      if (!cmp_stmt || gimple_code (cmp_stmt) != GIMPLE_COND)
	continue;
      cmp_rhs = gimple_cond_rhs (cmp_stmt);
      cmp_lhs = gimple_cond_lhs (cmp_stmt);
      if (!TREE_CONSTANT (cmp_rhs)
	  || !(integer_zerop (cmp_rhs) || integer_onep (cmp_rhs)))
	continue;
      if (TREE_CODE (cmp_lhs) != SSA_NAME)
	continue;
      if ((bb->flags & BB_ANNOTATED) == 0)
	continue;
      phi_stmt = SSA_NAME_DEF_STMT (cmp_lhs);
      while (phi_stmt && gimple_code (phi_stmt) == GIMPLE_ASSIGN
	     && gimple_assign_single_p (phi_stmt)
	     && TREE_CODE (gimple_assign_rhs1 (phi_stmt)) == SSA_NAME)
	phi_stmt = SSA_NAME_DEF_STMT (gimple_assign_rhs1 (phi_stmt));
      if (!phi_stmt || gimple_code (phi_stmt) != GIMPLE_PHI)
	continue;
      FOR_EACH_EDGE (e, ei, bb->succs)
	{
	  unsigned i, total = 0;
	  edge only_one;
	  bool check_value_one = (((integer_onep (cmp_rhs))
		    ^ (gimple_cond_code (cmp_stmt) == EQ_EXPR))
		    ^ ((e->flags & EDGE_TRUE_VALUE) != 0));
	  if ((e->flags & EDGE_ANNOTATED) == 0)
	    continue;
	  for (i = 0; i < gimple_phi_num_args (phi_stmt); i++)
	    {
	      tree val = gimple_phi_arg_def (phi_stmt, i);
	      edge ep = gimple_phi_arg_edge (phi_stmt, i);

	      if (!TREE_CONSTANT (val) || !(integer_zerop (val)
		  || integer_onep (val)))
		continue;
	      if (check_value_one ^ integer_onep (val))
		continue;
	      total++;
	      only_one = ep;
	    }
	  if (total == 1 && (only_one->flags & EDGE_ANNOTATED) == 0)
	    {
	      only_one->count = e->count;
	      only_one->flags |= EDGE_ANNOTATED;
	    }
	}
    }
}

/* Propagate the basic block count and edge count on the control flow
   graph. We do the propagation iteratively until stablize.  */

static void
afdo_propagate (void)
{
  basic_block bb;
  bool changed = true;
  int i = 0;

  FOR_ALL_BB_FN (bb, cfun)
    {
      bb->count = ((basic_block) bb->aux)->count;
      if ((((basic_block) bb->aux)->flags & BB_ANNOTATED) != 0)
	bb->flags |= BB_ANNOTATED;
    }

  while (changed && i++ < PARAM_VALUE (PARAM_AUTOFDO_MAX_PROPAGATE_ITERATIONS))
    {
      changed = false;

      if (afdo_propagate_edge (true))
	changed = true;
      if (afdo_propagate_edge (false))
	changed = true;
      afdo_propagate_circuit ();
    }
}

/* All information parsed from a location_t that will be stored into the ELF
   section.  */

struct locus_information_t {
  /* File name of the source file containing the branch.  */
  const char *filename;
  /* Line number of the branch location.  */
  unsigned lineno;
  /* Hash value calculated from function name, function length, branch site
     offset and discriminator, used to uniquely identify a branch across
     different source versions.  */
  char hash[33];
};

/* Return true iff file and lineno are available for the provided locus.
   Fill all fields of li with information about locus.  */

static bool
get_locus_information (location_t locus, locus_information_t* li) {
  if (locus == UNKNOWN_LOCATION || !LOCATION_FILE (locus))
    return false;
  li->filename = LOCATION_FILE (locus);
  li->lineno = LOCATION_LINE (locus);

  inline_stack stack;

  get_inline_stack (locus, &stack);
  if (stack.empty ())
    return false;

  tree function_decl = stack[0].first;

  if (!(function_decl && TREE_CODE (function_decl) == FUNCTION_DECL))
    return false;

  /* Get function_length, branch_offset and discriminator to identify branches
     across different source versions.  */
  unsigned function_lineno =
    LOCATION_LINE (DECL_SOURCE_LOCATION (function_decl));
  function *f = DECL_STRUCT_FUNCTION (function_decl);
  unsigned function_length = f? LOCATION_LINE (f->function_end_locus) -
	function_lineno : 0;
  unsigned branch_offset = li->lineno - function_lineno;
  int discriminator = get_discriminator_from_locus (locus);

  const char *fn_name = fndecl_name (function_decl);
  unsigned char md5_result[16];

  md5_ctx ctx;

  md5_init_ctx (&ctx);
  md5_process_bytes (fn_name, strlen (fn_name), &ctx);
  md5_process_bytes (&function_length, sizeof (function_length), &ctx);
  md5_process_bytes (&branch_offset, sizeof (branch_offset), &ctx);
  md5_process_bytes (&discriminator, sizeof (discriminator), &ctx);
  md5_finish_ctx (&ctx, md5_result);

  /* Convert MD5 to hexadecimal representation.  */
  for (int i = 0; i < 16; ++i)
    {
      sprintf (li->hash + i*2, "%02x", md5_result[i]);
    }

  return true;
}

/* Record branch prediction comparison for the given edge and actual
   probability.  */
static void
record_branch_prediction_results (edge e, int probability) {
  basic_block bb = e->src;

  if (bb->succs->length () == 2 &&
      maybe_hot_count_p (cfun, bb->count) &&
      bb->count >= check_branch_annotation_threshold)
    {
      gimple_stmt_iterator gsi;
      gimple last = NULL;

      for (gsi = gsi_last_nondebug_bb (bb);
	   !gsi_end_p (gsi);
	   gsi_prev_nondebug (&gsi))
	{
	  last = gsi_stmt (gsi);

	  if (gimple_has_location (last))
	    break;
	}

      struct locus_information_t li;
      bool annotated;

      if (e->flags & EDGE_PREDICTED_BY_EXPECT)
	annotated = true;
      else
	annotated = false;

      if (get_locus_information (e->goto_locus, &li))
	;  /* Intentionally do nothing.  */
      else if (get_locus_information (gimple_location (last), &li))
	;  /* Intentionally do nothing.  */
      else
	return;  /* Can't get locus information, return.  */

      switch_to_section (get_section (
	  ".gnu.switches.text.branch.annotation",
	  SECTION_DEBUG | SECTION_MERGE |
	  SECTION_STRINGS | (SECTION_ENTSIZE & 1),
	  NULL));
      char buf[1024];
      snprintf (buf, 1024, "%s;%u;"
		HOST_WIDEST_INT_PRINT_DEC";%d;%d;%d;%s",
		li.filename, li.lineno, bb->count, annotated?1:0,
		probability, e->probability, li.hash);
      dw2_asm_output_nstring (buf, (size_t)-1, NULL);
    }
}

/* Propagate counts on control flow graph and calculate branch
   probabilities.  */

static void
afdo_calculate_branch_prob (void)
{
  basic_block bb;
  bool has_sample = false;

  FOR_EACH_BB_FN (bb, cfun)
    if (bb->count > 0)
      has_sample = true;

  if (!has_sample)
    return;

  calculate_dominance_info (CDI_POST_DOMINATORS);
  calculate_dominance_info (CDI_DOMINATORS);
  loop_optimizer_init (0);

  afdo_find_equiv_class ();
  afdo_propagate ();

  FOR_EACH_BB_FN (bb, cfun)
    {
      edge e;
      edge_iterator ei;
      int num_unknown_succ = 0;
      gcov_type total_count = 0;

      FOR_EACH_EDGE (e, ei, bb->succs)
	{
	  if ((e->flags & EDGE_ANNOTATED) == 0)
	    num_unknown_succ ++;
	  else
	    total_count += e->count;
	}
      if (num_unknown_succ == 0 && total_count > 0)
	{
	  bool first_edge = true;

	  FOR_EACH_EDGE (e, ei, bb->succs)
	    {
	      double probability =
		  (double) e->count * REG_BR_PROB_BASE / total_count;

	      if (first_edge && flag_check_branch_annotation)
		{
		  record_branch_prediction_results (
		      e, static_cast<int> (probability + 0.5));
		  first_edge = false;
		}

	      e->probability = probability;
	    }
	}
    }
  FOR_ALL_BB_FN (bb, cfun)
    {
      edge e;
      edge_iterator ei;

      FOR_EACH_EDGE (e, ei, bb->succs)
	{
	  e->count =
		  (double) bb->count * e->probability / REG_BR_PROB_BASE;
	  if (flag_check_branch_annotation)
	    {
	      e->flags &= ~EDGE_PREDICTED_BY_EXPECT;
	    }
	}
      bb->aux = NULL;
    }

  loop_optimizer_finalize ();
  free_dominance_info (CDI_DOMINATORS);
  free_dominance_info (CDI_POST_DOMINATORS);
}

/* Perform value profile transformation using AutoFDO profile. Add the
   promoted stmts to PROMOTED_STMTS. Return TRUE if there is any
   indirect call promoted.  */

static bool
afdo_vpt_for_early_inline (stmt_set *promoted_stmts)
{
  basic_block bb;
  if (afdo_source_profile->get_function_instance_by_decl (
      current_function_decl) == NULL)
    return false;

  bool has_vpt = false;
  FOR_EACH_BB_FN (bb, cfun)
    {
      if (!has_indirect_call (bb))
	continue;
      gimple_stmt_iterator gsi;

      gcov_type bb_count = 0;
      for (gsi = gsi_start_bb (bb); !gsi_end_p (gsi); gsi_next (&gsi))
	{
	  count_info info;
	  gimple stmt = gsi_stmt (gsi);
	  if (afdo_source_profile->get_count_info (stmt, &info))
	    bb_count = MAX (bb_count, info.count);
	}

      for (gsi = gsi_start_bb (bb); !gsi_end_p (gsi); gsi_next (&gsi))
	{
	  gimple stmt = gsi_stmt (gsi);
	  /* IC_promotion and early_inline_2 is done in multiple iterations.
	     No need to promoted the stmt if its in promoted_stmts (means
	     it is already been promoted in the previous iterations).  */
	  if (gimple_code (stmt) != GIMPLE_CALL
	      || (gimple_call_fn (stmt) != NULL 
		  && TREE_CODE (gimple_call_fn (stmt)) == FUNCTION_DECL)
	      || promoted_stmts->find (stmt) != promoted_stmts->end ())
	    continue;

	  count_info info;
	  afdo_source_profile->get_count_info (stmt, &info);
	  info.count = bb_count;
	  if (afdo_source_profile->update_inlined_ind_target (stmt, &info))
	    {
	      /* Promote the indirect call and update the promoted_stmts.  */
	      promoted_stmts->insert (stmt);
	      afdo_vpt (&gsi, info.targets);
	      has_vpt = true;
	    }
	}
    }
  if (has_vpt && gimple_value_profile_transformations ())
    {
      free_dominance_info (CDI_DOMINATORS);
      free_dominance_info (CDI_POST_DOMINATORS);
      calculate_dominance_info (CDI_POST_DOMINATORS);
      calculate_dominance_info (CDI_DOMINATORS);
      update_ssa (TODO_update_ssa);
      rebuild_cgraph_edges ();
      return true;
    }
  else
    return false;
}

/* Annotate auto profile to the control flow graph. Do not annotate value
   profile for stmts in PROMOTED_STMTS.  */

static void
afdo_annotate_cfg (const stmt_set &promoted_stmts)
{
  basic_block bb;
  const function_instance *s =
      afdo_source_profile->get_function_instance_by_decl (
      current_function_decl);

  if (s == NULL)
    return;
  cgraph_get_node (current_function_decl)->count = s->head_count ();
  ENTRY_BLOCK_PTR_FOR_FN (cfun)->count = s->head_count ();
  gcov_type max_count = ENTRY_BLOCK_PTR_FOR_FN (cfun)->count;

  FOR_EACH_BB_FN (bb, cfun)
    {
      edge e;
      edge_iterator ei;

      bb->count = 0;
      bb->flags &= (~BB_ANNOTATED);
      FOR_EACH_EDGE (e, ei, bb->succs)
	{
	  e->count = 0;
	  e->flags &= (~EDGE_ANNOTATED);
	}

      bb->count = afdo_get_bb_count (bb, promoted_stmts);
      if (bb->count > max_count)
	max_count = bb->count;
    }
  if (ENTRY_BLOCK_PTR_FOR_FN (cfun)->count >
      ENTRY_BLOCK_PTR_FOR_FN (cfun)->next_bb->count)
    {
      ENTRY_BLOCK_PTR_FOR_FN (cfun)->next_bb->count =
	  ENTRY_BLOCK_PTR_FOR_FN (cfun)->count;
      ENTRY_BLOCK_PTR_FOR_FN (cfun)->next_bb->flags |= BB_ANNOTATED;
    }
  if (ENTRY_BLOCK_PTR_FOR_FN (cfun)->count >
      EXIT_BLOCK_PTR_FOR_FN (cfun)->prev_bb->count)
    {
      EXIT_BLOCK_PTR_FOR_FN (cfun)->prev_bb->count =
	  ENTRY_BLOCK_PTR_FOR_FN (cfun)->count;
      EXIT_BLOCK_PTR_FOR_FN (cfun)->prev_bb->flags |= BB_ANNOTATED;
    }
  afdo_source_profile->mark_annotated (
      DECL_SOURCE_LOCATION (current_function_decl));
  afdo_source_profile->mark_annotated (cfun->function_start_locus);
  afdo_source_profile->mark_annotated (cfun->function_end_locus);
  if (max_count > 0)
    {
      profile_status_for_fn (cfun) = PROFILE_READ;
      afdo_calculate_branch_prob ();
      counts_to_freqs ();
    }
  if (flag_value_profile_transformations)
    {
      gimple_value_profile_transformations ();
      free_dominance_info (CDI_DOMINATORS);
      free_dominance_info (CDI_POST_DOMINATORS);
      calculate_dominance_info (CDI_POST_DOMINATORS);
      calculate_dominance_info (CDI_DOMINATORS);
      update_ssa (TODO_update_ssa);
    }
}

/* Wrapper function to invoke early inliner.  */

static void early_inline ()
{
  compute_inline_parameters (cgraph_get_node (current_function_decl), true);
  unsigned todo = early_inliner ();
  if (todo & TODO_update_ssa_any)
    update_ssa (TODO_update_ssa);
}

/* Use AutoFDO profile to annoate the control flow graph.
   Return the todo flag.  */

static unsigned int
auto_profile (void)
{
  struct cgraph_node *node;

  if (cgraph_state == CGRAPH_STATE_FINISHED)
    return 0;

  if (!flag_auto_profile)
    return 0;

  profile_info = autofdo::afdo_profile_info;
  if (L_IPO_COMP_MODE)
    lipo_link_and_fixup ();
  init_node_map (true);

  FOR_EACH_FUNCTION (node)
    {
      if (!gimple_has_body_p (node->decl))
	continue;

      /* Don't profile functions produced for builtin stuff.  */
      if (DECL_SOURCE_LOCATION (node->decl) == BUILTINS_LOCATION)
	continue;

      push_cfun (DECL_STRUCT_FUNCTION (node->decl));

      /* First do indirect call promotion and early inline to make the
	 IR match the profiled binary before actual annotation.

	 This is needed because an indirect call might have been promoted
	 and inlined in the profiled binary. If we do not promote and
	 inline these indirect calls before annotation, the profile for
	 these promoted functions will be lost.

	 e.g. foo() --indirect_call--> bar()
	 In profiled binary, the callsite is promoted and inlined, making
	 the profile look like:

	 foo: {
	   loc_foo_1: count_1
	   bar@loc_foo_2: {
	     loc_bar_1: count_2
	     loc_bar_2: count_3
	   }
	 }

	 Before AutoFDO pass, loc_foo_2 is not promoted thus not inlined.
	 If we perform annotation on it, the profile inside bar@loc_foo2
	 will be wasted.

	 To avoid this, we promote loc_foo_2 and inline the promoted bar
	 function before annotation, so the profile inside bar@loc_foo2
	 will be useful.  */
      autofdo::stmt_set promoted_stmts;
      for (int i = 0; i < PARAM_VALUE (PARAM_EARLY_INLINER_MAX_ITERATIONS); i++)
	{
	  if (!flag_value_profile_transformations
	      || !autofdo::afdo_vpt_for_early_inline (&promoted_stmts))
	    break;
	  early_inline ();
	}

      early_inline ();
      autofdo::afdo_annotate_cfg (promoted_stmts);
      compute_function_frequency ();

      /* Local pure-const may imply need to fixup the cfg.  */
      if (execute_fixup_cfg () & TODO_cleanup_cfg)
	cleanup_tree_cfg ();

      free_dominance_info (CDI_DOMINATORS);
      free_dominance_info (CDI_POST_DOMINATORS);
      rebuild_cgraph_edges ();
      compute_inline_parameters (cgraph_get_node (current_function_decl), true);
      pop_cfun ();
    }

  if (flag_auto_profile_record_coverage_in_elf)
    autofdo::afdo_source_profile->write_annotated_count ();
  return TODO_rebuild_cgraph_edges;
}
}  /* namespace autofdo.  */

/* Read the profile from the profile data file.  */

void
init_auto_profile (void)
{
  if (auto_profile_file == NULL)
    auto_profile_file = DEFAULT_AUTO_PROFILE_FILE;

  autofdo::afdo_profile_info = (struct gcov_ctr_summary *)
      xcalloc (1, sizeof (struct gcov_ctr_summary));
  autofdo::afdo_profile_info->runs = 1;
  autofdo::afdo_profile_info->sum_max = 0;
  autofdo::afdo_profile_info->sum_all = 0;

  /* Read the profile from the profile file.  */
  autofdo::read_profile ();

  if (flag_dyn_ipa)
    autofdo::read_aux_modules ();
}

/* Free the resources.  */

void
end_auto_profile (void)
{
  delete autofdo::afdo_source_profile;
  delete autofdo::afdo_string_table;
  delete autofdo::afdo_module_profile;
  profile_info = NULL;
}

/* Returns TRUE if EDGE is hot enough to be inlined early.  */

bool
afdo_callsite_hot_enough_for_early_inline (struct cgraph_edge *edge)
{
  gcov_type count =
      autofdo::afdo_source_profile->get_callsite_total_count (edge);
  if (count > 0)
    {
      bool is_hot;
      const struct gcov_ctr_summary *saved_profile_info = profile_info;
      /* At earling inline stage, profile_info is not set yet. We need to
	 temporarily set it to afdo_profile_info to calculate hotness.  */
      profile_info = autofdo::afdo_profile_info;
      is_hot = maybe_hot_count_p (NULL, count);
      profile_info = saved_profile_info;
      return is_hot;
    }
  else
    return false;
}

namespace {

const pass_data pass_data_ipa_auto_profile =
{
  SIMPLE_IPA_PASS,
  "afdo", /* name */
  OPTGROUP_NONE, /* optinfo_flags */
  true, /* has_gate */
  true, /* has_execute */
  TV_IPA_AUTOFDO, /* tv_id */
  0, /* properties_required */
  0, /* properties_provided */
  0, /* properties_destroyed */
  0, /* todo_flags_start */
  0, /* todo_flags_finish */
};

class pass_ipa_auto_profile : public simple_ipa_opt_pass
{
public:
  pass_ipa_auto_profile(gcc::context *ctxt)
    : simple_ipa_opt_pass(pass_data_ipa_auto_profile, ctxt)
  {}

  /* opt_pass methods: */
  bool gate () { return flag_auto_profile; }
  unsigned int execute () { return autofdo::auto_profile (); }

}; // class pass_ipa_auto_profile

} // anon namespace

simple_ipa_opt_pass *
make_pass_ipa_auto_profile (gcc::context *ctxt)
{
  return new pass_ipa_auto_profile (ctxt);
}
