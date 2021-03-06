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
#ifndef PMEMFILE_LAYOUT_H
#define PMEMFILE_LAYOUT_H

/*
 * On-media structures.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#ifdef __clang_analyzer__
/*
 * When clang analyzer sees pmemobj_direct can return NULL, it warns about NULL
 * not being handled at D_RW/D_RO call sites, even when it's obvious it's
 * not possible. Just hide its definition to quiet those warnings.
 */
#define PMEMOBJ_DIRECT_NON_INLINE
#endif

#include "libpmemobj.h"

POBJ_LAYOUT_BEGIN(pmemfile);
POBJ_LAYOUT_ROOT(pmemfile, struct pmemfile_super);
POBJ_LAYOUT_TOID(pmemfile, struct pmemfile_inode);
POBJ_LAYOUT_TOID(pmemfile, struct pmemfile_dir);
POBJ_LAYOUT_TOID(pmemfile, struct pmemfile_block_array);
POBJ_LAYOUT_TOID(pmemfile, struct pmemfile_block);
POBJ_LAYOUT_TOID(pmemfile, struct pmemfile_inode_array);
POBJ_LAYOUT_TOID(pmemfile, char);
POBJ_LAYOUT_END(pmemfile);

struct pmemfile_block {
	/* block data pointer */
	TOID(char) data;

	/* usable size of the block */
	uint32_t size;

	/* additional information about block */
	uint32_t flags;

	/* offset in file */
	uint64_t offset;

	/* next block, with offset bigger than offset+size */
	TOID(struct pmemfile_block) next;

	/* previous block, with smaller offset */
	TOID(struct pmemfile_block) prev;
};

#define BLOCK_INITIALIZED 1

/* single block array */
struct pmemfile_block_array {
	/* next block array */
	TOID(struct pmemfile_block_array) next;

	/* number of entries in "blocks" */
	uint32_t length;

	/* padding / unused */
	uint32_t padding;

	/* blocks */
	struct pmemfile_block blocks[];
};

#define PMEMFILE_MAX_FILE_NAME 255
/* directory entry */
struct pmemfile_dirent {
	/* inode */
	TOID(struct pmemfile_inode) inode;

	/* name */
	char name[PMEMFILE_MAX_FILE_NAME + 1];
};

/* Directory */
struct pmemfile_dir {
	/* number of entries in "dirents" */
	uint32_t num_elements;

	/* padding / unused */
	uint32_t padding;

	/* next batch of entries */
	TOID(struct pmemfile_dir) next;

	/* directory entries */
	struct pmemfile_dirent dirents[];
};

struct pmemfile_time {
	/* seconds */
	int64_t sec;

	/* nanoseconds */
	int64_t nsec;
};

#define PMEMFILE_INODE_VERSION(a) ((uint32_t)0x00444E49 | \
		((uint32_t)(a + '0') << 24))

#define PMEMFILE_IN_INODE_STORAGE (4096\
				- 4  /* version */ \
				- 4  /* uid */ \
				- 4  /* gid */ \
				- 4  /* reserved */ \
				- 16 /* atime */ \
				- 16 /* ctime */ \
				- 16 /* mtime */ \
				- 8  /* nlink */ \
				- 8  /* size */ \
				- 8  /* flags */)

/* Inode */
struct pmemfile_inode {
	/* layout version */
	uint32_t version;

	/* owner */
	uint32_t uid;

	/* group */
	uint32_t gid;

	/* unused / padding */
	uint32_t reserved;

	/* time of last access */
	struct pmemfile_time atime;

	/* time of last status change */
	struct pmemfile_time ctime;

	/* time of last modification */
	struct pmemfile_time mtime;

	/* hard link counter */
	uint64_t nlink;

	/* size of file */
	uint64_t size;

	/* file flags */
	uint64_t flags;

	/* data! */
	union {
		/* file specific data */
		struct pmemfile_block_array blocks;

		/* directory specific data */
		struct pmemfile_dir dir;

		char data[PMEMFILE_IN_INODE_STORAGE];
	} file_data;
};

#define NUMINODES_PER_ENTRY 249

struct pmemfile_inode_array {
	PMEMmutex mtx;
	TOID(struct pmemfile_inode_array) prev;
	TOID(struct pmemfile_inode_array) next;

	/* number of used entries, <0, NUMINODES_PER_ENTRY> */
	uint32_t used;

	char padding[12];

	TOID(struct pmemfile_inode) inodes[NUMINODES_PER_ENTRY];
};

#define PMEMFILE_SUPER_VERSION(a, b) ((uint64_t)0x000056454C494650 | \
		((uint64_t)(a + '0') << 48) | ((uint64_t)(b + '0') << 56))

/* superblock */
struct pmemfile_super {
	/* superblock version */
	uint64_t version;

	/* root directory inode */
	TOID(struct pmemfile_inode) root_inode;

	/* list of arrays of inodes that were deleted, but are still opened */
	TOID(struct pmemfile_inode_array) orphaned_inodes;

	char padding[4096
			- 8  /* version */
			- 16 /* toid */
			- 16 /* toid */];
};

#endif
