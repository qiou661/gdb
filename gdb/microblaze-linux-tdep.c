/* Target-dependent code for Xilinx MicroBlaze.

   Copyright (C) 2009-2013 Free Software Foundation, Inc.

   This file is part of GDB.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 3 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program.  If not, see <http://www.gnu.org/licenses/>.  */

#include "defs.h"
#include "frame.h"
#include "inferior.h"
#include "symtab.h"
#include "target.h"
#include "gdbcore.h"
#include "gdbcmd.h"
#include "symfile.h"
#include "objfiles.h"
#include "regcache.h"
#include "value.h"
#include "osabi.h"
#include "regset.h"
#include "solib-svr4.h"
#include "microblaze-tdep.h"
#include "glibc-tdep.h"
#include "trad-frame.h"
#include "frame-unwind.h"
#include "tramp-frame.h"
#include "linux-tdep.h"

static int microblaze_debug_flag = 0;

static void
microblaze_debug (const char *fmt, ...)
{
  if (microblaze_debug_flag)
    {
       va_list args;

       va_start (args, fmt);
       printf_unfiltered ("MICROBLAZE LINUX: ");
       vprintf_unfiltered (fmt, args);
       va_end (args);
    }
}

static int
microblaze_linux_memory_remove_breakpoint (struct gdbarch *gdbarch, 
					   struct bp_target_info *bp_tgt)
{
  CORE_ADDR addr = bp_tgt->placed_address;
  const gdb_byte *bp;
  int val;
  int bplen;
  gdb_byte old_contents[BREAKPOINT_MAX];
  struct cleanup *cleanup;

  /* Determine appropriate breakpoint contents and size for this address.  */
  bp = gdbarch_breakpoint_from_pc (gdbarch, &addr, &bplen);
  if (bp == NULL)
    error (_("Software breakpoints not implemented for this target."));

  /* Make sure we see the memory breakpoints.  */
  cleanup = make_show_memory_breakpoints_cleanup (1);
  val = target_read_memory (addr, old_contents, bplen);

  /* If our breakpoint is no longer at the address, this means that the
     program modified the code on us, so it is wrong to put back the
     old value.  */
  if (val == 0 && memcmp (bp, old_contents, bplen) == 0)
  {
      val = target_write_raw_memory (addr, bp_tgt->shadow_contents, bplen);
      microblaze_debug ("microblaze_linux_memory_remove_breakpoint writing back to memory at addr 0x%lx\n", addr);
  }

  do_cleanups (cleanup);
  return val;
}

static void
microblaze_linux_sigtramp_cache (struct frame_info *next_frame,
				 struct trad_frame_cache *this_cache,
				 CORE_ADDR func, LONGEST offset,
				 int bias)
{
  CORE_ADDR base;
  CORE_ADDR gpregs;
  int regnum;
  struct gdbarch *gdbarch = get_frame_arch (next_frame);
  struct gdbarch_tdep *tdep = gdbarch_tdep (gdbarch);

  base = frame_unwind_register_unsigned (next_frame, MICROBLAZE_SP_REGNUM);
  if (bias > 0 && get_frame_address_in_block (next_frame) != func)
    /* See below, some signal trampolines increment the stack as their
       first instruction, need to compensate for that.  */
    base -= bias;

  /* Find the address of the register buffer.  */
  gpregs = base + offset;

  /* Registers saved on stack.  */
  for (regnum = 0; regnum < MICROBLAZE_BTR_REGNUM; regnum++)
    trad_frame_set_reg_addr (this_cache, regnum, 
			     gpregs + regnum * MICROBLAZE_REGISTER_SIZE);
  trad_frame_set_id (this_cache, frame_id_build (base, func));
}


static void
microblaze_linux_sighandler_cache_init (const struct tramp_frame *self,
					struct frame_info *next_frame,
					struct trad_frame_cache *this_cache,
					CORE_ADDR func)
{
  microblaze_linux_sigtramp_cache (next_frame, this_cache, func,
				   0 /* Offset to ucontext_t.  */
				   + 24 /* Offset to .reg.  */,
				   0);
}

static struct tramp_frame microblaze_linux_sighandler_tramp_frame = 
{
  SIGTRAMP_FRAME,
  4,
  {
    { 0x31800077, -1 }, /* addik R12,R0,119.  */
    { 0xb9cc0008, -1 }, /* brki R14,8.  */
    { TRAMP_SENTINEL_INSN },
  },
  microblaze_linux_sighandler_cache_init
};

const struct microblaze_gregset microblaze_linux_core_gregset;

static void
microblaze_linux_supply_core_gregset (const struct regset *regset,
                                   struct regcache *regcache,
                                   int regnum, const void *gregs, size_t len)
{
  microblaze_supply_gregset (&microblaze_linux_core_gregset, regcache,
                             regnum, gregs);
}

static void
microblaze_linux_collect_core_gregset (const struct regset *regset,
                                    const struct regcache *regcache,
                                    int regnum, void *gregs, size_t len)
{
  microblaze_collect_gregset (&microblaze_linux_core_gregset, regcache,
                              regnum, gregs);
}

static void
microblaze_linux_supply_core_fpregset (const struct regset *regset,
                                    struct regcache *regcache,
                                    int regnum, const void *fpregs, size_t len)
{
  /* FIXME.  */
  microblaze_supply_fpregset (regcache, regnum, fpregs);
}

static void
microblaze_linux_collect_core_fpregset (const struct regset *regset,
                                     const struct regcache *regcache,
                                     int regnum, void *fpregs, size_t len)
{
  /* FIXME.  */
  microblaze_collect_fpregset (regcache, regnum, fpregs);
}

static void
microblaze_linux_init_abi (struct gdbarch_info info,
			   struct gdbarch *gdbarch)
{
  struct gdbarch_tdep *tdep = gdbarch_tdep (gdbarch);

  tdep->gregset = regset_alloc (gdbarch, microblaze_linux_supply_core_gregset,
                                microblaze_linux_collect_core_gregset);
  tdep->sizeof_gregset = 200;

  linux_init_abi (info, gdbarch);

  set_gdbarch_memory_remove_breakpoint (gdbarch,
					microblaze_linux_memory_remove_breakpoint);

  /* Shared library handling.  */
  set_solib_svr4_fetch_link_map_offsets (gdbarch,
					 svr4_ilp32_fetch_link_map_offsets);

  /* Trampolines.  */
  tramp_frame_prepend_unwinder (gdbarch,
				&microblaze_linux_sighandler_tramp_frame);

  /* BFD target for core files.  */
  if (gdbarch_byte_order (gdbarch) == BFD_ENDIAN_BIG)
    set_gdbarch_gcore_bfd_target (gdbarch, "elf32-microblaze");
  else
    set_gdbarch_gcore_bfd_target (gdbarch, "elf32-microblazeel");


  /* Shared library handling.  */
  set_gdbarch_skip_trampoline_code (gdbarch, find_solib_trampoline_target);
  set_gdbarch_skip_solib_resolver (gdbarch, glibc_skip_solib_resolver);

  set_gdbarch_regset_from_core_section (gdbarch,
					microblaze_regset_from_core_section);

  /* Enable TLS support.  */
  set_gdbarch_fetch_tls_load_module_address (gdbarch,
                                             svr4_fetch_objfile_link_map);

}

/* -Wmissing-prototypes */
extern initialize_file_ftype _initialize_microblaze_linux_tdep;

void
_initialize_microblaze_linux_tdep (void)
{
  gdbarch_register_osabi (bfd_arch_microblaze, 0, GDB_OSABI_LINUX, 
			  microblaze_linux_init_abi);
}
