# ioperf Per-Thread 数据结构设计

## 概述

用 per-thread 结构 `ioperf_thread_ctx` 替换现有的 per-channel `ioperf_io_channel`，在模块初始化时收集线程并分配顺序 thread_id。

## 设计目标

1. 在 ioperf 模块初始化时收集 SPDK 线程
2. 给每个线程分配从 0 开始的顺序 thread_id
3. 为每个线程注册 poller 处理 wait queue
4. 支持 O(1) 获取当前线程的 thread_id

## 数据结构

### ioperf_thread_ctx（替代 ioperf_io_channel）

```c
struct ioperf_thread_ctx {
    uint32_t                    thread_id;           /* 从 0 开始顺序 ID */
    struct spdk_thread          *thread;             /* SPDK 线程句柄 */
    TAILQ_HEAD(, ioperf_io_ctx) wait_queue;       /* IO 等待队列 */
    struct spdk_poller       *poller;             /* wait queue poller */
    uint64_t                delay_ticks;        /* 100us 延迟的 tick 数（初始化时计算） */
    TAILQ_ENTRY(ioperf_thread_ctx) link;
};
```

### 全局管理

```c
struct ioperf_thread_mgr {
    struct ioperf_thread_ctx *ctxs;         /* 线程上下文数组 */
    uint32_t                thread_count;     /* 线程数量 */
    _Atomic uint32_t        next_id;        /* 下一个分配的 ID */
};
extern struct ioperf_thread_mgr g_ioperf_thread_mgr;
```

## 初始化流程

### bdev_ioperf_initialize()

```c
static int
bdev_ioperf_initialize(void)
{
    /* 1. 注册 io_device（用于获取 io_channel，但不是必需的） */
    spdk_io_device_register(&g_ioperf_bdev_head, NULL, NULL, 0, "ioperf_bdev");

    /* 2. 初始化线程管理器 */
    g_ioperf_thread_mgr.thread_count = 0;
    g_ioperf_thread_mgr.ctxs = NULL;
    g_ioperf_thread_mgr.next_id = 0;

    /* 3. 收集现有线程并分配 thread_id */
    spdk_for_each_thread(ioperf_collect_thread, &g_ioperf_thread_mgr, NULL);

    /* 4. 初始化 RPC */
    bdev_ioperf_rpc_init();

    SPDK_NOTICELOG("ioperf: initialized with %u threads\n", g_ioperf_thread_mgr.thread_count);

    return 0;
}
```

### ioperf_collect_thread() - 遍历回调

```c
static void
ioperf_collect_thread(void *ctx)
{
    struct ioperf_thread_mgr *mgr = ctx;
    struct spdk_thread *thread = spdk_get_thread();
    struct ioperf_thread_ctx *thread_ctx;

    /* 检查是否已存在 */
    if (spdk_thread_get_private(thread) != NULL) {
        return;
    }

    /* 分配 thread_ctx */
    thread_ctx = calloc(1, sizeof(*thread_ctx));
    if (!thread_ctx) {
        return;
    }

    /* 分配顺序 ID */
    thread_ctx->thread_id = atomic_fetch_add(&mgr->next_id, 1);
    thread_ctx->thread = thread;
    TAILQ_INIT(&thread_ctx->wait_queue);

    /* 注册 poller */
    thread_ctx->poller = spdk_poller_register(ioperf_thread_poll, thread_ctx, 0);

    /* 计算 delay_ticks（100us延迟） */
    thread_ctx->delay_ticks = spdk_get_ticks_hz() / 10000;

    /* 存储到线程私有数据 */
    spdk_thread_set_private(thread, thread_ctx);

    /* 添加到数组（预先分配足够空间更佳） */
    void *new_ptr = realloc(mgr->ctxs, (mgr->thread_count + 1) * sizeof(*mgr->ctxs));
    if (!new_ptr) {
        /* 分配失败，跳过此线程，避免内存泄漏 */
        spdk_poller_unregister(thread_ctx->poller);
        free(thread_ctx);
        return;
    }
    mgr->ctxs = new_ptr;
    mgr->ctxs[mgr->thread_count++] = thread_ctx;
}
```

## Poller 处理

### ioperf_thread_poll()

```c
static int
ioperf_thread_poll(void *ctx)
{
    struct ioperf_thread_ctx *thread_ctx = ctx;
    struct ioperf_io_ctx *io_ctx, *tmp;
    uint64_t now = spdk_get_ticks();

    TAILQ_FOREACH_SAFE(io_ctx, &thread_ctx->wait_queue, link, tmp) {
        if (now - io_ctx->queued_io >= thread_ctx->delay_ticks) {
            TAILQ_REMOVE(&thread_ctx->wait_queue, io_ctx, link);

            /* 模拟硬件延迟 */
            ioperf_reg_access();
            ioperf_mem_barrier();

            /* 完成 IO */
            spdk_bdev_io_complete(io_ctx->bio, SPDK_BDEV_IO_STATUS_SUCCESS);

            /* 更新统计 */
            struct ioperf_bdev *ioperf = (struct ioperf_bdev *)io_ctx->bio->bdev->ctxt;
            ioperf->total_io++;
            ioperf->total_bytes += io_ctx->bio->u.bdev.num_blocks * io_ctx->bio->bdev->blocklen;

            /* 释放 io_ctx */
            spdk_mempool_put(ioperf->io_pool, io_ctx);
        }
    }

    return 0;
}
```

## 获取当前线程 thread_id

```c
static inline uint32_t
ioperf_get_thread_id(void)
{
    struct ioperf_thread_ctx *ctx = spdk_thread_get_private(spdk_get_thread());
    return ctx ? ctx->thread_id : UINT32_MAX;
}
```

## IO 提交流程

1. `submit_request` 计算目标线程
2. 获取目标线程的 `ioperf_thread_ctx`
3. 发送 IO 到目标线程
4. 目标线程将 IO 添加到自己的 wait_queue
5. Poller 检查 wait_queue，处理超时的 IO

## 清理流程

在 `bdev_ioperf_finish()` 中：
- 遍历 `g_ioperf_thread_mgr.ctxs`
- 对于每个 thread_ctx， drain wait_queue 中的 IO：
  - 调用 `spdk_bdev_io_complete(io_ctx->bio, SPDK_BDEV_IO_STATUS_ABORTED)`
  - 释放 io_ctx 到 pool
- 销毁 poller
- 清除线程私有数据 (`spdk_thread_set_private(thread, NULL)`)
- 释放所有 thread_ctx
- 释放数组

**注意**：调用者需确保所有 IO 处理完成后再调用模块 finish，避免 IO 泄露。

## 设计权衡

- 不支持运行时动态添加线程（模块初始化时一次性收集）
- 使用线程私有数据实现 O(1) 访问
- 顺序 thread_id 从 0 开始，便于调试和日志