#include "pool.h"

#include <stdlib.h>
#include <signal.h>

#if defined(_WIN32)
#  include <windows.h>
typedef HANDLE             bs2_thread_t;
typedef CRITICAL_SECTION   bs2_mutex_t;
#else
#  include <pthread.h>
typedef pthread_t          bs2_thread_t;
typedef pthread_mutex_t    bs2_mutex_t;
#endif

typedef struct {
    void (*fn)(void *ctx, int idx);
    void  *ctx;
    int    count;
    int    next;          /* next task index, guarded by mutex */
    bs2_mutex_t mu;
} bs2_pool;

static void mu_init(bs2_mutex_t *m) {
#if defined(_WIN32)
    InitializeCriticalSection(m);
#else
    pthread_mutex_init(m, NULL);
#endif
}

static void mu_lock(bs2_mutex_t *m) {
#if defined(_WIN32)
    EnterCriticalSection(m);
#else
    pthread_mutex_lock(m);
#endif
}

static void mu_unlock(bs2_mutex_t *m) {
#if defined(_WIN32)
    LeaveCriticalSection(m);
#else
    pthread_mutex_unlock(m);
#endif
}

static int next_task(bs2_pool *p) {
    mu_lock(&p->mu);
    int idx = p->next < p->count ? p->next++ : -1;
    mu_unlock(&p->mu);
    return idx;
}

#if defined(_WIN32)
static DWORD WINAPI worker(LPVOID arg) {
#else
static void *worker(void *arg) {
#endif
    bs2_pool *p = (bs2_pool *)arg;
    for (;;) {
        int idx = next_task(p);
        if (idx < 0) break;
        p->fn(p->ctx, idx);
    }
#if defined(_WIN32)
    return 0;
#else
    return NULL;
#endif
}

int bs2_pool_run(void (*fn)(void *ctx, int idx), void *ctx,
                 int count, int nthreads) {
    if (count <= 0) return 0;
    if (nthreads < 1) nthreads = 1;
    if (nthreads > count) nthreads = count;

    bs2_pool p;
    p.fn = fn;
    p.ctx = ctx;
    p.count = count;
    p.next = 0;
    mu_init(&p.mu);

    bs2_thread_t *tids = (bs2_thread_t *)calloc((size_t)nthreads, sizeof(bs2_thread_t));
    int *ok = (int *)calloc((size_t)nthreads, sizeof(int));
    if (!tids || !ok) { free(tids); free(ok); return -1; }

    int started = 0;
    for (int i = 0; i < nthreads; i++) {
#if defined(_WIN32)
        tids[i] = CreateThread(NULL, 0, worker, &p, 0, NULL);
        if (tids[i] != NULL) { ok[i] = 1; started++; }
#else
        if (pthread_create(&tids[i], NULL, worker, &p) == 0) { ok[i] = 1; started++; }
#endif
    }

    /* help out on the caller thread too (also covers started == 0) */
    worker(&p);

    for (int i = 0; i < nthreads; i++) {
#if defined(_WIN32)
        if (ok[i]) {
            WaitForSingleObject(tids[i], INFINITE);
            CloseHandle(tids[i]);
        }
#else
        if (ok[i]) pthread_join(tids[i], NULL);
#endif
    }
    free(ok);
    free(tids);
    return started > 0 ? 0 : -1;
}
