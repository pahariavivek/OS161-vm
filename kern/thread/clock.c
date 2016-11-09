/*
 * Copyright (c) 2000, 2001, 2002, 2003, 2004, 2005, 2008, 2009
 *	The President and Fellows of Harvard College.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the University nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE UNIVERSITY AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE UNIVERSITY OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include <types.h>
#include <lib.h>
#include <cpu.h>
#include <wchan.h>
#include <clock.h>
#include <thread.h>
#include <threadlist.h>
#include <current.h>
#include <addrspace.h>

/*
 * Time handling.
 *
 * This is pretty primitive. A real kernel will typically have some
 * kind of support for scheduling callbacks to happen at specific
 * points in the future, usually with more resolution that one second.
 *
 * A real kernel also has to maintain the time of day; in OS/161 we
 * skimp on that because we have a known-good hardware clock.
 */

/*
 * Timing constants. These should be tuned along with any work done on
 * the scheduler.
 */
#define SCHEDULE_HARDCLOCKS	1	/* Reschedule every 1 hardclock. */
#define MIGRATE_HARDCLOCKS	16	/* Migrate every 16 hardclocks. */
#define SCHEDULE_REF_BIT 10

/*
 * Once a second, everything waiting on lbolt is awakened by CPU 0.
 */
static struct wchan *lbolt;

struct wchan *mybolt;

/*
 * Setup.
 */
void
hardclock_bootstrap(void)
{
	mybolt = NULL;
	mybolt = wchan_create("mybolt");
	if (mybolt == NULL) {
		panic("Couldn't create mybolt\n");
	}	
	
	lbolt = wchan_create("lbolt");
	if (lbolt == NULL) {
		panic("Couldn't create lbolt\n");
	}
	
	
}

/*
 * This is called once per second, on one processor, by the timer
 * code.
 */
void
timerclock(void)
{
	/* Just broadcast on lbolt */
	
	wchan_wakeall(lbolt);
	
	/*
	spinlock_acquire(&mybolt->wc_lock);
	
	struct threadlistnode *itervar;
	if (!threadlist_isempty(&mybolt->wc_threads))
	{
		for (itervar = ((mybolt->wc_threads).tl_head).tln_next ; itervar != &(mybolt->wc_threads.tl_tail) ; itervar = itervar->tln_next)
		{
			itervar->tln_self->t_sleeptime--;
			if (itervar->tln_self->t_sleeptime == 0)
			{
				threadlist_remove(&mybolt->wc_threads, itervar->tln_self);
				thread_make_runnable(itervar->tln_self, false);
			}
		}
	}
	
	spinlock_release(&mybolt->wc_lock);
	*/
	ref_bit_count++;
	if (ref_bit_count == RESET_INTERVAL)
	{
		ref_bit_count = 0;
		reset_reference_bit();
	}
	wchan_mywake(mybolt);
	//kprintf("DAS\n");
}

/*
 * This is called HZ times a second (on each processor) by the timer
 * code.
 */
void
hardclock(void)
{
	/*
	 * Collect statistics here as desired.
	 */

	curcpu->c_hardclocks++;
	if ((curcpu->c_hardclocks % SCHEDULE_HARDCLOCKS) == 0) {
		schedule();
	}
	if ((curcpu->c_hardclocks % MIGRATE_HARDCLOCKS) == 0) {
		thread_consider_migration();
	}
	thread_yield();
}

/*
 * Suspend execution for n seconds.
 */
void
clocksleep(int num_secs)
{
	while (num_secs > 0) {
		wchan_lock(lbolt);
		wchan_sleep(lbolt);
		num_secs--;
	}
}
