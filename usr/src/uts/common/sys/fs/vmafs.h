/*
 * CDDL HEADER START
 *
 * The contents of this file are subject to the terms of the
 * Common Development and Distribution License (the "License").
 * You may not use this file except in compliance with the License.
 *
 * You can obtain a copy of the license at usr/src/OPENSOLARIS.LICENSE
 * or http://www.opensolaris.org/os/licensing.
 * See the License for the specific language governing permissions
 * and limitations under the License.
 *
 * When distributing Covered Code, include this CDDL HEADER in each
 * file and include the License file at usr/src/OPENSOLARIS.LICENSE.
 * If applicable, add the following below this CDDL HEADER, with the
 * fields enclosed by brackets "[]" replaced with your own identifying
 * information: Portions Copyright [yyyy] [name of copyright owner]
 *
 * CDDL HEADER END
 */
/*
 * Copyright 2009 Sun Microsystems, Inc.  All rights reserved.
 * Use is subject to license terms.
 */

/*
 * Virtual Machine Aware FileSystem
 */

#ifndef _SYS_FS_VMAWARE_H
#define	_SYS_FS_VMAWARE_H

#include <sys/sysmacros.h>
#include <sys/avl.h>

#ifdef _KERNEL
#include <sys/vfs_opreg.h>
#include <sys/time.h>
#include <sys/door.h>
#else
#include <time.h>
#include <sys/vnode.h>
#include <sys/vfs.h>
#include <door.h>
/*
 * door_handle_t is not defined outside of _KERNEL. To let
 * this header compile in userland, we must give a definition for it,
 * preferably as an opaque equivalent to the real type.
 * 
 * door_handle_t is a pointer to a __door_handle struct, so the
 * best fit opaque type is a (void *).
 */
typedef void *door_handle_t;
#endif

#ifdef	__cplusplus
extern "C" {
#endif

/************************************************
 * Token bucket for rate-limiting IOPS on a VFS.
 ************************************************/
 
typedef struct {
	/*
	 * Deadline timestamp and token count:
	 * These are packed in a single 64-bit dword, to
	 * allow atomic updates:
	 * - Lower 48 bits: number of nanosecs between the epoch and
	 *   the time the last I/O operation is scheduled to finish.
	 * - Higher 16 bits: number of tokens when that happens.
	 */ 
	volatile uint64_t vma_bucket;
	/* Time when the struct was created (nsec): */
	volatile uint64_t vma_epoch;
	/*
	 * Control word of the token bucket: this word also
	 * packs two values, delay between tokens and burst size,
	 * so they can be updated at once.
	 * 
	 * - Lower 32 bits: number of nanoseconds between tokens.
	 * - Higher 32 bits: maximum burst size.
	 */
	uint64_t vma_control;
	/* maximum allowed delay (nanosecs) */
	uint64_t vma_timeout;
} vma_tbucket_t;

/* Number of bits of the step size part of the control field */
#define VMA_TBUCKET_STEPBITS	32
#define VMA_TBUCKET_STEPMASK	((1ll << VMA_TBUCKET_STEPBITS) - 1)
/* Step size (nanosecs)  */
#define VMA_TBUCKET_STEP(c)	((c) & VMA_TBUCKET_STEPMASK)
/* Burst size */
#define VMA_TBUCKET_BURST(c)	((c) >> VMA_TBUCKET_STEPBITS)

/* Recompose the control word from 16-bit rate and burst */
inline uint64_t vma_tbucket_control(uint64_t rate, uint64_t burst)
{
	return ((burst << VMA_TBUCKET_STEPBITS) |
		((1000000000ul / rate) & VMA_TBUCKET_STEPMASK));
}

/* Number of bits in the timestamp part of the bucket field */
#define	VMA_TBUCKET_STAMPBITS	48
#define VMA_TBUCKET_STAMPMASK	((1ll << VMA_TBUCKET_STAMPBITS) - 1)
/* Deadline */
#define VMA_TBUCKET_STAMP(b)	((b) & VMA_TBUCKET_STAMPMASK)
/* Tokens available */
#define VMA_TBUCKET_TOKENS(b)	((b) >> VMA_TBUCKET_STAMPBITS)

/* Recombines the deadline from 48-bit stamp and 16-bit tokens */
inline uint64_t vma_tbucket_deadline(uint64_t stamp, uint64_t tokens)
{
	return ((tokens << VMA_TBUCKET_STAMPBITS) |
		(stamp & VMA_TBUCKET_STAMPMASK));
}

/* Default timeout for token bucket (5 secs = 5*10^9 nanosecs) */
#define	VMA_TBUCKET_TIMEOUT	5000000000ll
/* Special token value to signal the packet should be dropped */
#define VMA_TBUCKET_DROP	(~(0ll))

/* Initialize the token bucket */
void vma_tbucket_init(vma_tbucket_t *tb, uint16_t rate, uint16_t burst, uint64_t timeout);

/* Updates the token bucket rate, returns the burst size. */
inline void vma_tbucket_setrate(vma_tbucket_t *tb,
	uint64_t rate, uint64_t burst)
{
	if (rate > burst) burst = rate;
	tb->vma_control = (rate == 0) ? 0 : vma_tbucket_control(rate, burst);
}

/* Updates the token bucket timeout. */
inline void vma_tbucket_settimeout(vma_tbucket_t *tb, uint64_t timeout)
{
	tb->vma_timeout = (timeout > 0) ? timeout : VMA_TBUCKET_TIMEOUT;
}

/* Force get token, call only if rate != 0 */
uint64_t vma_do_get_token(vma_tbucket_t *tb);

/*
 * Get a token. Returns the nanosecs to delay I/O operation, or 
 * VMA_TBUCKET_DROP if delay exceeds timeout.
 */
inline uint64_t vma_get_token(vma_tbucket_t *tb)
{
	return ((tb->vma_control == 0) ? 0 : vma_do_get_token(tb));
}

/*********************
 * Per-VFS statistics
 *********************/
 
typedef struct {
	/*
	 * Latency added by I/O shaping and disk, microsecs:
	 * - Higher 32 bits are disk latency.
	 * - Lower 32 bits are I/O shaping latency.
	 */
	volatile uint64_t vma_latency;
	/* threads delayed because of I/O shaping: */
	volatile uint32_t vma_delays;
	/* drops due to I/O shaping: */
	volatile uint32_t vma_drops;
} vma_stats_t;

/* Number of bits of each latency sub-field */
#define VMA_STATS_LATBITS	32
#define VMA_STATS_LATMASK	((1ll << VMA_STATS_LATBITS) - 1)
/* I/O Shaping latency */
#define	VMA_STATS_SHAPING(lat)	((lat) & VMA_STATS_LATMASK)
/* Disk latency */
#define	VMA_STATS_DISK(lat)	((lat) >> VMA_STATS_LATBITS)

/* Recombine 32-bits shaping and disk latency into a 64-bit word */
inline uint64_t vma_stats_latency(uint64_t shape, uint64_t disk)
{
	return ((shape & VMA_STATS_LATMASK) |
		(disk << VMA_STATS_LATBITS));
}

/*
 * Update stats.
 * 
 * Drop and delay stats are simple counters. Latency stats are
 * averaged over time. A weighted average algorithm is used, were
 * the last average and the new value are combined, giving each factor
 * a different weight:
 * 
 * - The old average is multiplied by (2^32 - vma_weight)
 * - The new value is multiplied by (vma_weight)
 * - Both values are added and divided by (2^32)
 * 
 * vma_weight can range from 0 to 2^32-1.
 * 
 * - if vma_weight == 2^31, both values have the same weight
 *   in the resulting average.
 * - If vma_weight < 2^31, the old average is given priority,
 *   favoring long-term stability over outliers.
 * - If vma_weight > 2^31, the new value is given priority,
 *   making the average respond more quickly to changes in the
 *   instantaneous values of latency.
 * 
 * Weight should be used consistently in all calls to update.
 */
void vma_stats_update(vma_stats_t *sp, uint32_t weight,
	uint64_t token, uint64_t disk);

/* maximum number of bits of the weighting factor */
#define VMA_STATS_WEIGHTBITS	32
#define VMA_STATS_WEIGHTMASK	((1ll << VMA_STATS_WEIGHTBITS) - 1)
#define VMA_STATS_WEIGHT20	((VMA_STATS_WEIGHTMASK * 20) / 100)

typedef struct {
	vma_stats_t vma_stats[2]; /* read and write stats */
	uint32_t vma_weight;
} vma_rwstats_t;

/* values for "op" flag in vma_rwstats_update and vma_rwstats_op */
#define	VMA_STATS_READ	0
#define VMA_STATS_WRITE	1

/* Retrieves the read or write stat fields */
#define	VMA_RWSTATS_OP(sp, op)	(&((sp)->vma_stats[(op)]))
#define	VMA_RWSTATS_WRITE(sp)	VMA_RWSTATS_OP((sp), VMA_STATS_WRITE)
#define	VMA_RWSTATS_READ(sp)	VMA_RWSTATS_OP((sp), VMA_STATS_READ)

/* Initializes the stats object */
void vma_rwstats_init(vma_rwstats_t *sp, uint32_t weight);

/* Updates the rwstats object */
inline void vma_rwstats_update(vma_rwstats_t *sp, int op,
	uint64_t token, uint64_t disk)
{
	return vma_stats_update(VMA_RWSTATS_OP(sp, op),
		sp->vma_weight, token, disk);
}

/***************************************************************
 * VMA FileSystem object
 * 
 * These objects represent the vm-aware filesystem, and enforce
 * settings and policy over the underlying real filesystems.
 ***************************************************************/

#define	vmafs_node_t	avl_node_t
#define	vmafs_index_t	avl_tree_t
#define vmafs_sync_t	krwlock_t

typedef struct vmafs {
	vmafs_node_t	 vma_link;	/* Link to index by path */
	vnode_t		*vma_realvp;	/* real root vp */
	vfs_t		*vma_realvfs;	/* real vfs */
	vma_tbucket_t	 vma_bucket;	/* token bucket */
	vma_rwstats_t	 vma_stats;	/* filesystem stats */
	vfs_t		 vma_vfs;	/* new loopback vfs */
} vmafs_t;

/* inheritable mount flags - propagated from real vfs to loopback */
#define	INHERIT_VFS_FLAG	\
	(VFS_RDONLY|VFS_NOSETUID|VFS_NODEVICES|VFS_XATTR|VFS_NBMAND|VFS_NOEXEC)

/*
 * IOCTL commands
 */
#define	VMAFS_IOC_SETDOOR	0xFFFF

/*
 * The vmanode is the "inode" for loop-back files.  It contains
 * all the information necessary to handle loop-back files on the
 * client side.
 */
typedef struct vmanode {
	vmafs_node_t	 vma_link;
	vnode_t		*vma_realvp;	/* real vnode pointer */
	vnode_t		*vma_vnode;	/* place holder vnode for file */
} vmanode_t;

/* vmanode <-> vnode manipulation */
#define	vton(vp)	((vmanode_t *)((vp)->v_data))
#define	ntov(np)	((np)->vma_vnode)
#define	realvp(vp)	(vton(vp)->vma_realvp)

/* Mount information for a vm-aware filesystem */
typedef struct vmainfo {
	vfs_t		*vma_realvfs;	/* real vfs of mount */
	vfs_t		*vma_mountvfs;	/* loopback vfs */
	vnode_t		*vma_rootvp;	/* root vnode of this vfs */
	vmafs_index_t	*vma_vnode;	/* index of vmanodes by vnode ptr */
	vmafs_index_t	*vma_path;	/* index of vmafs by vfs path */
	vmafs_sync_t	 vma_sync;	/* Sync. primitive protecting the indexes */
	kmutex_t	 vma_doorlock;  /* protect the door */
	door_handle_t	 vma_door;	/* door handle */
	unsigned int	 vma_refct;	/* # outstanding vnodes */
	int		 vma_mflag;	/* mount flags to inherit */
	int		 vma_dflag;	/* mount flags to not inherit */
	int		 vma_flag;	/* filesystem behavior flags */
} vmainfo_t;

#define	vtoli(vp)	((vmainfo_t *)((vp)->v_vfsp->v_data))

/* fileid (fid_t) sub-structure */
typedef struct vmafid {
	ushort_t len;
	fsid_t lf_fsid;
	fid_t  lf_fid;
} vmafid_t;

#define	VMAFIDSIZE	offsetof(vmafid_t, lf_fid.fid_data)

extern vfs_t *vma_realvfs(vfs_t *, vnode_t **);

extern void vmafs_subrinit(void);
extern void vmafs_subrfini(void);

extern vnode_t *vmanode_make(vnode_t *, vmainfo_t *, int);
extern void vmanode_free(vmanode_t *);

extern void vmainfo_setup(vmainfo_t *, uint_t);
extern void vmainfo_destroy(vmainfo_t *);

extern int vma_dispatch_mkdir(vmainfo_t *, vnode_t *, char *, cred_t *);
extern int vma_open_door(vmainfo_t *, int);

#ifdef _KERNEL

extern const struct fs_operation_def vma_vnodeops_template[];

extern struct vnodeops *vma_vnodeops;
extern vfsops_t *vma_vfsops;
extern struct mod_ops mod_fsops;

#endif /* _KERNEL */

#ifdef	__cplusplus
}
#endif

#endif	/* _SYS_FS_VMAWARE_H */
