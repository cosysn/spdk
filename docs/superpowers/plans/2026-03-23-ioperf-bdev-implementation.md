# ioperf bdev Module Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Create an independent memory-only bdev module with configurable latency, LBA-based hash routing, 100+ field IO context, global rate limiting, and hash map with read locks.

**Architecture:** Follows SPDK bdev module pattern (similar to null/delay modules). Uses TAILQ for wait queues, pthread_rwlock for hash map, atomic operations for stats.

**Tech Stack:** C, SPDK bdev framework, pthread, DPDK

---

## File Structure

```
module/bdev/ioperf/
├── Makefile              # Build file
├── bdev_ioperf.h        # Header with structs and macros
├── bdev_ioperf.c        # Main module implementation
└── bdev_ioperf_rpc.c    # RPC handlers
```

**Reference files:**
- `module/bdev/null/bdev_null.c` - Simple bdev module pattern
- `module/bdev/null/bdev_null.h` - Header pattern
- `module/bdev/null/bdev_null_rpc.c` - RPC pattern
- `module/bdev/null/Makefile` - Build pattern

---

## Task 1: Create Module Directory and Makefile

**Files:**
- Create: `module/bdev/ioperf/Makefile`

- [ ] **Step 1: Create Makefile**

```makefile
#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2024 Intel Corporation.
#  All rights reserved.
#

SPDK_ROOT_DIR := $(abspath $(CURDIR)/../../..)
include $(SPDK_ROOT_DIR)/mk/spdk.common.mk

SO_VER := 8
SO_MINOR := 0

C_SRCS = bdev_ioperf.c bdev_ioperf_rpc.c
LIBNAME = bdev_ioperf

SPDK_MAP_FILE = $(SPDK_ROOT_DIR)/mk/spdk_blank.map

include $(SPDK_ROOT_DIR)/mk/spdk.lib.mk
```

- [ ] **Step 2: Commit**

```bash
git add module/bdev/ioperf/Makefile
git commit -m "feat(ioperf): add Makefile

Co-Authored-By: Claude Opus 4.5 <noreply@anthropic.com>"
```

---

## Task 2: Create Header File (bdev_ioperf.h)

**Files:**
- Create: `module/bdev/ioperf/bdev_ioperf.h`

- [ ] **Step 1: Write header file**

```c
/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2024 Intel Corporation.
 *   All rights reserved.
 */

#ifndef SPDK_BDEV_IOPERF_H
#define SPDK_BDEV_IOPERF_H

#include "spdk/stdinc.h"
#include "spdk/bdev.h"
#include "spdk/bdev_module.h"
#include <pthread.h>

/* Compile-time configuration macros */
#ifndef IOPERF_MAX_IOPS
#define IOPERF_MAX_IOPS           1000000
#endif

#ifndef IOPERF_MAX_BANDWIDTH_MB
#define IOPERF_MAX_BANDWIDTH_MB   1000
#endif

#ifndef IOPERF_DEFAULT_READ_LATENCY_US
#define IOPERF_DEFAULT_READ_LATENCY_US   0
#endif

#ifndef IOPERF_DEFAULT_WRITE_LATENCY_US
#define IOPERF_DEFAULT_WRITE_LATENCY_US  0
#endif

#ifndef IOPERF_IO_CTX_FIELDS
#define IOPERF_IO_CTX_FIELDS      128
#endif

#ifndef IOPERF_HASH_MAP_SIZE
#define IOPERF_HASH_MAP_SIZE      1000000
#endif

/* Forward declarations */
struct ioperf_bdev;
struct ioperf_io_ctx;

/* Hash map structure */
struct ioperf_hash_map {
    pthread_rwlock_t    lock;
    int                *keys;
    int                *values;
    size_t             size;
};

/* ioperf bdev structure */
struct ioperf_bdev {
    struct spdk_bdev           bdev;
    TAILQ_ENTRY(ioperf_bdev)  tailq;

    /* Configuration */
    uint32_t                   num_threads;
    uint64_t                   read_latency_us;
    uint64_t                   write_latency_us;
    uint64_t                   max_iops;
    uint64_t                   max_bandwidth_mb;
    bool                       enable_validation;

    /* Global stats (atomic) */
    SPDK_ATOMIC(uint64_t)      total_io;
    SPDK_ATOMIC(uint64_t)      total_bytes;

    /* Hash maps for testing read lock performance */
    struct ioperf_hash_map     hash_map_1;
    struct ioperf_hash_map     hash_map_2;
};

/* IO channel structure (per thread) */
struct ioperf_io_channel {
    struct spdk_poller              *poller;
    TAILQ_HEAD(, ioperf_io_ctx)    wait_queue;
    uint64_t                        queued_io;
    uint64_t                        last_time;
    uint64_t                        token_bucket;
    uint32_t                        thread_id;
};

/* IO context (driver_ctx) - 100+ fields */
struct ioperf_io_ctx {
    TAILQ_ENTRY(ioperf_io_ctx) link;
    struct spdk_bdev_io        *bio;

    /* 100+ fields for memory access simulation */
    uint64_t                   field_001;
    uint64_t                   field_002;
    uint64_t                   field_003;
    uint64_t                   field_004;
    uint64_t                   field_005;
    uint64_t                   field_006;
    uint64_t                   field_007;
    uint64_t                   field_008;
    uint64_t                   field_009;
    uint64_t                   field_010;
    uint64_t                   field_011;
    uint64_t                   field_012;
    uint64_t                   field_013;
    uint64_t                   field_014;
    uint64_t                   field_015;
    uint64_t                   field_016;
    uint64_t                   field_017;
    uint64_t                   field_018;
    uint64_t                   field_019;
    uint64_t                   field_020;
    uint64_t                   field_021;
    uint64_t                   field_022;
    uint64_t                   field_023;
    uint64_t                   field_024;
    uint64_t                   field_025;
    uint64_t                   field_026;
    uint64_t                   field_027;
    uint64_t                   field_028;
    uint64_t                   field_029;
    uint64_t                   field_030;
    uint64_t                   field_031;
    uint64_t                   field_032;
    uint64_t                   field_033;
    uint64_t                   field_034;
    uint64_t                   field_035;
    uint64_t                   field_036;
    uint64_t                   field_037;
    uint64_t                   field_038;
    uint64_t                   field_039;
    uint64_t                   field_040;
    uint64_t                   field_041;
    uint64_t                   field_042;
    uint64_t                   field_043;
    uint64_t                   field_044;
    uint64_t                   field_045;
    uint64_t                   field_046;
    uint64_t                   field_047;
    uint64_t                   field_048;
    uint64_t                   field_049;
    uint64_t                   field_050;
    uint64_t                   field_051;
    uint64_t                   field_052;
    uint64_t                   field_053;
    uint64_t                   field_054;
    uint64_t                   field_055;
    uint64_t                   field_056;
    uint64_t                   field_057;
    uint64_t                   field_058;
    uint64_t                   field_059;
    uint64_t                   field_060;
    uint64_t                   field_061;
    uint64_t                   field_062;
    uint64_t                   field_063;
    uint64_t                   field_064;
    uint64_t                   field_065;
    uint64_t                   field_066;
    uint64_t                   field_067;
    uint64_t                   field_068;
    uint64_t                   field_069;
    uint64_t                   field_070;
    uint64_t                   field_071;
    uint64_t                   field_072;
    uint64_t                   field_073;
    uint64_t                   field_074;
    uint64_t                   field_075;
    uint64_t                   field_076;
    uint64_t                   field_077;
    uint64_t                   field_078;
    uint64_t                   field_079;
    uint64_t                   field_080;
    uint64_t                   field_081;
    uint64_t                   field_082;
    uint64_t                   field_083;
    uint64_t                   field_084;
    uint64_t                   field_085;
    uint64_t                   field_086;
    uint64_t                   field_087;
    uint64_t                   field_088;
    uint64_t                   field_089;
    uint64_t                   field_090;
    uint64_t                   field_091;
    uint64_t                   field_092;
    uint64_t                   field_093;
    uint64_t                   field_094;
    uint64_t                   field_095;
    uint64_t                   field_096;
    uint64_t                   field_097;
    uint64_t                   field_098;
    uint64_t                   field_099;
    uint64_t                   field_100;
    uint64_t                   field_101;
    uint64_t                   field_102;
    uint64_t                   field_103;
    uint64_t                   field_104;
    uint64_t                   field_105;
    uint64_t                   field_106;
    uint64_t                   field_107;
    uint64_t                   field_108;
    uint64_t                   field_109;
    uint64_t                   field_110;
    uint64_t                   field_111;
    uint64_t                   field_112;
    uint64_t                   field_113;
    uint64_t                   field_114;
    uint64_t                   field_115;
    uint64_t                   field_116;
    uint64_t                   field_117;
    uint64_t                   field_118;
    uint64_t                   field_119;
    uint64_t                   field_120;
    uint64_t                   field_121;
    uint64_t                   field_122;
    uint64_t                   field_123;
    uint64_t                   field_124;
    uint64_t                   field_125;
    uint64_t                   field_126;
    uint64_t                   field_127;
    uint64_t                   field_128;

    /* Routing info */
    uint32_t                   target_thread;

    /* Hash map values */
    int                        hash_map_value_1;
    int                        hash_map_value_2;
};

/* Options for creating ioperf bdev */
struct ioperf_bdev_opts {
    char                *name;
    struct spdk_uuid   uuid;
    uint64_t           num_blocks;
    uint32_t           block_size;
    uint32_t           physical_block_size;
    uint32_t           num_threads;
    uint64_t           read_latency_us;
    uint64_t           write_latency_us;
    bool               enable_validation;
};

/* Delete callback */
typedef void (*spdk_delete_ioperf_complete)(void *cb_arg, int bdeverrno);

/* API functions */
int bdev_ioperf_create(struct spdk_bdev **bdev, const struct ioperf_bdev_opts *opts);
void bdev_ioperf_delete(const char *bdev_name, spdk_delete_ioperf_complete cb_fn, void *cb_arg);
int bdev_ioperf_resize(const char *bdev_name, const uint64_t new_size_in_mb);

#endif /* SPDK_BDEV_IOPERF_H */
```

- [ ] **Step 2: Commit**

```bash
git add module/bdev/ioperf/bdev_ioperf.h
git commit -m "feat(ioperf): add header file with structs and macros

Co-Authored-By: Claude Opus 4.5 <noreply@anthropic.com>"
```

---

## Task 3: Create Main Module Implementation (bdev_ioperf.c)

**Files:**
- Create: `module/bdev/ioperf/bdev_ioperf.c`

This is the largest task with many sub-steps. See the detailed implementation below.

- [ ] **Step 1: Write includes and globals**

```c
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

#include "spdk/bdev_module.h"
#include "spdk/log.h"

#include "bdev_ioperf.h"

static TAILQ_HEAD(, ioperf_bdev) g_ioperf_bdev_head = TAILQ_HEAD_INITIALIZER(g_ioperf_bdev_head);
static struct ioperf_bdev *g_ioperf_bdev = NULL;
static void *g_ioperf_read_buf;

#define MAX_QUEUED_IO 1024

static int bdev_ioperf_initialize(void);
static void bdev_ioperf_finish(void);
static int bdev_ioperf_config_json(struct spdk_json_write_ctx *w);
```

- [ ] **Step 2: Write get_ctx_size function**

```c
static int
bdev_ioperf_get_ctx_size(void)
{
    return sizeof(struct ioperf_io_ctx);
}
```

- [ ] **Step 3: Write module registration**

```c
static struct spdk_bdev_module ioperf_if = {
    .name = "ioperf",
    .module_init = bdev_ioperf_initialize,
    .module_fini = bdev_ioperf_finish,
    .config_json = bdev_ioperf_config_json,
    .async_fini = true,
    .get_ctx_size = bdev_ioperf_get_ctx_size,
};

SPDK_BDEV_MODULE_REGISTER(ioperf, &ioperf_if)
```

- [ ] **Step 4: Write hash map functions**

```c
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

static int
ioperf_hash_map_get(struct ioperf_hash_map *hash_map, int key, int *value)
{
    int idx = key % (int)hash_map->size;

    pthread_rwlock_rdlock(&hash_map->lock);
    *value = hash_map->values[idx];
    pthread_rwlock_unlock(&hash_map->lock);

    return 0;
}
```

- [ ] **Step 5: Write LBA hash function**

```c
static uint32_t
ioperf_hash_lba(uint64_t lba, uint32_t num_threads)
{
    /* Simple hash: (lba * prime) % num_threads
     * prime = 2654435761 (Knuth's golden ratio)
     */
    return (uint32_t)((lba * 2654435761ULL) % num_threads);
}
```

- [ ] **Step 6: Write fill_all_fields function**

```c
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
    /* ... assign all 128 fields ... */
    ctx->field_128 = spdk_get_ticks();
}
```

- [ ] **Step 7: Write rate_limit_check function**

```c
static bool
rate_limit_check(struct ioperf_io_channel *ch, struct spdk_bdev_io *bdev_io)
{
    struct ioperf_bdev *ioperf = g_ioperf_bdev;
    uint64_t now = spdk_get_ticks();
    uint64_t elapsed = now - ch->last_time;

    if (elapsed == 0) {
        return ch->token_bucket >= bdev_io->u.bdev.num_blocks * bdev_io->bdev->blocklen;
    }

    /* Refill token bucket */
    uint64_t tokens_per_tick = ioperf->max_bandwidth_mb * 1024 * 1024 / spdk_get_ticks_hz();
    ch->token_bucket += elapsed * tokens_per_tick;
    ch->last_time = now;

    uint64_t io_size = bdev_io->u.bdev.num_blocks * bdev_io->bdev->blocklen;

    if (ch->queued_io < MAX_QUEUED_IO && ch->token_bucket >= io_size) {
        ch->token_bucket -= io_size;
        return true;
    }
    return false;
}
```

- [ ] **Step 8: Write destruct function**

```c
static int
bdev_ioperf_destruct(void *ctx)
{
    struct ioperf_bdev *bdev = ctx;

    TAILQ_REMOVE(&g_ioperf_bdev_head, bdev, tailq);

    /* Destroy hash maps */
    ioperf_hash_map_destroy(&bdev->hash_map_1);
    ioperf_hash_map_destroy(&bdev->hash_map_2);

    free(bdev->bdev.name);
    free(bdev);

    g_ioperf_bdev = NULL;

    return 0;
}
```

- [ ] **Step 9: Write abort_io function**

```c
static bool
bdev_ioperf_abort_io(struct ioperf_io_channel *ch, struct spdk_bdev_io *bio_to_abort)
{
    struct ioperf_io_ctx *io_ctx;
    struct spdk_bdev_io *bdev_io;

    TAILQ_FOREACH(io_ctx, &ch->wait_queue, link) {
        bdev_io = spdk_bdev_io_from_ctx(io_ctx);

        if (bdev_io == bio_to_abort) {
            TAILQ_REMOVE(&ch->wait_queue, io_ctx, link);
            ch->queued_io--;
            spdk_bdev_io_complete(bio_to_abort, SPDK_BDEV_IO_STATUS_ABORTED);
            return true;
        }
    }

    return false;
}
```

- [ ] **Step 10: Write submit_request function**

```c
static void
bdev_ioperf_submit_request(struct spdk_io_channel *_ch, struct spdk_bdev_io *bdev_io)
{
    struct ioperf_io_ctx *ctx = (struct ioperf_io_ctx *)bdev_io->driver_ctx;
    struct ioperf_io_channel *ch = spdk_io_channel_get_ctx(_ch);
    struct ioperf_bdev *ioperf = g_ioperf_bdev;
    uint64_t lba = bdev_io->u.bdev.offset_blocks;

    ctx->bio = bdev_io;

    /* 1. LBA hash routing to target thread */
    ctx->target_thread = ioperf_hash_lba(lba, ioperf->num_threads);

    /* 2. Hash map get operations (with pthread read lock) */
    int hash_key_1 = (int)(lba % ioperf->hash_map_1.size);
    int hash_key_2 = (int)((lba / 1000) % ioperf->hash_map_2.size);

    ioperf_hash_map_get(&ioperf->hash_map_1, hash_key_1, &ctx->hash_map_value_1);
    ioperf_hash_map_get(&ioperf->hash_map_2, hash_key_2, &ctx->hash_map_value_2);

    /* 3. Get target thread's channel */
    struct ioperf_io_channel *target_ch = &ch[ctx->target_thread];

    /* 4. Check rate limit */
    if (!rate_limit_check(target_ch, bdev_io)) {
        TAILQ_INSERT_TAIL(&target_ch->wait_queue, ctx, link);
        target_ch->queued_io++;
        return;
    }

    /* 5. Fill all 100+ fields */
    fill_all_fields(ctx);

    /* 6. Complete IO immediately (no persistence, no actual delay in submit path) */
    spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_SUCCESS);

    /* 7. Update stats */
    spdk_atomic_fetch_add(&ioperf->total_io, 1);
    spdk_atomic_fetch_add(&ioperf->total_bytes,
                          bdev_io->u.bdev.num_blocks * bdev_io->bdev->blocklen);
}
```

- [ ] **Step 11: Write io_type_supported function**

```c
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
```

- [ ] **Step 12: Write get_io_channel function**

```c
static struct spdk_io_channel *
bdev_ioperf_get_io_channel(void *ctx)
{
    return spdk_get_io_channel(&g_ioperf_bdev_head);
}
```

- [ ] **Step 13: Write fn_table and config functions**

```c
static const struct spdk_bdev_fn_table ioperf_fn_table = {
    .destruct = bdev_ioperf_destruct,
    .submit_request = bdev_ioperf_submit_request,
    .io_type_supported = bdev_ioperf_io_type_supported,
    .get_io_channel = bdev_ioperf_get_io_channel,
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
    spdk_json_write_named_uuid(w, "uuid", &bdev->uuid);
    spdk_json_write_object_end(w);

    spdk_json_write_object_end(w);
}

static int
bdev_ioperf_config_json(struct spdk_json_write_ctx *w)
{
    struct ioperf_bdev *bdev;

    spdk_json_write_batch_begin(w);
    TAILQ_FOREACH(bdev, &g_ioperf_bdev_head, tailq) {
        bdev_ioperf_write_config_json(&bdev->bdev, w);
    }
    spdk_json_write_batch_end(w);

    return 0;
}
```

- [ ] **Step 14: Write bdev_ioperf_create function**

```c
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

    if (!spdk_uuid_is_null(&opts->uuid)) {
        spdk_uuid_copy(&ioperf->bdev.uuid, &opts->uuid);
    }

    ioperf->bdev.ctxt = ioperf;
    ioperf->bdev.fn_table = &ioperf_fn_table;
    ioperf->bdev.module = &ioperf_if;

    spdk_atomic_init(&ioperf->total_io, 0);
    spdk_atomic_init(&ioperf->total_bytes, 0);

    rc = spdk_bdev_register(&ioperf->bdev);
    if (rc) {
        ioperf_hash_map_destroy(&ioperf->hash_map_1);
        ioperf_hash_map_destroy(&ioperf->hash_map_2);
        free(ioperf->bdev.name);
        free(ioperf);
        return rc;
    }

    *bdev = &(ioperf->bdev);

    TAILQ_INSERT_TAIL(&g_ioperf_bdev_head, ioperf, tailq);
    g_ioperf_bdev = ioperf;

    return rc;
}
```

- [ ] **Step 15: Write delete and resize functions**

```c
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
```

- [ ] **Step 16: Write initialize and finish functions**

```c
static int
bdev_ioperf_initialize(void)
{
    /* Allocate read buffer */
    g_ioperf_read_buf = spdk_zmalloc(SPDK_BDEV_LARGE_BUF_MAX_SIZE, 0x1000, NULL,
                                      SPDK_ENV_SOCKET_ID_ANY, SPDK_MALLOC_DMA);
    if (!g_ioperf_read_buf) {
        SPDK_ERRLOG("Failed to allocate read buffer\n");
        return -ENOMEM;
    }

    return 0;
}

static void
bdev_ioperf_finish(void)
{
    if (g_ioperf_read_buf) {
        spdk_free(g_ioperf_read_buf);
        g_ioperf_read_buf = NULL;
    }
}
```

- [ ] **Step 17: Commit**

```bash
git add module/bdev/ioperf/bdev_ioperf.c
git commit -m "feat(ioperf): add main module implementation

Co-Authored-By: Claude Opus 4.5 <noreply@anthropic.com>"
```

---

## Task 4: Create RPC Handler (bdev_ioperf_rpc.c)

**Files:**
- Create: `module/bdev/ioperf/bdev_ioperf_rpc.c`

- [ ] **Step 1: Write RPC handler**

```c
/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2024 Intel Corporation.
 *   All rights reserved.
 */

#include "spdk/stdinc.h"

#include "spdk/bdev.h"
#include "spdk/json.h"
#include "spdk/string.h"

#include "spdk/bdev_module.h"
#include "spdk/log.h"

#include "bdev_ioperf.h"

static void
bdev_ioperf_create_json(const struct ioperf_bdev_opts *opts, struct spdk_json_write_ctx *w)
{
    spdk_json_write_object_begin(w);
    spdk_json_write_named_string(w, "name", opts->name);
    spdk_json_write_named_uuid(w, "uuid", &opts->uuid);
    spdk_json_write_named_uint64(w, "num_blocks", opts->num_blocks);
    spdk_json_write_named_uint32(w, "block_size", opts->block_size);
    spdk_json_write_named_uint32(w, "physical_block_size", opts->physical_block_size);
    spdk_json_write_named_uint32(w, "num_threads", opts->num_threads);
    spdk_json_write_named_uint64(w, "read_latency_us", opts->read_latency_us);
    spdk_json_write_named_uint64(w, "write_latency_us", opts->write_latency_us);
    spdk_json_write_named_bool(w, "enable_validation", opts->enable_validation);
    spdk_json_write_object_end(w);
}

static int
bdev_ioperf_create_parse_opts(struct spdk_json_parse_ctx *ctx, struct ioperf_bdev_opts *opts)
{
    memset(opts, 0, sizeof(*opts));

    if (spdk_json_decode_string(ctx, "name", &opts->name) != 0) {
        SPDK_ERRLOG("bdev_ioperf_create: missing name parameter\n");
        return -EINVAL;
    }

    spdk_json_decode_uuid(ctx, "uuid", &opts->uuid);

    if (spdk_json_decode_uint64(ctx, "num_blocks", &opts->num_blocks) != 0) {
        opts->num_blocks = 131072; /* default 512MB */
    }

    if (spdk_json_decode_uint32(ctx, "block_size", &opts->block_size) != 0) {
        opts->block_size = 512;
    }

    if (spdk_json_decode_uint32(ctx, "physical_block_size", &opts->physical_block_size) != 0) {
        opts->physical_block_size = 512;
    }

    if (spdk_json_decode_uint32(ctx, "num_threads", &opts->num_threads) != 0) {
        opts->num_threads = 4;
    }

    if (spdk_json_decode_uint64(ctx, "read_latency_us", &opts->read_latency_us) != 0) {
        opts->read_latency_us = IOPERF_DEFAULT_READ_LATENCY_US;
    }

    if (spdk_json_decode_uint64(ctx, "write_latency_us", &opts->write_latency_us) != 0) {
        opts->write_latency_us = IOPERF_DEFAULT_WRITE_LATENCY_US;
    }

    if (spdk_json_decode_bool(ctx, "enable_validation", &opts->enable_validation) != 0) {
        opts->enable_validation = false;
    }

    return 0;
}

static int
bdev_ioperf_create_handler(struct spdk_jsonrpc_request *request,
                          const struct spdk_json_val *params)
{
    struct ioperf_bdev_opts opts = {};
    struct spdk_bdev *bdev = NULL;
    int rc;

    if (params == NULL || params->type != SPDK_JSON_VAL_OBJECT) {
        SPDK_ERRLOG("bdev_ioperf_create: params is not an object\n");
        return SPDK_JSONRPC_ERROR_INVALID_PARAMS;
    }

    rc = bdev_ioperf_create_parse_opts(params, &opts);
    if (rc != 0) {
        return rc;
    }

    rc = bdev_ioperf_create(&bdev, &opts);
    free(opts.name);

    if (rc != 0) {
        return rc;
    }

    spdk_jsonrpc_start_response(request);
    bdev_ioperf_create_json(&opts, request->w);
    spdk_jsonrpc_end_response(request);

    return 0;
}

SPDK_JSONRPC_DONE(bdev_ioperf_create_handler)
```

- [ ] **Step 2: Register RPC in bdev_ioperf.c**

Add this to bdev_ioperf.c after includes:

```c
/* RPC handlers are in bdev_ioperf_rpc.c */
extern int bdev_ioperf_rpc_init(void);
```

Add this to bdev_ioperf_initialize():

```c
bdev_ioperf_rpc_init();
```

- [ ] **Step 3: Commit**

```bash
git add module/bdev/ioperf/bdev_ioperf_rpc.c
git commit -m "feat(ioperf): add RPC handlers

Co-Authored-By: Claude Opus 4.5 <noreply@anthropic.com>"
```

---

## Task 5: Build and Test

**Files:**
- Modify: `module/bdev/Makefile` (to include ioperf)

- [ ] **Step 1: Check if module/bdev/Makefile needs update**

Run: `ls module/bdev/Makefile`

If it doesn't exist or doesn't list submodules, check how other modules are built.

- [ ] **Step 2: Build ioperf module**

Run: `cd /home/ubuntu/spdk && make`

Expected: Should compile without errors

- [ ] **Step 3: Run unit tests**

Run: `./test/unit/unittest.sh`

- [ ] **Step 4: Commit build changes**

```bash
git add -A
git commit -m "build(ioperf): add ioperf module to build

Co-Authored-By: Claude Opus 4.5 <noreply@anthropic.com>"
```

---

## Summary

| Task | Description | Files |
|------|-------------|-------|
| 1 | Create Makefile | `module/bdev/ioperf/Makefile` |
| 2 | Create header file | `module/bdev/ioperf/bdev_ioperf.h` |
| 3 | Create main module | `module/bdev/ioperf/bdev_ioperf.c` |
| 4 | Create RPC handlers | `module/bdev/ioperf/bdev_ioperf_rpc.c` |
| 5 | Build and test | - |

---

**Plan complete and saved to `docs/superpowers/plans/2026-03-23-ioperf-bdev-implementation.md`. Two execution options:**

**1. Subagent-Driven (recommended)** - I dispatch a fresh subagent per task, review between tasks, fast iteration

**2. Inline Execution** - Execute tasks in this session using executing-plans, batch execution with checkpoints

**Which approach?**
