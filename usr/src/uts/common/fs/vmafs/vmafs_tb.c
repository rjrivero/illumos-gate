#ifdef _KERNEL
#include <sys/sysmacros.h>
#include <sys/atomic.h>
#include <sys/random.h>
#include <sys/atomic.h>
#include <sys/time.h>
#else
#include <time.h>
#include <atomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#endif
#include <sys/fs/vmafs.h>

#ifdef _KERNEL
#define	vma_random(wait)	random_get_pseudo_bytes((uint8_t *)&(wait), sizeof((wait)))
#else 
#define vma_random(wait)	(wait) = random()
#define	gethrtime_waitfree	gethrtime
#endif

/************************************************
 * Token bucket for rate-limiting IOPS on a VFS.
 ************************************************/

/*
 * Initialize a token bucket.
 * - tb: the token bucket.
 * - rate: the rate at which tokens must be added to the bucket
 *   (tokens per second)
 * - burst: the maximum burst size (tokens).
 */
uint64_t vma_tbucket_init(vma_tbucket_t *tb, uint16_t rate, uint16_t burst, uint64_t timeout)
{
	/*
	 * The bucket starts full, and with stamp == 1
	 * (stamp == 0 is reserved as a flag for updating epoch)
	 */
	tb->vma_epoch   = gethrtime();
	tb->vma_timeout = (timeout != 0) ? timeout : VMA_TBUCKET_TIMEOUT;
	tb->vma_bucket  = VMA_TBUCKET_DEADLINE(1, burst);
	tb->vma_control = VMA_TBUCKET_CONTROL(rate, burst);
	return tb->vma_bucket;
}

/*
 * Acquires a token.
 *
 * This implementation of the token bucket algorithm uses a "deadline"
 * variant: instead of keeping track of the current number of tokens
 * at each point in time, and making threads wait until new tokens
 * arrive, it calculates the "virtual time" an I/O operation is
 * scheduled to finish.
 * 
 * So, when a thread tries to acquire a token, we check the current
 * time and the "virtual end time" of the last operation that acquired
 * a token:
 * 
 * - If the virtual end time is in the past, we calculate how many
 *   more tokens have arrived since them, take one for ourselves, and
 *   set the virtual end time for the current operation to now.
 * 
 * - If the virtual end time is in the future, we check if there will
 *   be tokens available at that time. If yes, that is our virtual end
 *   time too. Otherwise, our virtual end time is one step further.
 * 
 * This function returns the number of microseconds until our
 * scheduled virtual end time, or (-1) if delay is larger than
 * 4 seconds.
 */
uint64_t vma_get_token(vma_tbucket_t *tb)
{
	uint64_t vma_control, vma_epoch, vma_bucket;
	uint64_t winner;
	uint64_t step, burst, stamp, tokens;
	uint64_t deadline, lapse, now;

retry:
	now         = gethrtime_waitfree();
	/* Get the virtual end time of the last operation */
	vma_bucket  = tb->vma_bucket;
	vma_control = tb->vma_control;
	/* Make sure we read the bucket before the epoch */
	membar_consumer();
	vma_epoch   = tb->vma_epoch;
	/* Extract the packed values */
	stamp       = VMA_TBUCKET_STAMP(vma_bucket);
	tokens      = VMA_TBUCKET_TOKENS(vma_bucket);
	step        = VMA_TBUCKET_STEP(vma_control);
	burst       = VMA_TBUCKET_BURST(vma_control);
	if (now < vma_epoch) {
		/*
		 * Epoch in the future? start a race to update it.
		 * 
		 * Use CAS instead of unconditional write, in case
		 * some other thread has already realized and started
		 * the race.
		 */
		atomic_cas_64(&(tb->vma_bucket), vma_bucket, 0);
		goto retry;
	}
	/*
	 * If stamp == 0, we are trying to update the epoch!
	 * let's race to change it!
	 */
	if(stamp == 0) {
		/*
		 * We have to change both the bucket and the epoch
		 * in a way so as not to cause deadlocks, I/O errors,
		 * or slowdowns.
		 * 
		 * We must CAS one of the values, and then, if
		 * succeeded, CAS or change the other. This is a
		 * concurrent operation, so several things can happen.
		 * Let's analyze what is the proper order of CAS,
		 * stores, and loads:
		 * 
		 * - Assume we write the bucket first, then the epoch.
		 * 
		 *   If a concurrent thread enters in between and reads
		 *   the new bucket and an old epoch, it may update the
		 *   bucket with a large lapse (since the old epoch may
		 *   be far away in the past), so any following thread
		 *   will see an updated epoch and a large deadline
		 *   => stuck I/O.
		 * 
		 * - Assume we write the epoch first, then the bucket.
		 * 
		 *   If a concurrent thread sees the new epoch and
		 *   the old bucket, it will enter this codepath. In
		 *   the meantime, the current thread may have
		 *   successfully changed the bucket, and new threads
		 *   may have started to push the deadline forward.
		 * 
		 *   Now the concurrent thread tries to CAS epoch and,
		 *   since it has read the new value, it succeeds.
		 *   If it tries to CAS the bucket now, it will
		 *   fail, and the bucket may be left with a recent
		 *   epoch and a deadline larger than it should be =>
		 *   delayed I/O (probably not stalled since the
		 *   difference in epoch should be low, but however...)
		 * 
		 *   On the other hand, if the new thread overwrites
		 *   the bucket inconditionally instead of CASing it,
		 *   the bucket will be reset again and new threads
		 *   may schedule themselves earlier than they should.
		 * 
		 *   Both situations are wrong, but we prefer to
		 *   fail on the side of service continuity. So if
		 *   several threads enter the reset code path and
		 *   suceed in changing the epoch, they will
		 *   inconditionally reset the bucket too. It may cause
		 *   some peaks of traffic, but that's better than
		 *   a delay we cannot estimate.
		 * 
		 *   In any case, we must force memory barriers. If 
		 *   stores/loads are reordered and a thread, which has
		 *   already read the old epoch, now reads the new
		 *   bucket, it will not enter this codepath and may
		 *   try to change the bucket and succeed. As stated
		 *   before, since that thread is using an old epoch,
		 *   the deadline it sets will be large, and any
		 *   following thread will find an updated epoch and
		 *   a large deadline => Stuck I/O
		 * 
		 * So, the only acceptable course of action is:
		 * 
		 * - Modify epoch first with CAS.
		 * - If success, reset bucket inconditionally.
		 * - Order stores and loads so that bucket is read
		 *   before epoch, to avoid the case where a thread
		 *   sees the new bucket before the new epoch.
		 */
		winner = atomic_cas_64(&(tb->vma_epoch), vma_epoch, now);
		if (winner == vma_epoch) {
			membar_producer();
			tb->vma_bucket = VMA_TBUCKET_DEADLINE(1, burst);
		}
		goto retry;
	}
	/* If stamp is past, update tokens and bring stamp forward */
	lapse = now - vma_epoch + 1; /* add 1 to make sure it is not 0 */
	if (lapse > stamp) {
		tokens += ((lapse - stamp) / step);
		if (tokens > burst)
			tokens = burst;
		stamp = lapse;
	}
	/*
	 * If there are tokens free at deadline, take one.
	 * Otherwise, push the deadline back until next token arrives.
	 */
	if(tokens > 0)
		tokens--;
	else {
		stamp = ((stamp / step) + 1) * step;
	}
	/*
	 * Before updating the deadline, let's check the new stamp
	 * does not overflow the bits reserved for it in the bucket.
	 */
	if ((stamp & (~VMA_TBUCKET_STAMPMASK)) != 0) {
		/*
		 * There is overflow! start a race to update
		 * the epoch.
		 */
		atomic_cas_64(&(tb->vma_bucket), vma_bucket, 0);
		goto retry;
	}
	/* One last check: make sure delay is lower than maximum */
	lapse = (stamp > lapse) ? (stamp - lapse) : 0;
	if (lapse > tb->vma_timeout) {
		return VMA_TBUCKET_DROP;
	}
	/*
	 * Stamp does not overflow, lapse is lower than maximum,
	 * and we have updated the token count: Safe to update now.
	 */
	winner = atomic_cas_64(&(tb->vma_bucket), vma_bucket,
		VMA_TBUCKET_DEADLINE(stamp, tokens));
	if (winner != vma_bucket) {
		goto retry;
	}
	/* return the number of microseconds to wait */
	return lapse;
}

/*********************
 * Per-VFS statistics
 *********************/

void
vma_rwstats_init(vma_rwstats_t *sp, uint32_t weight)
{
	memset(sp->vma_stats, 0, sizeof(sp->vma_stats));
	sp->vma_weight = weight & VMA_STATS_WEIGHTMASK;
}

void
vma_stats_update(vma_stats_t *sp, uint32_t weight,
	uint64_t token, uint32_t disk)
{
	/* update drops and delays */
	if(token == VMA_TBUCKET_DROP)	atomic_inc_32(&(sp->vma_drops));
	if(token != 0)			atomic_inc_32(&(sp->vma_delays));
	/* This macro calculates the weighted average for
	 * a series of uint32_t latency values. */
#define	AVERAGE_LATENCY(weight, avg, lat)		\
	((uint32_t) ((					\
		(((uint64_t) (avg)) * ((weight) ^ VMA_STATS_WEIGHTMASK)) + \
		(((uint64_t) (lat)) * ((weight)))	\
	) >> VMA_STATS_WEIGHTBITS))
	/* update latencies */
	uint64_t orig, winner = sp->vma_latency;
	do {
		orig = winner;
		/* 
		 * Tokens are nsec, latency average is usec...
		 * roughly convert nsec to usec by dividing by 1024
		 */
		uint32_t snew = AVERAGE_LATENCY(weight, VMA_STATS_SHAPING(orig), (token >> 10));
		uint32_t dnew = AVERAGE_LATENCY(weight,	VMA_STATS_DISK(orig), disk);
		uint64_t lnew = VMA_STATS_LATENCY(snew, dnew);
		winner = atomic_cas_64(&(sp->vma_latency), orig, lnew);
	}
	while(orig != winner);
}

#ifndef _KERNEL
int main(int argc, char *argv)
{
	vma_tbucket_t bucket;
	vma_rwstats_t stats;
	vma_tbucket_init(&bucket, 10, 15, 0);
	vma_rwstats_init(&stats, VMA_STATS_WEIGHT20);
	int i;
	struct timespec tspec;
	printf("BUCKET STEP: %d\n", VMA_TBUCKET_STEP(bucket.vma_control));

	for(i = 0; i < 30; i++) {
		hrtime_t start = gethrtime();
		uint64_t token = vma_get_token(&bucket);
		hrtime_t stop = gethrtime();
		if (token > 0 && token != VMA_TBUCKET_DROP && (i&0x01)) {
			tspec.tv_sec = token / 1000000000l;
			tspec.tv_nsec = token % 1000000000l;
			nanosleep(&tspec, NULL);
		}
		vma_rwstats_update(&stats, i&0x01, token, 0);
		printf("\nTIEMPO DE ESPERA LAPSO %d (%llu): %lld\n", i, token, stop-start);
		printf("DEADLINE: %lld, TOKENS: %lld\n", VMA_TBUCKET_STAMP(bucket.vma_bucket), VMA_TBUCKET_TOKENS(bucket.vma_bucket));
	}

	tspec.tv_sec  = 1;
	tspec.tv_nsec = 0;
	nanosleep(&tspec, NULL);

	vma_tbucket_reset(&bucket, 5, 10);
	for(i = 0; i < 30; i++) {
		hrtime_t start = gethrtime();
		uint64_t token = vma_get_token(&bucket);
		hrtime_t stop = gethrtime();
		if (token > 0 && token != VMA_TBUCKET_DROP && (i&0x01)) {
			tspec.tv_sec = token / 1000000000l;
			tspec.tv_nsec = token % 1000000000l;
			nanosleep(&tspec, NULL);
		}
		vma_rwstats_update(&stats, i&0x01, token, 0);
		printf("\nTIEMPO DE ESPERA LAPSO %d (%llu): %lld\n", i, token, stop-start);
		printf("DEADLINE: %lld, TOKENS: %lld\n", VMA_TBUCKET_STAMP(bucket.vma_bucket), VMA_TBUCKET_TOKENS(bucket.vma_bucket));
	}
	printf("STATS: avg read %u, avg write %u\n", VMA_STATS_SHAPING(VMA_RWSTATS_READ(&stats)->vma_latency), VMA_STATS_SHAPING(VMA_RWSTATS_WRITE(&stats)->vma_latency));
	printf("STATS: read drops %u, write drops %u\n", VMA_RWSTATS_READ(&stats)->vma_drops, VMA_RWSTATS_WRITE(&stats)->vma_drops);
	printf("STATS: read delays %u, write delays %u\n", VMA_RWSTATS_READ(&stats)->vma_delays, VMA_RWSTATS_WRITE(&stats)->vma_delays);
	return 0;
}
#endif /* _KERNEL */
