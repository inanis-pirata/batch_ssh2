#ifndef BS2_POOL_H
#define BS2_POOL_H

/* Run fn(ctx, idx) for idx in [0, count) using nthreads worker threads.
 * Blocks until every task is done. 0 = ok, -1 = thread creation failed
 * (remaining tasks are still executed on the caller thread). */
int bs2_pool_run(void (*fn)(void *ctx, int idx), void *ctx,
                 int count, int nthreads);

#endif
