/*	$OpenBSD: rthread_mutex.c,v 1.3 2017/08/15 07:06:29 guenther Exp $ */
/*
 * Copyright (c) 2017 Martin Pieuchot <mpi@openbsd.org>
 * Copyright (c) 2012 Philip Guenther <guenther@openbsd.org>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "rthread.h"

/*
 * States defined in "Futexes Are Tricky" 5.2
 */
enum {
	UNLOCKED = 0,
	LOCKED = 1,	/* locked without waiter */
	CONTENDED = 2,	/* threads waiting for this mutex */
};

#define SPIN_COUNT	128
#if defined(__i386__) || defined(__amd64__)
#define SPIN_WAIT()	asm volatile("pause": : : "memory")
#else
#define SPIN_WAIT()	do { } while (0)
#endif

static _atomic_lock_t static_init_lock = _SPINLOCK_UNLOCKED;

int
pthread_mutex_init(pthread_mutex_t *mutexp, const pthread_mutexattr_t *attr)
{
	pthread_mutex_t mutex;

	mutex = calloc(1, sizeof(*mutex));
	if (mutex == NULL)
		return (ENOMEM);

	//phal_mutex_init(&mutex->hal_handle);
	if (attr == NULL) {
		mutex->type = PTHREAD_MUTEX_DEFAULT;
		mutex->prioceiling = -1;
	} else {
		mutex->type = (*attr)->ma_type;
		mutex->prioceiling = (*attr)->ma_protocol ==
		    PTHREAD_PRIO_PROTECT ? (*attr)->ma_prioceiling : -1;
	}
	*mutexp = mutex;

	return (0);
}

int
pthread_mutex_destroy(pthread_mutex_t *mutexp)
{
	pthread_mutex_t mutex;

	if (mutexp == NULL || *mutexp == NULL)
		return (EINVAL);

	mutex = *mutexp;
	if (mutex) {
		if (mutex->lock != UNLOCKED) {
#define MSG "pthread_mutex_destroy on mutex with waiters!\n"
			write(2, MSG, sizeof(MSG) - 1);
#undef MSG
			return (EBUSY);
		}
		free((void *)mutex);
		*mutexp = NULL;
	}

	return (0);
}

static int
_rthread_mutex_trylock(pthread_mutex_t mutex, int trywait,
    const struct timespec *abs)
{
	pthread_t self = pthread_self();
	int unlocked = UNLOCKED;
	if (atomic_compare_exchange_strong(&mutex->lock, &unlocked, LOCKED)) {
		// TODO: Membar
		//membar_enter_after_atomic();
		mutex->owner = self;
		return (0);
	}

	if (mutex->owner == self) {
		int type = mutex->type;

		/* already owner?  handle recursive behavior */
		if (type != PTHREAD_MUTEX_RECURSIVE) {
			if (trywait || type == PTHREAD_MUTEX_ERRORCHECK)
				return (trywait ? EBUSY : EDEADLK);

			/* self-deadlock is disallowed by strict */
			if (type == PTHREAD_MUTEX_STRICT_NP && abs == NULL)
				abort();

			/* self-deadlock, possibly until timeout */
			phal_semaphore_lock(&mutex->sem);
			phal_semaphore_wait(&mutex->sem, abs);
			phal_semaphore_unlock(&mutex->sem);
			return (ETIMEDOUT);
		} else {
			if (mutex->count == INT_MAX)
				return (EAGAIN);
			mutex->count++;
			return (0);
		}
	}

	return (EBUSY);
}

static int
_rthread_mutex_timedlock(pthread_mutex_t *mutexp, int trywait,
    const struct timespec *abs, int timed)
{
	pthread_t self = pthread_self();
	pthread_mutex_t mutex;
	unsigned int i;
	int lock, error = 0;

	if (mutexp == NULL)
		return (EINVAL);

	/*
	 * If the mutex is statically initialized, perform the dynamic
	 * initialization. Note: _thread_mutex_lock() in libc requires
	 * pthread_mutex_lock() to perform the mutex init when *mutexp
	 * is NULL.
	 */
	if (*mutexp == NULL) {
		_spinlock(&static_init_lock);
		if (*mutexp == NULL)
			error = pthread_mutex_init(mutexp, NULL);
		_spinunlock(&static_init_lock);
		if (error != 0)
			return (EINVAL);
	}

	mutex = *mutexp;
	_rthread_debug(5, "%p: mutex_%slock %p (%p)\n", self,
	    (timed ? "timed" : (trywait ? "try" : "")), (void *)mutex,
	    (void *)mutex->owner);

	error = _rthread_mutex_trylock(mutex, trywait, abs);
	if (error != EBUSY || trywait)
		return (error);

	/* Try hard to not enter the kernel. */
	for (i = 0; i < SPIN_COUNT; i ++) {
		if (mutex->lock == UNLOCKED)
			break;

		SPIN_WAIT();
	}

	lock = UNLOCKED;
	atomic_compare_exchange_strong(&mutex->lock, &lock, LOCKED);
	if (lock == UNLOCKED) {
		// TODO: membar
		//membar_enter_after_atomic();
		mutex->owner = self;
		return (0);
	}

	if (lock != CONTENDED) {
		/* Indicate that we're waiting on this mutex. */
		lock = atomic_exchange(&mutex->lock, CONTENDED);
	}

	while (lock != UNLOCKED) {
		// We don't *actually* need the lock here, as we are only using this to
		// allow cross-thread signaling.
		// If I could remove the mutex, I would, but I think switch's
		// implementation actually requires a valid, locked mutex.
		// TODO: error handling.
		phal_semaphore_lock(&mutex->sem);
		error = phal_semaphore_wait(&mutex->sem, abs);
		phal_semaphore_unlock(&mutex->sem);
		if (error == ETIMEDOUT)
			return (error);
		/*
		 * We cannot know if there's another waiter, so in
		 * doubt set the state to CONTENDED.
		 */
		lock = atomic_exchange(&mutex->lock, CONTENDED);
	};

	// TODO: membar
	//membar_enter_after_atomic();
	mutex->owner = self;
	return (0);
}

int
pthread_mutex_trylock(pthread_mutex_t *mutexp)
{
	return (_rthread_mutex_timedlock(mutexp, 1, NULL, 0));
}

int
pthread_mutex_timedlock(pthread_mutex_t *mutexp, const struct timespec *abs)
{
	return (_rthread_mutex_timedlock(mutexp, 0, abs, 1));
}

int
pthread_mutex_lock(pthread_mutex_t *mutexp)
{
	return (_rthread_mutex_timedlock(mutexp, 0, NULL, 0));
}

int
pthread_mutex_unlock(pthread_mutex_t *mutexp)
{
	pthread_t self = pthread_self();
	pthread_mutex_t mutex;

	if (mutexp == NULL)
		return (EINVAL);

	if (*mutexp == NULL)
#if PTHREAD_MUTEX_DEFAULT == PTHREAD_MUTEX_ERRORCHECK
		return (EPERM);
#elif PTHREAD_MUTEX_DEFAULT == PTHREAD_MUTEX_NORMAL
		return(0);
#else
		abort();
#endif

	mutex = *mutexp;
	_rthread_debug(5, "%p: mutex_unlock %p (%p)\n", self, (void *)mutex,
	    (void *)mutex->owner);

	if (mutex->owner != self) {
	_rthread_debug(5, "%p: different owner %p (%p)\n", self, (void *)mutex,
	    (void *)mutex->owner);
		if (mutex->type == PTHREAD_MUTEX_ERRORCHECK ||
		    mutex->type == PTHREAD_MUTEX_RECURSIVE) {
			return (EPERM);
		} else {
			/*
			 * For mutex type NORMAL our undefined behavior for
			 * unlocking an unlocked mutex is to succeed without
			 * error.  All other undefined behaviors are to
			 * abort() immediately.
			 */
			if (mutex->owner == NULL &&
			    mutex->type == PTHREAD_MUTEX_NORMAL)
				return (0);
			else
				abort();

		}
	}

	if (mutex->type == PTHREAD_MUTEX_RECURSIVE) {
		if (mutex->count > 0) {
			mutex->count--;
			return (0);
		}
	}

	mutex->owner = NULL;
	// TODO: membar
	//membar_exit_before_atomic();
	if (atomic_fetch_sub(&mutex->lock, 1) != (UNLOCKED + 1)) {
		mutex->lock = UNLOCKED;
		phal_semaphore_lock(&mutex->sem);
		phal_semaphore_signal(&mutex->sem);
		phal_semaphore_unlock(&mutex->sem);
	}

	return (0);
}
