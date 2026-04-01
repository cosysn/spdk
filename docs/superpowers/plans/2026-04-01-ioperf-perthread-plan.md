# ioperf Per-Thread Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace per-channel ioperf_io_channel with per-thread ioperf_thread_ctx structure, collecting threads at module init and registering per-thread pollers.

**Architecture:** New per-thread design where each SPDK thread gets a context (ioperf_thread_ctx) allocated at module init, with sequential thread_id and poller for wait queue processing. Thread private data provides O(1) thread_id lookup.

**Tech Stack:** SPDK bdev module, C, spdk_thread APIs, spdk_poller APIs

---

## Files to Modify

| File | Responsibility |
|------|----------------|
| `module/bdev/ioperf/bdev_ioperf.h` | Add new data structures |
| `module/bdev/ioperf/bdev_ioperf.c` | Implement all logic changes |

---

## Tasks

### Task 1: Add ioperf_thread_ctx Structure

**Files:**
- Modify: `module/bdev/ioperf/bdev_ioperf.h:52-56`
- Modify: `module/bdev/ioperf/bdev_ioperf.h:96-104` (add new structure after existing ioperf_io_channel)

- [ ] **Step 1: Add ioperf_thread_mgr structure update**

```c
// In bdev_ioperf.h, update ioperf_thread_mgr (around line 52):
struct ioperf_thread_mgr {
    struct ioperf_thread_ctx **ctxs;   // Array of thread contexts
    uint32_t                thread_count;
    _Atomic uint32_t        next_id;
};
extern struct ioperf_thread_mgr g_ioperf_thread_mgr;
```

- [ ] **Step 2: Add ioperf_thread_ctx structure (with rate_limit_queue)**

```c
// After ioperf_io_channel definition (around line 104):
/* Per-thread context structure */
struct ioperf_thread_ctx {
    uint32_t                    thread_id;           /* Sequential ID (0-based) */
    struct spdk_thread         *thread;             /* SPDK thread handle */
    TAILQ_HEAD(, ioperf_io_ctx) wait_queue;       /* IO wait queue (100us delay) */
    TAILQ_HEAD(, ioperf_io_ctx) rate_limit_queue; /* IO rate limit queue */
    struct spdk_poller         *poller;             /* Wait queue poller */
    uint64_t                   delay_ticks;        /* 100us delay in ticks */
    uint64_t                   last_time;           /* Last rate limit check time */
    uint64_t                   token_bucket;       /* Rate limit token bucket */
    TAILQ_ENTRY(ioperf_thread_ctx) link;
};
```

- [ ] **Step 3: Add function declarations**

```c
// Add to bdev_ioperf.h (after line 72):
uint32_t ioperf_get_thread_id(void);
void ioperf_collect_thread(void *ctx);
```

- [ ] **Step 4: Compile to verify**

```bash
cd /home/ubuntu/spdk && make -C module/bdev/ioperf
```
Expected: No errors

- [ ] **Step 5: Commit**

```bash
git add module/bdev/ioperf/bdev_ioperf.h
git commit -m "feat: add ioperf_thread_ctx and ioperf_thread_mgr structures

Co-Authored-By: Claude Opus 4.5 <noreply@anthropic.com>"
```

---

### Task 2: Implement Thread Collection Function

**Files:**
- Modify: `module/bdev/ioperf/bdev_ioperf.c:49-57` (after existing ioperf_register_thread)

- [ ] **Step 1: Add ioperf_collect_thread forward declaration**

```c
// Add after line 46:
static void ioperf_collect_thread(void *ctx);
```

- [ ] **Step 2: Implement ioperf_collect_thread function**

```c
static void
ioperf_collect_thread(void *ctx)
{
    struct ioperf_thread_mgr *mgr = ctx;
    struct spdk_thread *thread = spdk_get_thread();
    struct ioperf_thread_ctx *thread_ctx;

    /* Check if thread already has context */
    if (spdk_thread_get_private(thread) != NULL) {
        return;
    }

    /* Allocate thread context */
    thread_ctx = calloc(1, sizeof(*thread_ctx));
    if (!thread_ctx) {
        SPDK_ERRLOG("Failed to allocate thread context\n");
        return;
    }

    /* Assign sequential thread_id */
    thread_ctx->thread_id = atomic_fetch_add(&mgr->next_id, 1);
    thread_ctx->thread = thread;
    TAILQ_INIT(&thread_ctx->wait_queue);
    TAILQ_INIT(&thread_ctx->rate_limit_queue);
    thread_ctx->last_time = 0;
    thread_ctx->token_bucket = 0;

    /* Register poller */
    thread_ctx->poller = spdk_poller_register(ioperf_thread_poll, thread_ctx, 0);

    /* Calculate delay_ticks for 100us */
    thread_ctx->delay_ticks = spdk_get_ticks_hz() / 10000;

    /* Store in thread private data */
    spdk_thread_set_private(thread, thread_ctx);

    /* Add to array */
    void *new_ptr = realloc(mgr->ctxs, (mgr->thread_count + 1) * sizeof(*mgr->ctxs));
    if (!new_ptr) {
        SPDK_ERRLOG("Failed to expand thread context array\n");
        spdk_poller_unregister(thread_ctx->poller);
        free(thread_ctx);
        return;
    }
    mgr->ctxs = new_ptr;
    mgr->ctxs[mgr->thread_count++] = thread_ctx;
}
```

- [ ] **Step 3: Add ioperf_get_thread_id function**

```c
uint32_t
ioperf_get_thread_id(void)
{
    struct ioperf_thread_ctx *ctx = spdk_thread_get_private(spdk_get_thread());
    return ctx ? ctx->thread_id : UINT32_MAX;
}
```

- [ ] **Step 4: Compile to verify**

```bash
cd /home/ubuntu/spdk && make -C module/bdev/ioperf
```
Expected: No errors

- [ ] **Step 5: Commit**

```bash
git add module/bdev/ioperf/bdev_ioperf.c
git commit -m "feat: add ioperf_collect_thread and ioperf_get_thread_id

Co-Authored-By: Claude Opus 4.5 <noreply@anthropic.com>"
```

---

### Task 3: Implement Per-Thread Poller Handler

**Files:**
- Modify: `module/bdev/ioperf/bdev_ioperf.c` (add new function, around line 219)

- [ ] **Step 1: Add ioperf_thread_poll forward declaration**

```c
// Add after line 45:
static int ioperf_thread_poll(void *ctx);
```

- [ ] **Step 2: Implement ioperf_thread_poll function (with rate_limit_queue processing)**

```c
static int
ioperf_thread_poll(void *ctx)
{
    struct ioperf_thread_ctx *thread_ctx = ctx;
    struct ioperf_io_ctx *io_ctx, *tmp;
    uint64_t now = spdk_get_ticks();

    /* Process wait queue - complete IO that has waited >100us */
    TAILQ_FOREACH_SAFE(io_ctx, &thread_ctx->wait_queue, link, tmp) {
        if (now - io_ctx->queued_io >= thread_ctx->delay_ticks) {
            TAILQ_REMOVE(&thread_ctx->wait_queue, io_ctx, link);

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
    TAILQ_FOREACH_SAFE(rl_ctx, &thread_ctx->rate_limit_queue, link, rl_tmp) {
        struct ioperf_bdev *ioperf = (struct ioperf_bdev *)rl_ctx->bio->bdev->ctxt;
        if (rate_limit_check(thread_ctx, ioperf, rl_ctx)) {
            TAILQ_REMOVE(&thread_ctx->rate_limit_queue, rl_ctx, link);
            /* Retry - send to target thread for processing */
            uint32_t target_thread_idx = ioperf_hash_lba(rl_ctx->bio->u.bdev.offset_blocks, ioperf->num_threads);
            struct spdk_thread *target_thread;
            if (ioperf->thread_pool && ioperf->thread_pool_size > 0) {
                target_thread = ioperf->thread_pool[target_thread_idx % ioperf->thread_pool_size];
            } else {
                target_thread = rl_ctx->src_thread;
            }
            rl_ctx->target_thread = target_thread_idx;
            spdk_thread_send_msg(target_thread, ioperf_process_io_on_target, rl_ctx);
        }
    }

    return 0;
}
```

- [ ] **Step 3: Compile to verify**

```bash
cd /home/ubuntu/spdk && make -C module/bdev/ioperf
```
Expected: No errors

- [ ] **Step 4: Commit**

```bash
git add module/bdev/ioperf/bdev_ioperf.c
git commit -m "feat: add ioperf_thread_poll per-thread wait queue poller

Co-Authored-By: Claude Opus 4.5 <noreply@anthropic.com>"
```

---

### Task 4: Update bdev_ioperf_initialize

**Files:**
- Modify: `module/bdev/ioperf/bdev_ioperf.c:840-856`

- [ ] **Step 1: Modify bdev_ioperf_initialize to collect threads**

```c
// Replace bdev_ioperf_initialize function:
static int
bdev_ioperf_initialize(void)
{
    /* Register the io_device */
    spdk_io_device_register(&g_ioperf_bdev_head, ioperf_bdev_create_cb, ioperf_bdev_destroy_cb,
                            sizeof(struct ioperf_io_channel), "ioperf_bdev");

    /* Initialize thread manager */
    g_ioperf_thread_mgr.ctxs = NULL;
    g_ioperf_thread_mgr.thread_count = 0;
    atomic_store(&g_ioperf_thread_mgr.next_id, 0);

    /* Collect existing threads and assign thread_ids */
    spdk_for_each_thread(ioperf_collect_thread, &g_ioperf_thread_mgr, NULL);

    /* Initialize RPC handlers */
    bdev_ioperf_rpc_init();

    SPDK_NOTICELOG("ioperf: initialized with %u threads\n", g_ioperf_thread_mgr.thread_count);

    return 0;
}
```

- [ ] **Step 2: Compile to verify**

```bash
cd /home/ubuntu/spdk && make -C module/bdev/ioperf
```
Expected: No errors

- [ ] **Step 3: Commit**

```bash
git add module/bdev/ioperf/bdev_ioperf.c
git commit -m "feat: update bdev_ioperf_initialize to collect threads at module init

Co-Authored-By: Claude Opus 4.5 <noreply@anthropic.com>"
```

---

### Task 5: Update bdev_ioperf_finish

**Files:**
- Modify: `module/bdev/ioperf/bdev_ioperf.c:858-862`

- [ ] **Step 1: Modify bdev_ioperf_finish to cleanup thread contexts**

```c
// Replace bdev_ioperf_finish function:
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
            spdk_poller_unregister(thread_ctx->poller);
        }

        /* Clear thread private data */
        spdk_thread_set_private(thread_ctx->thread, NULL);

        /* Free thread context */
        free(thread_ctx);
    }

    /* Free array and reset manager */
    free(g_ioperf_thread_mgr.ctxs);
    g_ioperf_thread_mgr.ctxs = NULL;
    g_ioperf_thread_mgr.thread_count = 0;
}
```

- [ ] **Step 2: Compile to verify**

```bash
cd /home/ubuntu/spdk && make -C module/bdev/ioperf
```
Expected: No errors

- [ ] **Step 3: Commit**

```bash
git add module/bdev/ioperf/bdev_ioperf.c
git commit -m "feat: add thread context cleanup in bdev_ioperf_finish

Co-Authored-By: Claude Opus 4.5 <noreply@anthropic.com>"
```

---

### Task 6: Update IO Processing to Use Per-Thread Context

**Files:**
- Modify: `module/bdev/ioperf/bdev_ioperf.c:268-300` (ioperf_process_io_on_target)

- [ ] **Step 1: Modify ioperf_process_io_on_target to use thread context**

```c
// Replace ioperf_process_io_on_target function:
static void
ioperf_process_io_on_target(void *ctx)
{
    struct ioperf_io_ctx *io_ctx = (struct ioperf_io_ctx *)ctx;
    struct ioperf_thread_ctx *thread_ctx;
    struct ioperf_io_ctx *wait_ctx, *tmp;
    uint64_t now = spdk_get_ticks();

    /* Get current thread's context */
    thread_ctx = spdk_thread_get_private(spdk_get_thread());
    if (!thread_ctx) {
        /* Should not happen, but handle gracefully */
        spdk_bdev_io_complete(io_ctx->bio, SPDK_BDEV_IO_STATUS_FAILED);
        struct ioperf_bdev *ioperf = (struct ioperf_bdev *)io_ctx->bio->bdev->ctxt;
        spdk_mempool_put(ioperf->io_pool, io_ctx);
        return;
    }

    /* Check wait queue - process IO that has been waiting >100us */
    TAILQ_FOREACH_SAFE(wait_ctx, &thread_ctx->wait_queue, link, tmp) {
        if (now - wait_ctx->queued_io >= thread_ctx->delay_ticks) {
            TAILQ_REMOVE(&thread_ctx->wait_queue, wait_ctx, link);
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
    TAILQ_INSERT_TAIL(&thread_ctx->wait_queue, io_ctx, link);
}
```

- [ ] **Step 2: Compile to verify**

```bash
cd /home/ubuntu/spdk && make -C module/bdev/ioperf
```
Expected: No errors

- [ ] **Step 3: Commit**

```bash
git add module/bdev/ioperf/bdev_ioperf.c
git commit -m "feat: update ioperf_process_io_on_target to use per-thread context

Co-Authored-By: Claude Opus 4.5 <noreply@anthropic.com>"
```

---

### Task 7: Remove Legacy ioperf_io_channel Usage (Optional)

**Files:**
- Modify: `module/bdev/ioperf/bdev_ioperf.h` (comment out or remove)
- Modify: `module/bdev/ioperf/bdev_ioperf.c` (comment out or remove)

- [ ] **Step 1: Consider whether to remove ioperf_io_channel**

This is optional - the existing ioperf_io_channel can coexist with the new per-thread design for backward compatibility. The key changes are complete in Tasks 1-6.

If removing:
1. Comment out `struct ioperf_io_channel` definition in bdev_ioperf.h
2. Comment out `ioperf_bdev_create_cb` and `ioperf_bdev_destroy_cb` in bdev_ioperf.c
3. Update `spdk_io_device_register` to pass 0 size if not using channel

- [ ] **Step 2: Commit if making changes**

```bash
git add module/bdev/ioperf/bdev_ioperf.h module/bdev/ioperf/bdev_ioperf.c
git commit -m "chore: optionally remove legacy ioperf_io_channel usage

Co-Authored-By: Claude Opus 4.5 <noreply@anthropic.com>"
```

---

## Dependencies

```
Task 1 (Structures) → Task 2 (Collection) → Task 3 (Poller) → Task 4 (Init) → Task 5 (Finish) → Task 6 (IO Processing)
```

All tasks build on previous ones. Complete in order.

## Testing

- Build the module: `make -C module/bdev/ioperf`
- Full SPDK build: `./build.sh`
- Run existing ioperf tests if available

---

**Plan complete and saved to `docs/superpowers/plans/2026-04-01-ioperf-perthread-plan.md`. Two execution options:**

**1. Subagent-Driven (recommended)** - I dispatch a fresh subagent per task, review between tasks, fast iteration

**2. Inline Execution** - Execute tasks in this session using executing-plans, batch execution with checkpoints

**Which approach?**