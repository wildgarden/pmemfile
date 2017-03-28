/*
 * Copyright 2016-2017, Intel Corporation
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in
 *       the documentation and/or other materials provided with the
 *       distribution.
 *
 *     * Neither the name of the copyright holder nor the names of its
 *       contributors may be used to endorse or promote products derived
 *       from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */
#ifndef PMEMFILE_LOCKS_H
#define PMEMFILE_LOCKS_H

#include "libpmemobj/thread.h"

#include "out.h"
#include "pool.h"
#include "os_thread.h"

void rwlock_tx_wlock(os_rwlock_t *l);
void rwlock_tx_unlock_on_commit(os_rwlock_t *l);

void mutex_tx_lock(PMEMfilepool *pfp, PMEMmutex *mutexp);
void mutex_tx_unlock_on_abort(PMEMmutex *mutexp);
void mutex_tx_unlock_on_commit(PMEMmutex *mutexp);


/*
 * pmemobj_mutex_lock_nofail -- pmemobj_mutex_lock variant that never
 * fails from caller perspective. If pmemobj_mutex_lock failed, this function
 * aborts the program.
 */
static inline void
pmemobj_mutex_lock_nofail(PMEMobjpool *pop, PMEMmutex *mutexp)
{
	int ret = pmemobj_mutex_lock(pop, mutexp);
	if (ret) {
		errno = ret;
		FATAL("!pmemobj_mutex_lock");
	}
}

/*
 * pmemobj_mutex_unlock_nofail -- pmemobj_mutex_unlock variant that never
 * fails from caller perspective. If pmemobj_mutex_unlock failed, this function
 * aborts the program.
 */
static inline void
pmemobj_mutex_unlock_nofail(PMEMobjpool *pop, PMEMmutex *mutexp)
{
	int ret = pmemobj_mutex_unlock(pop, mutexp);
	if (ret) {
		errno = ret;
		FATAL("!pmemobj_mutex_unlock");
	}
}

#endif