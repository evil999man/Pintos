#include "threads/thread.h"
#include <debug.h>
#include <stddef.h>
#include <random.h>
#include <stdio.h>
#include <string.h>
#include "threads/flags.h"
#include "threads/interrupt.h"
#include "threads/intr-stubs.h"
#include "threads/palloc.h"
#include "threads/switch.h"
#include "threads/synch.h"
#include "threads/vaddr.h"
#ifdef USERPROG
#include "userprog/process.h"
#endif

enum sighandler_t Signal(int signum, enum sighandler_t handler) {
	if (signum == SIG_KILL) return 0;
	ASSERT (intr_get_level () == INTR_ON);
	enum intr_level old_level;
	old_level = intr_disable ();
	
	struct thread * cur = thread_current();
	enum sighandler_t old_handler = ((cur->mask >> signum) & 1) ? SIG_IGN : SIG_DFL;

	if (old_handler != handler) {
		cur->mask ^= (1 << signum);
	}

	intr_set_level (old_level);
	return old_handler;
}

int kill(int tid, int sig) {
	if (sig == SIG_CHLD || sig == SIG_CPU || tid <= 2) return -1;
	ASSERT (intr_get_level () == INTR_ON);
	enum intr_level old_level;
	old_level = intr_disable ();

	struct thread * x = thread_lookup(tid);

	if (x == NULL) {intr_set_level (old_level); return -1;}
	if (sig != SIG_KILL && ((x->mask >> sig) & 1)) {intr_set_level (old_level);return 0;}

	if (sig == SIG_UBLOCK) {
		if (x->status == THREAD_BLOCKED) {
			list_push_back(&to_unblock_list, &x->blkelem);
		}
		intr_set_level (old_level);
		return 0;
	}

	if (sig == SIG_KILL) {
		if (x->ptid != running_thread()->tid) {intr_set_level(old_level);return -1;}
	}
	if (x->signals[sig].type != -1) {
		x->signals[sig].sent_by = running_thread()->tid;
		intr_set_level (old_level);
		return 0;
	}
	x->signals[sig].type = sig;
	x->signals[sig].sent_by = running_thread()->tid;
	list_push_back(&x->signals_queue, &x->signals[sig].threadelem);
	intr_set_level (old_level);
	return 0;
}

// 0 - SIGBLOCK 1 - SIG_UNBLOCK 2 - SIG_SETMASK
int sigprocmask(int how, const sigset_t *set, sigset_t *oldset){
	if (set && *set >= (1 << NUM_SIGNAL)) return -1;
	ASSERT (intr_get_level () == INTR_ON);
	enum intr_level old_level;
	old_level = intr_disable ();
	
	struct thread * cur = running_thread();
	if (oldset) *oldset = cur->mask;
	if (set == NULL) {
		intr_set_level (old_level);
		return 0;	
	}
	if(how == SIG_BLOCK) {
		cur->mask |= (*set);
	}
	else if(how == SIG_UNBLOCK) {
		cur->mask &= ( ( (((sigset_t)1) << NUM_SIGNAL) - 1) ^ (*set) );
	}
	else if(how == SIG_SETMASK) {
		cur->mask = (*set);
	}
	else {
		intr_set_level (old_level);
		return -1;
	}
	intr_set_level (old_level);
	return 0;
}

int sigemptyset(sigset_t *set){
	if (!set) {
		return -1;
	}
	ASSERT (intr_get_level () == INTR_ON);
	enum intr_level old_level;
	old_level = intr_disable ();
	*set = 0;
	intr_set_level (old_level);
	return 0;
}

int sigfillset(sigset_t *set){
	if (!set) {
		return -1;
	}
	ASSERT (intr_get_level () == INTR_ON);
	enum intr_level old_level;
	old_level = intr_disable ();
	*set = ( ((sigset_t)1) << NUM_SIGNAL)-1;
	intr_set_level (old_level);
	return 0;
}

int sigaddset(sigset_t *set, int signum){
	if (signum >= NUM_SIGNAL || !set) {
		return -1;
	}
	ASSERT (intr_get_level () == INTR_ON);
	enum intr_level old_level;
	old_level = intr_disable ();
	*set |= ( ((sigset_t)1) << signum);
	intr_set_level (old_level);
	return 0;
}

int sigdelset(sigset_t *set, int signum){
	if(signum >= NUM_SIGNAL || !set){
		return -1;
	}
	ASSERT (intr_get_level () == INTR_ON);
	enum intr_level old_level;
	old_level = intr_disable ();
	*set &= ~(((sigset_t)1) << signum);
	intr_set_level (old_level);
	return 0;
}
	
void SIG_KILL_DFL(int by) {
	printf("%d Killed by %d\n", running_thread()->tid, by);
	thread_exit();
}

void SIG_USER_DFL(int by) {
	printf("%d sent SIG_USER to %d\n", by, running_thread()->tid);
}

void SIG_CPU_DFL(int by UNUSED) {
	printf("Lifetime of %d = %lld\n", running_thread()->tid, running_thread()->lifetime);
	thread_exit();
}

void SIG_CHLD_DFL(int by UNUSED) {
	running_thread()->alive--;
	printf("Thread %d: %d Children, %d alive\n", running_thread()->tid, running_thread()->total, running_thread()->alive);
}