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

/*
 * inode.c -- inode operations
 */

#include <errno.h>
#include <inttypes.h>

#include "callbacks.h"
#include "data.h"
#include "dir.h"
#include "inode.h"
#include "inode_array.h"
#include "internal.h"
#include "locks.h"
#include "os_thread.h"
#include "out.h"

/*
 * pmfi_path -- returns one of the full paths inode can be reached on
 *
 * Only for debugging.
 */
const char *
pmfi_path(struct pmemfile_vinode *vinode)
{
#ifdef DEBUG
	if (!vinode)
		return NULL;
	if (!vinode->path)
		LOG(LTRC, "0x%lx: no vinode->path", vinode->tinode.oid.off);
	return vinode->path;
#else
	return NULL;
#endif
}

/*
 * vinode_ref -- increases inode runtime reference counter
 *
 * Does not need transaction.
 */
struct pmemfile_vinode *
vinode_ref(PMEMfilepool *pfp, struct pmemfile_vinode *vinode)
{
	(void) pfp;

	__sync_fetch_and_add(&vinode->ref, 1);
	return vinode;
}

#define BUCKET_SIZE 2

/* hash map bucket */
struct inode_map_bucket {
	struct {
		/* persistent inode */
		TOID(struct pmemfile_inode) pinode;

		/* volatile inode */
		struct pmemfile_vinode *vinode;
	} arr[BUCKET_SIZE];
};

/* inode->vinode hash map */
struct pmemfile_inode_map {
	os_rwlock_t rwlock;

	/* hash function coefficients */
	uint32_t hash_fun_a;
	uint32_t hash_fun_b;
	uint64_t hash_fun_p;

	/* number of elements in "buckets" */
	size_t sz;

	/* buckets */
	struct inode_map_bucket *buckets;

	/* number of used slots */
	size_t inodes;
};

/*
 * inode_map_rand_params -- randomizes coefficients of the hashing function
 */
static void
inode_map_rand_params(struct pmemfile_inode_map *c)
{
	// XXX use independent random pool
	do {
		c->hash_fun_a = (uint32_t)rand();
	} while (c->hash_fun_a == 0);
	c->hash_fun_b = (uint32_t)rand();
}

/*
 * inode_map_alloc -- allocates inode hash map
 */
struct pmemfile_inode_map *
inode_map_alloc()
{
	struct pmemfile_inode_map *c = calloc(1, sizeof(*c));

	c->sz = 2;
	c->buckets = calloc(1, c->sz * sizeof(c->buckets[0]));

	inode_map_rand_params(c);
	c->hash_fun_p = 32212254719ULL;

	os_rwlock_init(&c->rwlock);

	return c;
}

/*
 * inode_map_free -- destroys inode hash map
 */
void
inode_map_free(struct pmemfile_inode_map *c)
{
	for (unsigned i = 0; i < c->sz; ++i) {
		struct inode_map_bucket *bucket = &c->buckets[i];

		for (unsigned j = 0; j < BUCKET_SIZE; ++j)
			if (bucket->arr[j].vinode)
				FATAL("memory leak");
	}

	os_rwlock_destroy(&c->rwlock);
	free(c->buckets);
	free(c);
}

/*
 * inode_hash -- returns hash value of the inode
 */
static inline size_t
inode_hash(struct pmemfile_inode_map *c, TOID(struct pmemfile_inode) inode)
{
	return (c->hash_fun_a * inode.oid.off + c->hash_fun_b) % c->hash_fun_p;
}

/*
 * inode_map_rebuild -- rebuilds the whole inode hash map
 */
static bool
inode_map_rebuild(struct pmemfile_inode_map *c, size_t new_sz)
{
	struct inode_map_bucket *new_buckets =
			calloc(1, new_sz * sizeof(new_buckets[0]));
	size_t idx;

	for (size_t i = 0; i < c->sz; ++i) {
		struct inode_map_bucket *b = &c->buckets[i];

		for (unsigned j = 0; j < BUCKET_SIZE; ++j) {
			if (b->arr[j].pinode.oid.off == 0)
				continue;

			idx = inode_hash(c, b->arr[j].pinode) % new_sz;
			struct inode_map_bucket *newbucket = &new_buckets[idx];
			unsigned k;
			for (k = 0; k < BUCKET_SIZE; ++k) {
				if (newbucket->arr[k].pinode.oid.off == 0) {
					newbucket->arr[k] = b->arr[j];
					break;
				}
			}

			if (k == BUCKET_SIZE) {
				free(new_buckets);
				return false;
			}
		}
	}

	free(c->buckets);
	c->sz = new_sz;
	c->buckets = new_buckets;

	return true;
}

/*
 * vinode_unregister_locked -- removes vinode from inode map
 */
static void
vinode_unregister_locked(PMEMfilepool *pfp,
		struct pmemfile_vinode *vinode)
{
	struct pmemfile_inode_map *c = pfp->inode_map;

	size_t idx = inode_hash(c, vinode->tinode) % c->sz;
	struct inode_map_bucket *b = &c->buckets[idx];
	unsigned j;
	for (j = 0; j < BUCKET_SIZE; ++j) {
		if (b->arr[j].vinode == vinode) {
			memset(&b->arr[j], 0, sizeof(b->arr[j]));
			break;
		}
	}

	if (j == BUCKET_SIZE)
		FATAL("vinode not found");

	c->inodes--;

	vinode_destroy_data_state(pfp, vinode);

#ifdef DEBUG
	/* "path" field is defined only in DEBUG builds */
	free(vinode->path);
#endif
	os_rwlock_destroy(&vinode->rwlock);
	free(vinode);
}

/*
 * _inode_get -- deals with vinode life time related to inode
 */
static struct pmemfile_vinode *
_inode_get(PMEMfilepool *pfp, TOID(struct pmemfile_inode) inode,
		bool is_new, struct pmemfile_vinode *parent,
		volatile bool *parent_refed,
		const char *name, size_t namelen)
{
	struct pmemfile_inode_map *map = pfp->inode_map;
	int tx = 0;
	int error = 0;

	if (D_RO(inode)->version != PMEMFILE_INODE_VERSION(1)) {
		ERR("unknown inode version 0x%x for inode 0x%" PRIx64,
				D_RO(inode)->version, inode.oid.off);
		if (pmemobj_tx_stage() == TX_STAGE_WORK)
			pmemfile_tx_abort(EINVAL);
		else {
			errno = EINVAL;
			return NULL;
		}
	}

	os_rwlock_rdlock(&map->rwlock);
	size_t idx = inode_hash(map, inode) % map->sz;

	struct inode_map_bucket *b = &map->buckets[idx];
	struct pmemfile_vinode *vinode;
	for (unsigned j = 0; j < BUCKET_SIZE; ++j) {
		if (TOID_EQUALS(b->arr[j].pinode, inode)) {
			vinode = b->arr[j].vinode;
			goto end;
		}
	}
	os_rwlock_unlock(&map->rwlock);

	if (is_new) {
		rwlock_tx_wlock(&map->rwlock);
		tx = 1;
	} else
		os_rwlock_wrlock(&map->rwlock);

	/* recalculate slot, someone could rebuild the hash map */
	idx = inode_hash(map, inode) % map->sz;

	/* check again */
	b = &map->buckets[idx];
	unsigned empty_slot = UINT32_MAX;
	for (unsigned j = 0; j < BUCKET_SIZE; ++j) {
		if (TOID_EQUALS(b->arr[j].pinode, inode)) {
			vinode = b->arr[j].vinode;
			goto end;
		}
		if (empty_slot == UINT32_MAX && b->arr[j].pinode.oid.off == 0)
			empty_slot = j;
	}

	int tries = 0;
	while (empty_slot == UINT32_MAX) {
		size_t new_sz = map->sz;

		do {
			if (map->inodes > 2 * new_sz || tries == 2) {
				new_sz *= 2;
				tries = 0;
			} else {
				inode_map_rand_params(map);
				tries++;
			}
		} while (!inode_map_rebuild(map, new_sz));

		idx = inode_hash(map, inode) % map->sz;
		b = &map->buckets[idx];

		for (unsigned j = 0; j < BUCKET_SIZE; ++j) {
			if (b->arr[j].pinode.oid.off == 0) {
				empty_slot = j;
				break;
			}
		}
	}

	vinode = calloc(1, sizeof(*vinode));
	if (!vinode) {
		error = errno;
		goto end;
	}

	os_rwlock_init(&vinode->rwlock);
	vinode->tinode = inode;
	vinode->inode = D_RW(inode);
	if (inode_is_dir(vinode->inode) && parent) {
		vinode->parent = vinode_ref(pfp, parent);

		if (parent_refed)
			*parent_refed = true;
	}

	if (parent && name && namelen)
		vinode_set_debug_path_locked(pfp, parent, vinode, name,
				namelen);

	b->arr[empty_slot].pinode = inode;
	b->arr[empty_slot].vinode = vinode;
	map->inodes++;

	if (is_new)
		cb_push_front(TX_STAGE_ONABORT,
			(cb_basic)vinode_unregister_locked,
			vinode);

end:
	__sync_fetch_and_add(&vinode->ref, 1);
	if (is_new && tx)
		rwlock_tx_unlock_on_commit(&map->rwlock);
	else
		os_rwlock_unlock(&map->rwlock);

	if (error)
		errno = error;

	return vinode;
}

/*
 * inode_ref_new -- increases inode reference counter
 *
 * Assumes inode was allocated in the same transaction.
 * Return volatile inode.
 */
struct pmemfile_vinode *
inode_ref_new(PMEMfilepool *pfp,
		TOID(struct pmemfile_inode) inode,
		struct pmemfile_vinode *parent,
		volatile bool *parent_refed,
		const char *name,
		size_t namelen)
{
	return _inode_get(pfp, inode, true, parent, parent_refed, name,
			namelen);
}

/*
 * inode_ref -- increases inode reference counter
 *
 * Assumes inode was not allocated in the same transaction.
 * Return volatile inode.
 */
struct pmemfile_vinode *
inode_ref(PMEMfilepool *pfp,
		TOID(struct pmemfile_inode) inode,
		struct pmemfile_vinode *parent,
		volatile bool *parent_refed,
		const char *name,
		size_t namelen)
{
	return _inode_get(pfp, inode, false, parent, parent_refed, name,
			namelen);
}

/*
 * vinode_unref -- decreases inode reference counter
 *
 * Must be called in transaction.
 */
static bool
vinode_unref(PMEMfilepool *pfp, struct pmemfile_vinode *vinode)
{
	if (__sync_sub_and_fetch(&vinode->ref, 1) > 0)
		return false;

	if (vinode->inode->nlink == 0) {
		inode_array_unregister(pfp, vinode->orphaned.arr,
				vinode->orphaned.idx);

		inode_free(pfp, vinode->tinode);
	}

	return true;
}

/*
 * vinode_unref_tx -- decreases inode reference counter
 */
void
vinode_unref_tx(PMEMfilepool *pfp, struct pmemfile_vinode *vinode)
{
	ASSERTeq(pmemobj_tx_stage(), TX_STAGE_NONE);

	struct pmemfile_inode_map *c = pfp->inode_map;

	os_rwlock_wrlock(&c->rwlock);

	while (vinode) {
		struct pmemfile_vinode *to_unregister = NULL;
		TX_BEGIN_CB(pfp->pop, cb_queue, pfp) {
			struct pmemfile_vinode *parent = vinode->parent;

			if (vinode_unref(pfp, vinode))
				to_unregister = vinode;

			if (to_unregister && vinode != pfp->root)
				vinode = parent;
			else
				vinode = NULL;
		} TX_ONCOMMIT {
			if (to_unregister)
				vinode_unregister_locked(pfp, to_unregister);
		} TX_ONABORT {
			FATAL("!");
		} TX_END
	}

	os_rwlock_unlock(&c->rwlock);
}

/*
 * file_get_time -- sets *t to current time
 */
void
file_get_time(struct pmemfile_time *t)
{
	struct timespec tm;
	if (clock_gettime(CLOCK_REALTIME, &tm)) {
		ERR("!clock_gettime");
		pmemfile_tx_abort(errno);
	}
	t->sec = tm.tv_sec;
	t->nsec = tm.tv_nsec;
}

/*
 * inode_alloc -- allocates inode
 *
 * Must be called in transaction.
 */
struct pmemfile_vinode *
inode_alloc(PMEMfilepool *pfp, uint64_t flags, struct pmemfile_vinode *parent,
		volatile bool *parent_refed, const char *name, size_t namelen)
{
	LOG(LDBG, "flags 0x%lx", flags);

	ASSERTeq(pmemobj_tx_stage(), TX_STAGE_WORK);

	TOID(struct pmemfile_inode) tinode = TX_ZNEW(struct pmemfile_inode);
	struct pmemfile_inode *inode = D_RW(tinode);

	struct pmemfile_time t;
	file_get_time(&t);

	inode->version = PMEMFILE_INODE_VERSION(1);
	inode->flags = flags;
	inode->ctime = t;
	inode->mtime = t;
	inode->atime = t;
	inode->nlink = 0;
	os_rwlock_rdlock(&pfp->cred_rwlock);
	inode->uid = pfp->cred.euid;
	inode->gid = pfp->cred.egid;
	os_rwlock_unlock(&pfp->cred_rwlock);

	if (inode_is_regular_file(inode))
		inode->file_data.blocks.length =
				(sizeof(inode->file_data) -
				sizeof(inode->file_data.blocks)) /
				sizeof(struct pmemfile_block);
	else if (inode_is_dir(inode)) {
		inode->file_data.dir.num_elements =
				(sizeof(inode->file_data) -
				sizeof(inode->file_data.dir)) /
				sizeof(struct pmemfile_dirent);
		inode->size = sizeof(inode->file_data);
	}

	return inode_ref_new(pfp, tinode, parent, parent_refed, name, namelen);
}

/*
 * vinode_orphan -- register specified inode in orphaned_inodes array
 */
void
vinode_orphan(PMEMfilepool *pfp, struct pmemfile_vinode *vinode)
{
	LOG(LDBG, "inode 0x%" PRIx64 " path %s", vinode->tinode.oid.off,
			pmfi_path(vinode));

	ASSERTeq(vinode->orphaned.arr, NULL);

	rwlock_tx_wlock(&pfp->rwlock);

	TOID(struct pmemfile_inode_array) orphaned =
			pfp->super->orphaned_inodes;

	inode_array_add(pfp, orphaned, vinode,
			&vinode->orphaned.arr, &vinode->orphaned.idx);

	rwlock_tx_unlock_on_commit(&pfp->rwlock);
}

/*
 * dir_assert_no_dirents -- checks that directory has no entries
 */
static void
dir_assert_no_dirents(struct pmemfile_dir *dir)
{
	for (uint32_t i = 0; i < dir->num_elements; ++i)
		if (dir->dirents[i].inode.oid.off)
			FATAL("Trying to free non-empty directory");
}

/*
 * file_inode_free -- frees inode
 *
 * Must be called in transaction.
 */
void
inode_free(PMEMfilepool *pfp, TOID(struct pmemfile_inode) tinode)
{
	(void) pfp;

	LOG(LDBG, "inode 0x%" PRIx64, tinode.oid.off);

	struct pmemfile_inode *inode = D_RW(tinode);
	if (inode_is_dir(inode)) {
		struct pmemfile_dir *dir = &inode->file_data.dir;
		TOID(struct pmemfile_dir) tdir = TOID_NULL(struct pmemfile_dir);

		while (dir != NULL) {
			/* should have been catched earlier */
			dir_assert_no_dirents(dir);

			TOID(struct pmemfile_dir) next = dir->next;
			if (!TOID_IS_NULL(tdir))
				TX_FREE(tdir);
			tdir = next;
			dir = D_RW(tdir);
		}
	} else if (inode_is_regular_file(inode)) {
		struct pmemfile_block_array *arr = &inode->file_data.blocks;
		TOID(struct pmemfile_block_array) tarr =
				TOID_NULL(struct pmemfile_block_array);

		while (arr != NULL) {
			for (unsigned i = 0; i < arr->length; ++i)
				TX_FREE(arr->blocks[i].data);

			TOID(struct pmemfile_block_array) next = arr->next;
			if (!TOID_IS_NULL(tarr))
				TX_FREE(tarr);
			tarr = next;
			arr = D_RW(tarr);
		}
	} else if (inode_is_symlink(inode)) {
		/* nothing to be done */
	} else {
		FATAL("unknown inode type 0x%lx", inode->flags);
	}
	TX_FREE(tinode);
}

/*
 * pmemfile_time_to_timespec -- convert between pmemfile_time and timespec
 */
static inline struct timespec
pmemfile_time_to_timespec(const struct pmemfile_time *t)
{
	struct timespec tm;
	tm.tv_sec = t->sec;
	tm.tv_nsec = t->nsec;
	return tm;
}

/*
 * vinode_stat -- fill struct stat using information from vinode
 */
static int
vinode_stat(struct pmemfile_vinode *vinode, struct stat *buf)
{
	struct pmemfile_inode *inode = vinode->inode;

	memset(buf, 0, sizeof(*buf));
	buf->st_dev = vinode->tinode.oid.pool_uuid_lo;
	buf->st_ino = vinode->tinode.oid.off;
	buf->st_mode = inode->flags & (PMEMFILE_S_IFMT | PMEMFILE_ALLPERMS);
	buf->st_nlink = inode->nlink;
	buf->st_uid = inode->uid;
	buf->st_gid = inode->gid;
	buf->st_rdev = 0;
	if ((off_t)inode->size < 0)
		return EOVERFLOW;
	buf->st_size = (off_t)inode->size;
	buf->st_blksize = 1;
	if ((blkcnt_t)inode->size < 0)
		return EOVERFLOW;

	blkcnt_t blks = 0;
	if (inode_is_regular_file(inode)) {
		const struct pmemfile_block_array *arr =
				&inode->file_data.blocks;
		size_t sz = 0;
		while (arr) {
			for (uint32_t i = 0; i < arr->length; ++i)
				sz += arr->blocks[i].size;
			arr = D_RO(arr->next);
		}

		/*
		 * XXX This doesn't match reality. It will match once we start
		 * getting 4k-aligned blocks from pmemobj allocator.
		 */
		blks = (blkcnt_t)((sz + 511) / 512);
	} else if (inode_is_dir(inode)) {
		const struct pmemfile_dir *arr = &inode->file_data.dir;
		size_t sz = 0;
		while (arr) {
			sz += pmemfile_dir_size(arr->next);
			arr = D_RO(arr->next);
		}

		/*
		 * XXX This doesn't match reality. It will match once we start
		 * getting 4k-aligned blocks from pmemobj allocator.
		 */
		blks = (blkcnt_t)((sz + 511) / 512);
	} else if (inode_is_symlink(inode)) {
		blks = 0;
	} else
		ASSERT(0);
	buf->st_blocks = blks;
	buf->st_atim = pmemfile_time_to_timespec(&inode->atime);
	buf->st_ctim = pmemfile_time_to_timespec(&inode->ctime);
	buf->st_mtim = pmemfile_time_to_timespec(&inode->mtime);

	return 0;
}

static int
_pmemfile_fstatat(PMEMfilepool *pfp, struct pmemfile_vinode *dir,
		const char *path, struct stat *buf, int flags)
{
	int error = 0;
	struct pmemfile_cred cred;
	struct pmemfile_path_info info;
	struct pmemfile_vinode *vinode;

	LOG(LDBG, "path %s", path);

	if (!buf) {
		error = EFAULT;
		goto ret;
	}

	if (path[0] == 0 && (flags & PMEMFILE_AT_EMPTY_PATH)) {
		error = vinode_stat(dir, buf);
		goto ret;
	}

	if (flags & ~(PMEMFILE_AT_NO_AUTOMOUNT | PMEMFILE_AT_SYMLINK_NOFOLLOW |
			PMEMFILE_AT_EMPTY_PATH)) {
		error = EINVAL;
		goto ret;
	}

	if (get_cred(pfp, &cred)) {
		error = errno;
		goto ret;
	}

	vinode = resolve_pathat_full(pfp, &cred, dir, path, &info, 0,
				!(flags & PMEMFILE_AT_SYMLINK_NOFOLLOW));

	if (info.error) {
		error = info.error;
		goto end;
	}

	if (!vinode_is_dir(vinode) && strchr(info.remaining, '/')) {
		error = ENOTDIR;
		goto end;
	}

	error = vinode_stat(vinode, buf);

end:
	path_info_cleanup(pfp, &info);
	put_cred(&cred);

	if (vinode)
		vinode_unref_tx(pfp, vinode);
ret:
	if (error) {
		errno = error;
		return -1;
	}

	return 0;
}

int
pmemfile_fstatat(PMEMfilepool *pfp, PMEMfile *dir, const char *path,
		struct stat *buf, int flags)
{
	struct pmemfile_vinode *at;
	bool at_unref;

	if (!path) {
		errno = ENOENT;
		return -1;
	}

	at = pool_get_dir_for_path(pfp, dir, path, &at_unref);

	int ret = _pmemfile_fstatat(pfp, at, path, buf, flags);

	if (at_unref)
		vinode_cleanup(pfp, at, ret != 0);

	return ret;
}

/*
 * pmemfile_stat
 */
int
pmemfile_stat(PMEMfilepool *pfp, const char *path, struct stat *buf)
{
	return pmemfile_fstatat(pfp, PMEMFILE_AT_CWD, path, buf, 0);
}

/*
 * pmemfile_fstat
 */
int
pmemfile_fstat(PMEMfilepool *pfp, PMEMfile *file, struct stat *buf)
{
	if (!file) {
		errno = EBADF;
		return -1;
	}

	if (!buf) {
		errno = EFAULT;
		return -1;
	}

	int ret = vinode_stat(file->vinode, buf);

	if (ret) {
		errno = ret;
		return -1;
	}

	return 0;
}

/*
 * pmemfile_lstat
 */
int
pmemfile_lstat(PMEMfilepool *pfp, const char *path, struct stat *buf)
{
	return pmemfile_fstatat(pfp, PMEMFILE_AT_CWD, path, buf,
			PMEMFILE_AT_SYMLINK_NOFOLLOW);
}
