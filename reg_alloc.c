
/*---------------------------------------------------------------*/
/*---                                                         ---*/
/*--- This file (reg_alloc.c) is                              ---*/
/*--- Copyright (c) 2004 OpenWorks LLP.  All rights reserved. ---*/
/*---                                                         ---*/
/*---------------------------------------------------------------*/

/* Records information on virtual register live ranges.  Computed once
   and remains unchanged after that. */
typedef
   struct {
      /* Becomes live for the first time after this insn ... */
      Int live_after;
      /* Becomes dead for the last time before this insn ... */
      Int dead_before;
      /* The "home" spill slot, if needed.  Never changes. */
      Int spill_offset;
      Int spill_size;
      /* Preferencing info, if any. */
      Bool has_preference;
      HReg preferred_rreg; /* if True, where I would like to be */
   }
   VRegInfo;


/* Records information on real-register live ranges.  Computed once
   and remains unchanged after that. */
typedef
   struct {
      HReg rreg;
      /* Becomes live after this insn ... */
      Int live_after;
      /* Becomes dead before this insn ... */
      Int dead_before;
   }
   RRegInfo;


/* An array of the following structs comprises the running state of
   the allocator.  It indicates what the current disposition of each
   allocatable real register is.  The array gets updated as the
   allocator processes instructions. */
typedef
   struct {
      /* Which rreg is this for? */
      HReg rreg;
      /* What's it's current disposition? */
      enum { Free,     /* available for use */
             Unavail,  /* in a real-reg live range */
             Bound     /* in use (holding value of some vreg) */
           }
           disp;
      /* If RRegBound, what vreg is it bound to? */
      HReg vreg;
   }
   RRegState;



/* A target-independent register allocator for Valgrind.  Requires
   various functions which to deal abstractly with instructions and
   registers (of which it cannot have any target-specific knowledge).  

   Returns a new list of instructions, which (depending on
   the behaviour of mapRegs) may be in-place modifications of
   the original instructions.

   Requires that the incoming code has been generated using
   vreg numbers 0, 1 .. n_vregs-1.  Appearance of a vreg outside
   that range will cause an error.
*/
HInstr** doRegisterAllocation (

   /* Incoming virtual-registerised code */ 
   HInstr** instrs,
   Int      n_instrs,
   Int      n_vregs,

   /* An array listing all the real registers the allocator may use,
      in no particular order. */
   HReg* available_real_regs,
   Int   n_available_real_regs,

   /* Return True iff the given insn is a reg-reg move, in which
      case also return the src and dst regs. */
   Bool (*isMove) (HInstr*, HReg*, HReg*),

   /* Get info about register usage in this insn. */
   void (*getRegUsage) (HInstr*, HRegUsage*),

   /* Apply a reg-reg mapping to an insn. */
   void (*mapRegs) (HRegMap*, HInstr*),

   /* Return an insn to spill/restore a real reg to a spill slot
      offset. */
   HInstr* (*genSpill) ( HReg, Int ),
   HInstr* (*genRestore) ( HReg, Int )
)
{
   VRegInfo* vreg_info;
   HRegUsage reg_usage;


   /* --------- Stage 1: compute vreg live ranges. --------- */

   /* This is relatively simple, because (1) we only seek the complete
      end-to-end live range of each vreg, and are not interested in
      any holes in it, and (2) the vregs are conveniently numbered 0
      .. n_vregs-1, so we can just dump the results in a pre-allocated
      array. */

   vreg_info = NULL;
   if (n_vregs > 0)
      vreg_info = malloc(sizeof(VRegInfo) * n_vregs);

   for (iv = 0; iv < n_vregs; iv++) {
      vreg_info[iv].live_after     = -1;
      vreg_info[iv].dead_before    = -1;
      vreg_info[iv].spill_offset   = 0;
      vreg_info[iv].spill_size     = 0;
      vreg_info[iv].has_preference = False;
      vreg_info[iv].preferred_rreg = (HReg)(-1);
   }

   /* for each insn ... */
   for (ii = 0; ii < n_instrs; ii++) {

      (*getRegUsage)( instrs[ii], &reg_usage );

      /* for each reg mentioned in the insn ... */
      for (ih = 0; ih < reg_usage.n_used; ih++) {

         /* only interested in virtual registers right now. */
         if (!hregIsVirtual(reg_usage.hreg[ih]))
            continue;
         iv = hregNumber(reg_usage.hreg[ih]);
         assert(iv >= 0 && iv < n_vregs);
         switch (reg_usage.mode[ih]) {
            case HRmRead: 
               if (vreg_info[iv].live_after == -1)
                  panic("doRegisterAllocation: "
                        "first event for vreg is Read");
               vreg_info[iv].dead_before = ii;
               break;
            case HRmWrite:
               if (vreg_info[iv].live_after == -1)
                  vreg_info[iv].live_after = ii;
               vreg_info[iv].dead_before = ii + 1;
               break;
            case HRmModify:
               if (vreg_info[iv].live_after == -1)
                  panic("doRegisterAllocation: "
                        "first event for vreg is Modify");
               vreg_info[iv].dead_before = ii + 1;
               break;
            default:
               panic("doRegisterAllocation(1)");
         } /* switch */

      } /* iterate over registers */

   } /* iterate over insns */


   /* --------- Stage 2: compute rreg live ranges. --------- */

   /* This is more complex than Stage 1, because we need to compute
      exactly all the live ranges of all the allocatable real regs,
      and we don't know in advance how many there will be. */

   RRegInfo* rreg_info;
   Int rreg_info_size;
   Int rreg_info_used;

   rreg_info_used = 0;
   rreg_info_size = 4;
   rreg_info = malloc(rreg_info_size * sizeof(RRegInfo));

   /* We'll need to track live range start/end points seperately for
      each rreg.  Sigh. */
   Int* rreg_live_after;
   Int* rreg_dead_before;
   assert(n_available_real_regs > 0);
   rreg_live_after  = malloc(n_available_real_regs * sizeof(Int));
   rreg_dead_before = malloc(n_available_real_regs * sizeof(Int));

   for (ir = 0; ir < n_available_real_regs; ir++)
      rreg_live_after[i] = rreg_dead_before[i] = -1;

   /* for each insn ... */
   for (ii = 0; ii < n_instrs; ii++) {

      (*getRegUsage)( instrs[ii], &reg_usage );

      /* for each reg mentioned in the insn ... */
      for (ih = 0; ih < reg_usage.n_used; ih++) {

         Bool flush;
         Int flush_la, flush_db;

         hreg = reg_usage.hreg[ih];

         /* only interested in real registers right now. */
         if (hregIsVirtual(hreg))
            continue;

         /* Furthermore, we're not interested in this rreg unless it's
            one of the allocatable ones.  For example, it could be a
            stack pointer register, or some other register beyond our
            control, in which case we should just ignore it. */
         ir = hregToIndex(hreg, available_real_regs, 
                                n_available_real_regs);
         if (ir == -1) continue;

         flush = False;
         switch (reg_usage.mode[ih]) {
            case HRmWrite:
               flush    = True;
               flush_la = rreg_live_after[ir];
               flush_db = rreg_dead_before[ir];
               rreg_live_after[ir]  = ii;
               rreg_dead_before[ir] = ii+1;
               break;
            case HRmRead:
               if (rreg_live_after[ir] == -1)
                  panic("doRegisterAllocation: "
                        "first event for rreg is Read");
               rreg_dead_before[ir] = ii;
               break;
            case HRmModify:
               if (rreg_live_after[ir] == -1)
                  panic("doRegisterAllocation: "
                        "first event for rreg is Modify");
               rreg_dead_before[ir] = ii+1;
               break;
            default:
               panic("doRegisterAllocation(2)");
         }

         if (flush) {
            if (rreg_info_used == rreg_info_size) {
               panic("make rreg info array bigger(1)");
            }
            rreg_info[rreg_info_used].rreg = hreg;
            rreg_info[rreg_info_used].live_after = flush_la;
            rreg_info[rreg_info_used].dead_before = flush_db;
         }

      } /* iterate over regs in the instr */

   } /* iterate over instrs */

   /* Now finish up any live ranges left over. */
   for (ir = 0; ir < n_available_real_regs; ir++) {
      if (rreg_live_after[ir] == -1)
         continue;
      if (rreg_info_used == rreg_info_size) {
         panic("make rreg info array bigger(2)");
      }
      rreg_info[rreg_info_used].rreg = available_real_regs[ri];
      rreg_info[rreg_info_used].live_after = rreg_live_after[ir];
      rreg_info[rreg_info_used].dead_before = rreg_dead_before[ir];
   }

   free(rreg_live_after);
   free(rreg_dead_before);


   /* --------- Stage 3: allocate spill slots. --------- */

   /* Each spill slot is 8 bytes long.  For 128-bit vregs
      we'll have to allocate two spill slots.  For now, tho,
      ignore the 128-bit problem.

      Do a rank-based allocation of vregs to spill slot numbers.  We
      put as few values as possible in spill slows, but nevertheless
      need to have a spill slot available for all vregs, just in case.
   */
   //   max_ss_no = -1;

   for (i = 0; i < N_SPILL64S; i++)
      ss_busy_until_before[i] = 0;

   for (i = 0; i < n_vregs; i++) {

      /* True iff this vreg is unused. */
      if (vreg_info[i].live_after == -1)
         continue;

      /* Find the lowest-numbered spill slot which is available at the
         start point of this interval, and assign the interval to
         it. */
      for (j = 0; j < N_SPILL64S; j++)
         if (ss_busy_until_before[j] <= vreg_info[i].live_after)
            break;
      if (j == N_SPILL64S) {
         panic("N_SPILL64S is too low");
      }
      ss_busy_until_before[j] = vreg_info[i].dead_before;
      vreg_info[i].spill_offset = j * 8;
      //      if (j > max_ss_no)
      //   max_ss_no = j;
   }


   /* --------- Stage 4: establish rreg preferences --------- */

   /* It may be advantageous to allocating certain vregs to specific
      rregs, as a way of avoiding reg-reg moves later.  Here we
      establish which, if any, rreg each vreg would prefer to be in.
      Note that this constrains the allocator -- ideally we end up
      with as few as possible vregs expressing a preference. */

   /* For now, ignore this.  It's only an optimisation, not needed for
      correctness. */


   /* --------- Stage 5: process instructions --------- */

   /* This is the main loop of the allocator.  First, we need to
      correctly set up our running state, which tracks the status of
      each real register. */

   RRegState* rreg_state;
   rreg_state = malloc(n_available_real_regs * sizeof(RRegState));

   for (ir = 0; ir < n_available_real_regs; ir++) {
      rreg_state[ir].rreg = available_real_regs[ir];
      rreg_state[ir].disp = Free;
      rreg_state[ir].vreg = (HReg)(-1);
   }

   /* Process each insn in turn. */
   for (ii = 0; ii < n_instrs; ii++) {

      /* ------ Sanity checks ------ */

      /* Sanity check 1: all rregs with a hard live range crossing
         this insn must be marked as unavailable in the running
         state. */
      for (i = 0; i < rreg_info_used; i++) {
         if (rreg_info[i].live_after < ii 
             && ii < rreg_info[i].dead_before) {
            /* ii is the middle of a hard live range for some real reg.
               Check it's marked as such in the running state. */
            assert(rreg_state[rreg_info[i].rreg] == Unavail);
         }
      }

      /* Sanity check 2: conversely, all rregs marked as unavailable in
         the running state must have a corresponding hard live range
         entry in the rreg_info array. */
      for (ir = 0; ir < n_available_real_regs; ir++) {
         assert(rreg_state[ir].disp == Free 
                || rreg_state[ir].disp == Unavail
                || rreg_state[ir].disp == Bound);
         if (rreg_state[ir].disp != Unavail)
            continue;
         for (i = 0; i < rreg_info_used; i++) 
            if (rreg_info[i].rreg == rreg_state[ir].rreg
                && rreg_info[i].live_after < ii 
                && ii < rreg_info[i].dead_before) 
               break;
         /* If not so, we couldn't find a correspond HLR. */
         assert(i < rreg_info_used);
      }

      /* Sanity check 3: No vreg is bound to more than one rreg. */
      for (ir = 0; ir < n_available_real_regs; ir++) {
         if (rreg_state[ir].disp != Bound)
            continue;
         for (j = ir+1; j < n_available_real_regs; j++)
            if (rreg_state[j].disp == Bound)
               assert(rreg_state[ir].vreg != rreg_state[j].vreg);
      }

      /* Sanity check 4: all vreg-rreg bindings must bind registers of
         the same class. */
      for (ir = 0; ir < n_available_real_regs; ir++) {
         if (rreg_state[ir].disp != Bound)
            continue;
         assert(hregClass(reg_state[ir].rreg) 
                == hregClass(reg_state[ir].vreg));
         assert( hregIsVirtual(reg_state[ir].vreg));
         assert(!hregIsVirtual(reg_state[ir].rreg))
      }

      /* ------ end of Sanity checks ------ */

      /* Do various optimisations pertaining to register coalescing
         and preferencing:
            MOV  v -> v   coalescing
            MOV  v -> r   coalescing
         Not yet.
      */

      /* Update the local state.  Expire any v -> r bindings for 
	 vregs which have become dead. */
      for (ir = 0; ir < n_available_real_regs; ir++) {
         if (rreg_state[ir].disp != Bound)
            continue;
         iv = hregNumber(rreg_state[ir].vreg);
	 assert(iv >= 0 && iv < n_vregs);
         if (vreg_info[iv].dead_before == ii) {
	   /* It's just gone dead.  Free up the associated rreg. */
	   rreg_state[ir].disp = Free;
	   rreg_state[ir].vreg = INVALID_HREG;
	 }
      }

      /* Now we have to deal with rregs which are about to be made
         live by this instruction -- in other words, are entering into
         one of their live ranges.  If any such rreg holds a vreg, we
         will have to spill it in order to free up the rreg.

         Note we could do better:
         * Could move it into some other free rreg, if one is available 
         * Don't bother to spill if the spill-slot value is known to
           be consistent.

         Simplest way to do this is to iterate over the collection
         of rreg live ranges.
      */

   } /* iterate over insns */

}



/*---------------------------------------------------------------*/
/*---                                             reg_alloc.c ---*/
/*---------------------------------------------------------------*/
