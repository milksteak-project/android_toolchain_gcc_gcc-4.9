/* Definitions for Linux-based GRTE (Google RunTime Environment).
   Copyright (C) 2009,2010,2011,2012 Free Software Foundation, Inc.
   Contributed by Chris Demetriou and Ollie Wild.

This file is part of GCC.

GCC is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 3, or (at your option)
any later version.

GCC is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

Under Section 7 of GPL version 3, you are granted additional
permissions described in the GCC Runtime Library Exception, version
3.1, as published by the Free Software Foundation.

You should have received a copy of the GNU General Public License and
a copy of the GCC Runtime Library Exception along with this program;
see the files COPYING3 and COPYING.RUNTIME respectively.  If not, see
<http://www.gnu.org/licenses/>.  */

/* Overrides LIB_LINUX_SPEC from sysv4.h.  */
#undef	LIB_LINUX_SPEC
#define LIB_LINUX_SPEC \
  "%{pthread:-lpthread} \
   %{shared:-lc} \
   %{!shared:%{mieee-fp:-lieee} %{profile:%(libc_p)}%{!profile:%(libc)}}"

/* When GRTE links statically, it needs its NSS and resolver libraries
   linked in as well.  Note that when linking statically, these are
   enclosed in a group by LINK_GCC_C_SEQUENCE_SPEC.  */
#undef LINUX_GRTE_EXTRA_SPECS
#define LINUX_GRTE_EXTRA_SPECS \
  { "libc", "%{static:%(libc_static);:-lc}" }, \
  { "libc_p", "%{static:%(libc_p_static);:-lc_p}" }, \
  { "libc_static", "-lc -lresolv" }, \
  { "libc_p_static", "-lc_p -lresolv_p" },
