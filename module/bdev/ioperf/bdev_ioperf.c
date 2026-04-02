/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2024 Intel Corporation.
 *   All rights reserved.
 */

#include "spdk/stdinc.h"

#include "spdk/bdev.h"
#include "spdk/env.h"
#include "spdk/thread.h"
#include "spdk/json.h"
#include "spdk/string.h"
#include "spdk/likely.h"
#include "spdk/barrier.h"

#include "spdk/bdev_module.h"
#include "spdk/log.h"

#include "bdev_ioperf.h"

/* RPC handlers are in bdev_ioperf_rpc.c */
extern int bdev_ioperf_rpc_init(void);

/* Global ioperf bdev list */
static TAILQ_HEAD(ioperf_bdev_head, ioperf_bdev) g_ioperf_bdev_head = TAILQ_HEAD_INITIALIZER(g_ioperf_bdev_head);
struct ioperf_bdev *g_ioperf_bdev;
struct ioperf_thread_mgr g_ioperf_thread_mgr;

/* Getter function for bdev_ioperf_rpc.c */
struct ioperf_bdev *
ioperf_get_bdev_head(void)
{
    return TAILQ_FIRST(&g_ioperf_bdev_head);
}

#define MAX_QUEUED_IO 1024

static int bdev_ioperf_initialize(void);
static void bdev_ioperf_finish(void);
static int bdev_ioperf_config_json(struct spdk_json_write_ctx *w);
static void bdev_ioperf_write_config_json(struct spdk_bdev *bdev, struct spdk_json_write_ctx *w);

/* Forward declarations */
static void fill_all_fields(struct ioperf_io_ctx *ctx);
static bool rate_limit_check(void *ch, struct ioperf_bdev *ioperf, struct ioperf_io_ctx *io_ctx, bool is_thread_ctx);
static void ioperf_process_io_on_target(void *ctx);
static int ioperf_wait_poll(void *ctx);
static void ioperf_register_thread(void *ctx);
static int ioperf_thread_poll(void *ctx);
static void ioperf_reg_access(void);
static void ioperf_mem_barrier(void);
static uint32_t ioperf_hash_lba(uint64_t lba, uint32_t num_threads);

/* Collect SPDK threads for IO routing */
static void
ioperf_register_thread(void *ctx)
{
    struct ioperf_thread_mgr *mgr = ctx;
    uint32_t count = __atomic_fetch_add(&mgr->thread_count, 1, __ATOMIC_RELAXED);
    if (count < 128) {
        mgr->threads[count] = spdk_get_thread();
    }
}

/* Thread collection for per-thread context management */
void
ioperf_collect_thread(void *ctx)
{
    struct ioperf_thread_mgr *mgr = ctx;
    struct spdk_thread *thread = spdk_get_thread();
    struct ioperf_thread_ctx *thread_ctx;
    uint32_t i;

    if (!mgr || !thread) {
        return;
    }

    /* Check if thread already has context by searching existing contexts */
    for (i = 0; i < mgr->thread_count; i++) {
        if (mgr->ctxs[i]->thread == thread) {
            return;
        }
    }

    /* Allocate thread context */
    thread_ctx = calloc(1, sizeof(*thread_ctx));
    if (!thread_ctx) {
        SPDK_ERRLOG("Failed to allocate thread context\n");
        return;
    }

    /* Assign sequential thread_id */
    thread_ctx->thread_id = __atomic_fetch_add(&mgr->next_id, 1, __ATOMIC_RELAXED);
    thread_ctx->thread = thread;
    TAILQ_INIT(&thread_ctx->wait_queue);
    TAILQ_INIT(&thread_ctx->rate_limit_queue);
    thread_ctx->last_time = 0;
    thread_ctx->token_bucket = 0;

    /* Register poller */
    thread_ctx->poller = spdk_poller_register(ioperf_thread_poll, thread_ctx, 0);
    if (!thread_ctx->poller) {
        SPDK_ERRLOG("Failed to register poller for thread\n");
        free(thread_ctx);
        return;
    }

    /* Calculate delay_ticks for 100us */
    thread_ctx->delay_ticks = spdk_get_ticks_hz() / 10000;

    /* Add to array */
    void *new_ptr = realloc(mgr->ctxs, (mgr->thread_count + 1) * sizeof(*mgr->ctxs));
    if (!new_ptr) {
        SPDK_ERRLOG("Failed to expand thread context array\n");
        spdk_poller_unregister(&thread_ctx->poller);
        free(thread_ctx);
        return;
    }
    mgr->ctxs = new_ptr;
    mgr->ctxs[mgr->thread_count++] = thread_ctx;
}

/* Get thread ID for current thread */
uint32_t
ioperf_get_thread_id(void)
{
    struct spdk_thread *thread = spdk_get_thread();
    uint32_t i;

    /* Search through registered thread contexts */
    for (i = 0; i < g_ioperf_thread_mgr.thread_count; i++) {
        if (g_ioperf_thread_mgr.ctxs[i]->thread == thread) {
            return g_ioperf_thread_mgr.ctxs[i]->thread_id;
        }
    }
    return UINT32_MAX;
}

/* Stub for per-thread poller - implemented in Task 3 */
static int
ioperf_thread_poll(void *ctx)
{
    /* Get thread-local channel directly */
    struct ioperf_io_channel *ch = ctx;
    struct ioperf_io_ctx *io_ctx, *tmp;
    uint64_t now = spdk_get_ticks();

    /* Process wait queue - complete IO that has waited >100us */
    TAILQ_FOREACH_SAFE(io_ctx, &ch->wait_queue, link, tmp) {
        if (now - io_ctx->queued_io >= ch->delay_ticks) {
            TAILQ_REMOVE(&ch->wait_queue, io_ctx, link);

            /* Simulate hardware register access delay */
            ioperf_reg_access();
            ioperf_mem_barrier();

            /* Complete IO */
            spdk_bdev_io_complete(io_ctx->bio, SPDK_BDEV_IO_STATUS_SUCCESS);

            /* Update stats */
            struct ioperf_bdev *ioperf = (struct ioperf_bdev *)io_ctx->bio->bdev->ctxt;
            ioperf->total_io++;
            ioperf->total_bytes += io_ctx->bio->u.bdev.num_blocks * io_ctx->bio->bdev->blocklen;

            /* Return io_ctx to pool */
            spdk_mempool_put(ioperf->io_pool, io_ctx);
        }
    }

    /* Process rate limit queue - retry IO that is now within rate limit */
    struct ioperf_io_ctx *rl_ctx, *rl_tmp;
    TAILQ_FOREACH_SAFE(rl_ctx, &ch->rate_limit_queue, link, rl_tmp) {
        struct ioperf_bdev *ioperf = (struct ioperf_bdev *)rl_ctx->bio->bdev->ctxt;
        if (rate_limit_check(ch, ioperf, rl_ctx, true)) {
            TAILQ_REMOVE(&ch->rate_limit_queue, rl_ctx, link);
            /* Retry - process in current thread */
            spdk_thread_send_msg(rl_ctx->src_thread, ioperf_process_io_on_target, rl_ctx);
        }
    }

    return 0;
}

static int
bdev_ioperf_get_ctx_size(void)
{
    return sizeof(struct ioperf_io_ctx);
}

static struct spdk_bdev_module ioperf_if = {
    .name = "ioperf",
    .module_init = bdev_ioperf_initialize,
    .module_fini = bdev_ioperf_finish,
    .config_json = bdev_ioperf_config_json,
    .async_fini = true,
    .get_ctx_size = bdev_ioperf_get_ctx_size,
};

SPDK_BDEV_MODULE_REGISTER(ioperf, &ioperf_if)

static int
ioperf_hash_map_init(struct ioperf_hash_map *hash_map, size_t size)
{
    hash_map->size = size;

    if (pthread_rwlock_init(&hash_map->lock, NULL) != 0) {
        SPDK_ERRLOG("Failed to initialize hash map lock\n");
        return -1;
    }

    hash_map->keys = calloc(size, sizeof(int));
    if (!hash_map->keys) {
        pthread_rwlock_destroy(&hash_map->lock);
        return -1;
    }

    hash_map->values = calloc(size, sizeof(int));
    if (!hash_map->values) {
        free(hash_map->keys);
        pthread_rwlock_destroy(&hash_map->lock);
        return -1;
    }

    /* Initialize with sequential values for testing */
    for (size_t i = 0; i < size; i++) {
        hash_map->keys[i] = (int)i;
        hash_map->values[i] = (int)(i * 2);
    }

    return 0;
}

static int
ioperf_bdev_create_cb(void *io_device, void *ctx_buf)
{
    struct ioperf_io_channel *ch = ctx_buf;

    TAILQ_INIT(&ch->wait_queue);
    TAILQ_INIT(&ch->rate_limit_queue);
    ch->queued_io = 0;
    ch->last_time = spdk_get_ticks();
    ch->token_bucket = 0;
    ch->thread_id = spdk_thread_get_id(spdk_get_thread());

    /* Register poller to check wait queue every poll cycle */
    ch->wait_poller = spdk_poller_register(ioperf_wait_poll, ch, 0);

    return 0;
}

static void
ioperf_bdev_destroy_cb(void *io_device, void *ctx_buf)
{
    struct ioperf_io_channel *ch = ctx_buf;

    /* Drain wait queue */
    struct ioperf_io_ctx *ctx;
    while (!TAILQ_EMPTY(&ch->wait_queue)) {
        ctx = TAILQ_FIRST(&ch->wait_queue);
        TAILQ_REMOVE(&ch->wait_queue, ctx, link);
        spdk_bdev_io_complete(ctx->bio, SPDK_BDEV_IO_STATUS_ABORTED);
    }
}

static void
ioperf_hash_map_destroy(struct ioperf_hash_map *hash_map)
{
    if (hash_map->keys) {
        free(hash_map->keys);
    }
    if (hash_map->values) {
        free(hash_map->values);
    }
    pthread_rwlock_destroy(&hash_map->lock);
}

static uint32_t
ioperf_hash_lba(uint64_t lba, uint32_t num_threads)
{
    /* Simple hash: (lba * prime) % num_threads
     * prime = 2654435761 (Knuth's golden ratio)
     */
    return (uint32_t)((lba * 2654435761ULL) % num_threads);
}

/* Forward declaration */
static void ioperf_complete_io(void *ctx);

/* Complete I/O on source thread - io_ctx is freed here */
static void
ioperf_complete_io(void *ctx)
{
    struct ioperf_io_ctx *io_ctx = (struct ioperf_io_ctx *)ctx;
    struct spdk_bdev_io *bdev_io = io_ctx->bio;
    struct ioperf_bdev *ioperf = (struct ioperf_bdev *)bdev_io->bdev->ctxt;

    /* Complete the I/O */
    spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_SUCCESS);

    /* Update stats */
    ioperf->total_io++;
    ioperf->total_bytes += bdev_io->u.bdev.num_blocks * bdev_io->bdev->blocklen;

    /* Free io_ctx back to pool */
    spdk_mempool_put(ioperf->io_pool, io_ctx);
}

/* Simulate hardware register access with busy-wait delay */
static void
ioperf_reg_access(void)
{
    uint32_t val;
    int i;
    uint64_t start_ticks = spdk_get_ticks();
    /* 500ns fixed delay regardless of CPU frequency */
    uint64_t delay_ticks = spdk_get_ticks_hz() / 2000000;  /* 500ns = hz/2000000 */
    static uint32_t dummy = 0;

    /* 4 register accesses, ~500ns each = ~2000ns total */
    for (i = 0; i < 4; i++) {
        val = dummy;  /* read */
        dummy = val;  /* write */
        /* Busy wait fixed 500ns */
        while ((spdk_get_ticks() - start_ticks) < delay_ticks) {
            __asm__ volatile("" ::: "memory");
        }
        start_ticks = spdk_get_ticks();
    }
}

/* Simulate memory barrier access using SPDK portable barrier API */
static void
ioperf_mem_barrier(void)
{
    int i;

    /* 8 memory barriers */
    for (i = 0; i < 8; i++) {
        spdk_mb();
    }
}

/* Wait queue poller - check and complete IO waiting >100us */
static int
ioperf_wait_poll(void *ctx)
{
    struct ioperf_io_channel *ch = (struct ioperf_io_channel *)ctx;
    struct ioperf_io_ctx *wait_ctx, *tmp;
    uint64_t now = spdk_get_ticks();
    uint64_t delay_ticks = spdk_get_ticks_hz() / 10;  /* 100us */

    TAILQ_FOREACH_SAFE(wait_ctx, &ch->wait_queue, link, tmp) {
        if (now - wait_ctx->queued_io >= delay_ticks) {
            TAILQ_REMOVE(&ch->wait_queue, wait_ctx, link);
            /* Simulate hardware register access delay */
            ioperf_reg_access();
            /* Memory barrier */
            ioperf_mem_barrier();
            fill_all_fields(wait_ctx);
            spdk_bdev_io_complete(wait_ctx->bio, SPDK_BDEV_IO_STATUS_SUCCESS);
            struct ioperf_bdev *ioperf = (struct ioperf_bdev *)wait_ctx->bio->bdev->ctxt;
            ioperf->total_io++;
            ioperf->total_bytes += wait_ctx->bio->u.bdev.num_blocks * wait_ctx->bio->bdev->blocklen;
            spdk_mempool_put(ioperf->io_pool, wait_ctx);
        }
    }

    /* Process rate limit queue - try to resubmit IO */
    struct ioperf_io_ctx *rl_ctx, *rl_tmp;
    TAILQ_FOREACH_SAFE(rl_ctx, &ch->rate_limit_queue, link, rl_tmp) {
        struct ioperf_bdev *ioperf = (struct ioperf_bdev *)rl_ctx->bio->bdev->ctxt;
        if (rate_limit_check(ch, ioperf, rl_ctx, false)) {
            /* Rate limit passed, resubmit to target thread */
            TAILQ_REMOVE(&ch->rate_limit_queue, rl_ctx, link);
            uint32_t target_thread_idx = ioperf_hash_lba(rl_ctx->bio->u.bdev.offset_blocks, ioperf->num_threads);
            struct spdk_thread *target_thread;
            if (g_ioperf_thread_mgr.thread_count > 0) {
                target_thread = g_ioperf_thread_mgr.threads[target_thread_idx % g_ioperf_thread_mgr.thread_count];
            } else {
                target_thread = rl_ctx->src_thread;
            }
            rl_ctx->target_thread = target_thread_idx;
            rl_ctx->hash_map_value_1 = (int)(rl_ctx->bio->u.bdev.offset_blocks % ioperf->hash_map_1.size);
            rl_ctx->hash_map_value_2 = (int)((rl_ctx->bio->u.bdev.offset_blocks / 1000) % ioperf->hash_map_2.size);
            spdk_thread_send_msg(rl_ctx->src_thread, ioperf_process_io_on_target, rl_ctx);
        }
    }

    return 0;
}

/* Process I/O on target thread - check wait queue then add new IO */
static void
ioperf_process_io_on_target(void *ctx)
{
    struct ioperf_io_ctx *io_ctx = (struct ioperf_io_ctx *)ctx;
    /* Get thread-local context directly from SPDK */
    struct ioperf_io_channel *ch = spdk_thread_get_ctx(spdk_get_thread());
    struct ioperf_io_ctx *wait_ctx, *tmp;
    uint64_t now = spdk_get_ticks();

    if (!ch) {
        /* Channel not initialized, fail */
        spdk_bdev_io_complete(io_ctx->bio, SPDK_BDEV_IO_STATUS_FAILED);
        struct ioperf_bdev *ioperf = (struct ioperf_bdev *)io_ctx->bio->bdev->ctxt;
        spdk_mempool_put(ioperf->io_pool, io_ctx);
        return;
    }

    /* Check wait queue - process IO that has been waiting >100us */
    TAILQ_FOREACH_SAFE(wait_ctx, &ch->wait_queue, link, tmp) {
        if (now - wait_ctx->queued_io >= ch->queued_io) {
            TAILQ_REMOVE(&ch->wait_queue, wait_ctx, link);
            ioperf_reg_access();
            ioperf_mem_barrier();
            fill_all_fields(wait_ctx);
            spdk_bdev_io_complete(wait_ctx->bio, SPDK_BDEV_IO_STATUS_SUCCESS);
            struct ioperf_bdev *ioperf = (struct ioperf_bdev *)wait_ctx->bio->bdev->ctxt;
            ioperf->total_io++;
            ioperf->total_bytes += wait_ctx->bio->u.bdev.num_blocks * wait_ctx->bio->bdev->blocklen;
            spdk_mempool_put(ioperf->io_pool, wait_ctx);
        }
    }

    /* Add new IO to wait queue */
    io_ctx->queued_io = spdk_get_ticks();
    TAILQ_INSERT_TAIL(&ch->wait_queue, io_ctx, link);
}

static void
fill_all_fields(struct ioperf_io_ctx *ctx)
{
    struct spdk_bdev_io *bio = ctx->bio;

    ctx->field_001 = bio->u.bdev.offset_blocks;
    ctx->field_002 = bio->u.bdev.num_blocks;
    ctx->field_003 = bio->bdev->blocklen;
    ctx->field_004 = bio->type;
    ctx->field_005 = ctx->target_thread;
    ctx->field_006 = ctx->hash_map_value_1;
    ctx->field_007 = ctx->hash_map_value_2;
    ctx->field_008 = spdk_get_ticks();
    ctx->field_009 = bio->u.bdev.iovcnt;
    ctx->field_010 = bio->u.bdev.num_blocks * bio->bdev->blocklen;
    ctx->field_011 = ctx->field_001 + ctx->field_002;
    ctx->field_012 = ctx->field_003 + ctx->field_004;
    ctx->field_013 = ctx->field_005 + ctx->field_006;
    ctx->field_014 = ctx->field_007 + ctx->field_008;
    ctx->field_015 = ctx->field_009 + ctx->field_010;
    ctx->field_016 = ctx->field_001 * 2;
    ctx->field_017 = ctx->field_002 * 2;
    ctx->field_018 = ctx->field_003 * 2;
    ctx->field_019 = ctx->field_004 * 2;
    ctx->field_020 = ctx->field_005 * 2;
    ctx->field_021 = ctx->field_006 * 2;
    ctx->field_022 = ctx->field_007 * 2;
    ctx->field_023 = ctx->field_008 * 2;
    ctx->field_024 = ctx->field_009 * 2;
    ctx->field_025 = ctx->field_010 * 2;
    ctx->field_026 = ctx->field_001 - ctx->field_002;
    ctx->field_027 = ctx->field_003 - ctx->field_004;
    ctx->field_028 = ctx->field_005 - ctx->field_006;
    ctx->field_029 = ctx->field_007 - ctx->field_008;
    ctx->field_030 = ctx->field_009 - ctx->field_010;
    ctx->field_031 = ctx->field_001 & 0xFF;
    ctx->field_032 = ctx->field_002 & 0xFF;
    ctx->field_033 = ctx->field_003 & 0xFF;
    ctx->field_034 = ctx->field_004 & 0xFF;
    ctx->field_035 = ctx->field_005 & 0xFF;
    ctx->field_036 = ctx->field_006 | 0xFF;
    ctx->field_037 = ctx->field_007 | 0xFF;
    ctx->field_038 = ctx->field_008 | 0xFF;
    ctx->field_039 = ctx->field_009 | 0xFF;
    ctx->field_040 = ctx->field_010 | 0xFF;
    ctx->field_041 = ctx->field_001 ^ ctx->field_002;
    ctx->field_042 = ctx->field_003 ^ ctx->field_004;
    ctx->field_043 = ctx->field_005 ^ ctx->field_006;
    ctx->field_044 = ctx->field_007 ^ ctx->field_008;
    ctx->field_045 = ctx->field_009 ^ ctx->field_010;
    ctx->field_046 = ctx->field_001 << 1;
    ctx->field_047 = ctx->field_002 << 1;
    ctx->field_048 = ctx->field_003 << 1;
    ctx->field_049 = ctx->field_004 << 1;
    ctx->field_050 = ctx->field_005 << 1;
    ctx->field_051 = ctx->field_006 >> 1;
    ctx->field_052 = ctx->field_007 >> 1;
    ctx->field_053 = ctx->field_008 >> 1;
    ctx->field_054 = ctx->field_009 >> 1;
    ctx->field_055 = ctx->field_010 >> 1;
    ctx->field_056 = ctx->field_001 + ctx->field_003;
    ctx->field_057 = ctx->field_002 + ctx->field_004;
    ctx->field_058 = ctx->field_005 + ctx->field_007;
    ctx->field_059 = ctx->field_006 + ctx->field_008;
    ctx->field_060 = ctx->field_009 + ctx->field_010;
    ctx->field_061 = ctx->field_001 * ctx->field_002;
    ctx->field_062 = ctx->field_003 * ctx->field_004;
    ctx->field_063 = ctx->field_005 * ctx->field_006;
    ctx->field_064 = ctx->field_007 * ctx->field_008;
    ctx->field_065 = ctx->field_009 * ctx->field_010;
    ctx->field_066 = ctx->field_001 % 100;
    ctx->field_067 = ctx->field_002 % 100;
    ctx->field_068 = ctx->field_003 % 100;
    ctx->field_069 = ctx->field_004 % 100;
    ctx->field_070 = ctx->field_005 % 100;
    ctx->field_071 = ctx->field_001 / 2;
    ctx->field_072 = ctx->field_002 / 2;
    ctx->field_073 = ctx->field_003 / 2;
    ctx->field_074 = ctx->field_004 / 2;
    ctx->field_075 = ctx->field_005 / 2;
    ctx->field_076 = ctx->field_001 + 1;
    ctx->field_077 = ctx->field_002 + 1;
    ctx->field_078 = ctx->field_003 + 1;
    ctx->field_079 = ctx->field_004 + 1;
    ctx->field_080 = ctx->field_005 + 1;
    ctx->field_081 = ctx->field_001 - 1;
    ctx->field_082 = ctx->field_002 - 1;
    ctx->field_083 = ctx->field_003 - 1;
    ctx->field_084 = ctx->field_004 - 1;
    ctx->field_085 = ctx->field_005 - 1;
    ctx->field_086 = ctx->field_006 + ctx->field_007;
    ctx->field_087 = ctx->field_008 + ctx->field_009;
    ctx->field_088 = ctx->field_010 + ctx->field_001;
    ctx->field_089 = ctx->field_002 + ctx->field_003;
    ctx->field_090 = ctx->field_004 + ctx->field_005;
    ctx->field_091 = ctx->field_006 * ctx->field_007;
    ctx->field_092 = ctx->field_008 * ctx->field_009;
    ctx->field_093 = ctx->field_010 * ctx->field_001;
    ctx->field_094 = ctx->field_002 * ctx->field_003;
    ctx->field_095 = ctx->field_004 * ctx->field_005;
    ctx->field_096 = ctx->field_006 - ctx->field_007;
    ctx->field_097 = ctx->field_008 - ctx->field_009;
    ctx->field_098 = ctx->field_010 - ctx->field_001;
    ctx->field_099 = ctx->field_002 - ctx->field_003;
    ctx->field_100 = ctx->field_004 - ctx->field_005;
    ctx->field_101 = ctx->field_006 & ctx->field_007;
    ctx->field_102 = ctx->field_008 & ctx->field_009;
    ctx->field_103 = ctx->field_010 & ctx->field_001;
    ctx->field_104 = ctx->field_002 & ctx->field_003;
    ctx->field_105 = ctx->field_004 & ctx->field_005;
    ctx->field_106 = ctx->field_006 | ctx->field_007;
    ctx->field_107 = ctx->field_008 | ctx->field_009;
    ctx->field_108 = ctx->field_010 | ctx->field_001;
    ctx->field_109 = ctx->field_002 | ctx->field_003;
    ctx->field_110 = ctx->field_004 | ctx->field_005;
    ctx->field_111 = ctx->field_006 ^ ctx->field_007;
    ctx->field_112 = ctx->field_008 ^ ctx->field_009;
    ctx->field_113 = ctx->field_010 ^ ctx->field_001;
    ctx->field_114 = ctx->field_002 ^ ctx->field_003;
    ctx->field_115 = ctx->field_004 ^ ctx->field_005;
    ctx->field_116 = ctx->field_001 << 2;
    ctx->field_117 = ctx->field_002 << 2;
    ctx->field_118 = ctx->field_003 << 2;
    ctx->field_119 = ctx->field_004 << 2;
    ctx->field_120 = ctx->field_005 << 2;
    ctx->field_121 = ctx->field_006 >> 2;
    ctx->field_122 = ctx->field_007 >> 2;
    ctx->field_123 = ctx->field_008 >> 2;
    ctx->field_124 = ctx->field_009 >> 2;
    ctx->field_125 = ctx->field_010 >> 2;
    ctx->field_126 = ctx->field_001 + ctx->field_002 + ctx->field_003;
    ctx->field_127 = ctx->field_004 + ctx->field_005 + ctx->field_006;
    ctx->field_128 = spdk_get_ticks();
}

static bool
rate_limit_check(void *ch, struct ioperf_bdev *ioperf, struct ioperf_io_ctx *io_ctx, bool is_thread_ctx)
{
    if (ioperf == NULL) {
        return true;
    }

    /* Use is_thread_ctx parameter to determine context type */
    struct ioperf_thread_ctx *thread_ctx = (struct ioperf_thread_ctx *)ch;
    struct ioperf_io_channel *channel = (struct ioperf_io_channel *)ch;

    uint64_t *last_time_ptr;
    uint64_t *token_bucket_ptr;

    if (is_thread_ctx) {
        /* This is a thread context */
        last_time_ptr = &thread_ctx->last_time;
        token_bucket_ptr = &thread_ctx->token_bucket;
    } else {
        /* This is an IO channel */
        last_time_ptr = &channel->last_time;
        token_bucket_ptr = &channel->token_bucket;
    }

    /* Initialize last_time on first call */
    if (*last_time_ptr == 0) {
        *last_time_ptr = spdk_get_ticks();
        /* Give initial tokens to allow IO to proceed */
        *token_bucket_ptr = ioperf->max_bandwidth_mb * 1024 * 1024;
    }

    uint64_t now = spdk_get_ticks();
    uint64_t elapsed = now - *last_time_ptr;

    if (elapsed > 0) {
        /* Refill token bucket: tokens = MB/s * 1024*1024 / ticks_per_sec * elapsed */
        uint64_t tokens_per_second = ioperf->max_bandwidth_mb * 1024 * 1024;
        uint64_t ticks_per_second = spdk_get_ticks_hz();
        uint64_t tokens_to_add = (tokens_per_second / ticks_per_second) * elapsed;
        *token_bucket_ptr += tokens_to_add;
        *last_time_ptr = now;
    }

    uint64_t io_size = io_ctx->bio->u.bdev.num_blocks * io_ctx->bio->bdev->blocklen;

    /* Skip queued_io check for thread_ctx (not applicable) */
    if (channel->queued_io < MAX_QUEUED_IO && *token_bucket_ptr >= io_size) {
        *token_bucket_ptr -= io_size;
        return true;
    }

    /* Rate limited - add to rate limit queue for retry */
    TAILQ_INSERT_TAIL(&channel->rate_limit_queue, io_ctx, link);
    return false;
}

/* Initialize thread pool - will collect threads as they call submit_request */
static int
ioperf_init_thread_pool(struct ioperf_bdev *ioperf)
{
    /* Create memory pool for IO requests (includes routing info + 100+ fields) */
    ioperf->io_pool = spdk_mempool_create("ioperf_io_pool",
                                            4096,
                                            sizeof(struct ioperf_io_ctx),
                                            0, -1);
    if (!ioperf->io_pool) {
        SPDK_ERRLOG("Failed to create IO pool\n");
        return -ENOMEM;
    }

    return 0;
}

/* Cleanup thread pool */
static void
ioperf_destroy_thread_pool(struct ioperf_bdev *ioperf)
{
    if (ioperf->io_pool) {
        spdk_mempool_free(ioperf->io_pool);
        ioperf->io_pool = NULL;
    }
}

static int
bdev_ioperf_destruct(void *ctx)
{
    struct ioperf_bdev *bdev = ctx;

    TAILQ_REMOVE(&g_ioperf_bdev_head, bdev, tailq);

    /* Cleanup thread pool */
    ioperf_destroy_thread_pool(bdev);

    /* Destroy hash maps */
    ioperf_hash_map_destroy(&bdev->hash_map_1);
    ioperf_hash_map_destroy(&bdev->hash_map_2);

    free(bdev->bdev.name);
    free(bdev);

    g_ioperf_bdev = NULL;

    return 0;
}

static void
bdev_ioperf_submit_request(struct spdk_io_channel *_ch, struct spdk_bdev_io *bdev_io)
{
    struct ioperf_bdev *ioperf;
    uint64_t lba = bdev_io->u.bdev.offset_blocks;
    struct ioperf_io_ctx *io_ctx;
    struct spdk_thread *current_thread = spdk_get_thread();
    struct spdk_thread *target_thread;
    uint32_t i;

    /* Get ioperf bdev from bdev context */
    ioperf = (struct ioperf_bdev *)bdev_io->bdev->ctxt;
    if (ioperf == NULL) {
        spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
        return;
    }

    /* Lazy thread collection - initialize thread manager on first IO */
    if (g_ioperf_thread_mgr.ctxs == NULL) {
        /* Initialize thread manager arrays */
        g_ioperf_thread_mgr.threads = calloc(128, sizeof(struct spdk_thread *));
        g_ioperf_thread_mgr.ctxs = calloc(128, sizeof(struct ioperf_thread_ctx *));
        g_ioperf_thread_mgr.thread_count = 0;
        g_ioperf_thread_mgr.thread_allocs = 128;

        /* Collect current thread into thread manager */
        ioperf_collect_thread(&g_ioperf_thread_mgr);
    }

    /* Collect current thread into global thread manager */
    for (i = 0; i < g_ioperf_thread_mgr.thread_count; i++) {
        if (g_ioperf_thread_mgr.threads[i] == current_thread) {
            return;  /* Already registered */
        }
    }
    if (g_ioperf_thread_mgr.thread_count < g_ioperf_thread_mgr.thread_allocs) {
        g_ioperf_thread_mgr.threads[g_ioperf_thread_mgr.thread_count++] = current_thread;
    }

    /* Allocate IO context from memory pool first */
    io_ctx = spdk_mempool_get(ioperf->io_pool);
    if (!io_ctx) {
        SPDK_ERRLOG("Failed to get IO context from pool\n");
        spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
        return;
    }

    /* Initialize IO context */
    io_ctx->bio = bdev_io;
    io_ctx->src_thread = spdk_bdev_io_get_thread(bdev_io);

    /* Check rate limit on source thread */
    struct ioperf_io_channel *src_ch = spdk_io_channel_get_ctx(_ch);
    if (!rate_limit_check(src_ch, ioperf, io_ctx, false)) {
        /* Rate limited - IO added to rate_limit_queue, will retry */
        return;
    }

    /* Calculate target thread using LBA hash - use global thread manager */
    uint32_t thread_count = g_ioperf_thread_mgr.thread_count;
    uint32_t target_thread_idx;

    if (thread_count > 0) {
        target_thread_idx = ioperf_hash_lba(lba, thread_count);
        target_thread = g_ioperf_thread_mgr.threads[target_thread_idx % thread_count];
    } else {
        /* No threads registered yet, process on current thread */
        target_thread = current_thread;
        target_thread_idx = 0;
    }

    /* Lazy-collect target thread if needed */
    if (g_ioperf_thread_mgr.ctxs != NULL) {
        bool target_found = false;
        for (i = 0; i < g_ioperf_thread_mgr.thread_count; i++) {
            if (g_ioperf_thread_mgr.ctxs[i]->thread == target_thread) {
                target_found = true;
                break;
            }
        }
        if (!target_found) {
            ioperf_collect_thread(&g_ioperf_thread_mgr);
        }
    }

    io_ctx->target_thread = target_thread_idx;
    if (!io_ctx) {
        SPDK_ERRLOG("Failed to get IO context from pool\n");
        spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
        return;
    }

    /* Fill hash map values */
    io_ctx->hash_map_value_1 = (int)(lba % ioperf->hash_map_1.size);
    io_ctx->hash_map_value_2 = (int)((lba / 1000) % ioperf->hash_map_2.size);

    /* Process IO in current thread - no cross-thread sending */
    spdk_thread_send_msg(current_thread, ioperf_process_io_on_target, io_ctx);
}

static bool
bdev_ioperf_io_type_supported(void *ctx, enum spdk_bdev_io_type io_type)
{
    switch (io_type) {
    case SPDK_BDEV_IO_TYPE_READ:
    case SPDK_BDEV_IO_TYPE_WRITE:
    case SPDK_BDEV_IO_TYPE_WRITE_ZEROES:
    case SPDK_BDEV_IO_TYPE_RESET:
    case SPDK_BDEV_IO_TYPE_ABORT:
        return true;
    case SPDK_BDEV_IO_TYPE_FLUSH:
    case SPDK_BDEV_IO_TYPE_UNMAP:
    default:
        return false;
    }
}

static struct spdk_io_channel *
bdev_ioperf_get_io_channel(void *ctx)
{
    return spdk_get_io_channel(&g_ioperf_bdev_head);
}

static const struct spdk_bdev_fn_table ioperf_fn_table = {
    .destruct = bdev_ioperf_destruct,
    .submit_request = bdev_ioperf_submit_request,
    .io_type_supported = bdev_ioperf_io_type_supported,
    .get_io_channel = bdev_ioperf_get_io_channel,
    .write_config_json = bdev_ioperf_write_config_json,
};

static void
bdev_ioperf_write_config_json(struct spdk_bdev *bdev, struct spdk_json_write_ctx *w)
{
    struct ioperf_bdev *ioperf = (struct ioperf_bdev *)bdev;

    spdk_json_write_object_begin(w);

    spdk_json_write_named_string(w, "method", "bdev_ioperf_create");

    spdk_json_write_named_object_begin(w, "params");
    spdk_json_write_named_string(w, "name", bdev->name);
    spdk_json_write_named_uint64(w, "num_blocks", bdev->blockcnt);
    spdk_json_write_named_uint32(w, "block_size", bdev->blocklen);
    spdk_json_write_named_uint32(w, "physical_block_size", bdev->phys_blocklen);
    spdk_json_write_named_uint32(w, "num_threads", ioperf->num_threads);
    spdk_json_write_named_uint64(w, "read_latency_us", ioperf->read_latency_us);
    spdk_json_write_named_uint64(w, "write_latency_us", ioperf->write_latency_us);
    spdk_json_write_named_bool(w, "enable_validation", ioperf->enable_validation);
    spdk_json_write_object_end(w);

    spdk_json_write_object_end(w);
}

static int
bdev_ioperf_config_json(struct spdk_json_write_ctx *w)
{
    struct ioperf_bdev *bdev;

    TAILQ_FOREACH(bdev, &g_ioperf_bdev_head, tailq) {
        bdev_ioperf_write_config_json(&bdev->bdev, w);
    }

    return 0;
}

int
bdev_ioperf_create(struct spdk_bdev **bdev, const struct ioperf_bdev_opts *opts)
{
    struct ioperf_bdev *ioperf;
    uint32_t block_size;
    int rc;

    if (!opts) {
        SPDK_ERRLOG("No options provided for ioperf bdev.\n");
        return -EINVAL;
    }

    if (opts->num_blocks == 0) {
        SPDK_ERRLOG("Disk must be more than 0 blocks\n");
        return -EINVAL;
    }

    if (opts->block_size % 512 != 0) {
        SPDK_ERRLOG("Data block size %u is not a multiple of 512.\n", opts->block_size);
        return -EINVAL;
    }

    if (opts->physical_block_size % 512 != 0) {
        SPDK_ERRLOG("Physical block must be 512 bytes aligned\n");
        return -EINVAL;
    }

    block_size = opts->block_size;

    ioperf = calloc(1, sizeof(*ioperf));
    if (!ioperf) {
        SPDK_ERRLOG("Could not allocate ioperf_bdev\n");
        return -ENOMEM;
    }

    ioperf->bdev.name = strdup(opts->name);
    if (!ioperf->bdev.name) {
        free(ioperf);
        return -ENOMEM;
    }

    ioperf->bdev.product_name = "ioperf disk";

    ioperf->bdev.write_cache = 0;
    ioperf->bdev.blocklen = block_size;
    ioperf->bdev.phys_blocklen = opts->physical_block_size;
    ioperf->bdev.blockcnt = opts->num_blocks;

    /* Configuration */
    ioperf->num_threads = opts->num_threads;
    ioperf->read_latency_us = opts->read_latency_us;
    ioperf->write_latency_us = opts->write_latency_us;
    ioperf->max_iops = IOPERF_MAX_IOPS;
    ioperf->max_bandwidth_mb = IOPERF_MAX_BANDWIDTH_MB;
    ioperf->enable_validation = opts->enable_validation;

    /* Initialize hash maps */
    rc = ioperf_hash_map_init(&ioperf->hash_map_1, IOPERF_HASH_MAP_SIZE);
    if (rc != 0) {
        free(ioperf->bdev.name);
        free(ioperf);
        return rc;
    }

    rc = ioperf_hash_map_init(&ioperf->hash_map_2, IOPERF_HASH_MAP_SIZE);
    if (rc != 0) {
        ioperf_hash_map_destroy(&ioperf->hash_map_1);
        free(ioperf->bdev.name);
        free(ioperf);
        return rc;
    }

    /* Generate UUID */
    spdk_uuid_generate(&ioperf->bdev.uuid);

    ioperf->bdev.ctxt = ioperf;
    ioperf->bdev.fn_table = &ioperf_fn_table;
    ioperf->bdev.module = &ioperf_if;

    ioperf->total_io = 0;
    ioperf->total_bytes = 0;

    SPDK_NOTICELOG("ioperf: creating bdev with %u threads, read_latency=%lu us, write_latency=%lu us\n",
               ioperf->num_threads, ioperf->read_latency_us, ioperf->write_latency_us);

    /* Create worker threads */
    rc = ioperf_init_thread_pool(ioperf);
    if (rc) {
        ioperf_hash_map_destroy(&ioperf->hash_map_1);
        ioperf_hash_map_destroy(&ioperf->hash_map_2);
        free(ioperf->bdev.name);
        free(ioperf);
        return rc;
    }

    /* Thread collection is done lazily on first IO submission
     * Removing spdk_for_each_thread from here as it causes RPC hang
     * TODO: Implement lazy thread collection
     */

    SPDK_NOTICELOG("ioperf: registering bdev\n");
    rc = spdk_bdev_register(&ioperf->bdev);
    if (rc) {
        SPDK_ERRLOG("ioperf: bdev_register failed with rc=%d\n", rc);
        ioperf_destroy_thread_pool(ioperf);
        ioperf_hash_map_destroy(&ioperf->hash_map_1);
        ioperf_hash_map_destroy(&ioperf->hash_map_2);
        free(ioperf->bdev.name);
        free(ioperf);
        return rc;
    }
    SPDK_NOTICELOG("ioperf: bdev registered successfully\n");

    *bdev = &(ioperf->bdev);
    SPDK_NOTICELOG("ioperf: bdev assigned, name=%s\n", ioperf->bdev.name);

    TAILQ_INSERT_TAIL(&g_ioperf_bdev_head, ioperf, tailq);
    SPDK_NOTICELOG("ioperf: added to tailq\n");

    g_ioperf_bdev = ioperf;
    SPDK_NOTICELOG("ioperf: g_ioperf_bdev set\n");
    SPDK_NOTICELOG("ioperf: bdev_create complete!\n");

    return rc;
}

void
bdev_ioperf_delete(const char *bdev_name, spdk_delete_ioperf_complete cb_fn, void *cb_arg)
{
    struct ioperf_bdev *bdev = NULL;
    struct spdk_bdev *found;

    found = spdk_bdev_get_by_name(bdev_name);
    if (!found) {
        cb_fn(cb_arg, -ENODEV);
        return;
    }

    bdev = found->ctxt;

    spdk_bdev_unregister(&bdev->bdev, cb_fn, cb_arg);
}

int
bdev_ioperf_resize(const char *bdev_name, const uint64_t new_size_in_mb)
{
    struct spdk_bdev *found;
    struct ioperf_bdev *bdev;
    uint64_t new_num_blocks;

    found = spdk_bdev_get_by_name(bdev_name);
    if (!found) {
        return -ENODEV;
    }

    bdev = found->ctxt;

    new_num_blocks = (new_size_in_mb * 1024 * 1024) / bdev->bdev.blocklen;

    bdev->bdev.blockcnt = new_num_blocks;

    return 0;
}

static int
bdev_ioperf_initialize(void)
{
    /* Register the io_device */
    spdk_io_device_register(&g_ioperf_bdev_head, ioperf_bdev_create_cb, ioperf_bdev_destroy_cb,
                            sizeof(struct ioperf_io_channel), "ioperf_bdev");

    /* Initialize thread manager */
    g_ioperf_thread_mgr.ctxs = NULL;
    g_ioperf_thread_mgr.threads = NULL;
    g_ioperf_thread_mgr.thread_count = 0;
    __atomic_store_n(&g_ioperf_thread_mgr.next_id, 0, __ATOMIC_RELAXED);

    /* Initialize RPC handlers */
    bdev_ioperf_rpc_init();

    SPDK_NOTICELOG("ioperf: initialized\n");

    return 0;
}

static void
bdev_ioperf_finish(void)
{
    if (!g_ioperf_thread_mgr.ctxs) {
        return;
    }

    /* Drain wait_queues and cleanup each thread */
    for (uint32_t i = 0; i < g_ioperf_thread_mgr.thread_count; i++) {
        struct ioperf_thread_ctx *thread_ctx = g_ioperf_thread_mgr.ctxs[i];
        struct ioperf_io_ctx *io_ctx;

        /* Drain wait_queue */
        while (!TAILQ_EMPTY(&thread_ctx->wait_queue)) {
            io_ctx = TAILQ_FIRST(&thread_ctx->wait_queue);
            TAILQ_REMOVE(&thread_ctx->wait_queue, io_ctx, link);
            spdk_bdev_io_complete(io_ctx->bio, SPDK_BDEV_IO_STATUS_ABORTED);
            struct ioperf_bdev *ioperf = (struct ioperf_bdev *)io_ctx->bio->bdev->ctxt;
            spdk_mempool_put(ioperf->io_pool, io_ctx);
        }

        /* Drain rate_limit_queue */
        while (!TAILQ_EMPTY(&thread_ctx->rate_limit_queue)) {
            io_ctx = TAILQ_FIRST(&thread_ctx->rate_limit_queue);
            TAILQ_REMOVE(&thread_ctx->rate_limit_queue, io_ctx, link);
            spdk_bdev_io_complete(io_ctx->bio, SPDK_BDEV_IO_STATUS_ABORTED);
            struct ioperf_bdev *ioperf = (struct ioperf_bdev *)io_ctx->bio->bdev->ctxt;
            spdk_mempool_put(ioperf->io_pool, io_ctx);
        }

        /* Unregister poller */
        if (thread_ctx->poller) {
            spdk_poller_unregister(&thread_ctx->poller);
        }

        /* Free thread context */
        free(thread_ctx);
    }

    /* Free array and reset manager */
    free(g_ioperf_thread_mgr.ctxs);
    g_ioperf_thread_mgr.ctxs = NULL;
    g_ioperf_thread_mgr.thread_count = 0;
}
