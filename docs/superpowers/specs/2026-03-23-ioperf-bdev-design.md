# ioperf bdev 模块设计

## 1. 概述

创建一个独立的内存模拟 bdev 后端模块，提供可配置的延迟模拟、IO 路由和全局速率限制功能。

### 1.1 背景

用于模拟块设备行为，测试存储性能，评估 IO 路径中的内存访问开销。

### 1.2 目标

- 创建独立 bdev，不落盘，纯内存模拟
- 支持可配置读写延迟
- 支持 IO DIF 数据校验
- 支持 LBA hash 路由到多线程
- 支持 100+ 字段的 IO 上下文赋值（模拟内存访问）
- 支持全局 IOPS 和带宽限制
- 支持每线程等待队列

## 2. 架构设计

### 2.1 IO 路径

```
fio → spdk_nvme → nvme_bdev → ioperf_bdev → memory (no persistence)
```

### 2.2 核心组件

| 组件 | 描述 |
|------|------|
| `ioperf_bdev` | 独立的块设备，继承 spdk_bdev |
| `ioperf_io_channel` | 每个线程一个 channel，包含等待队列 |
| `ioperf_io_ctx` | 自定义 IO 上下文（100+ 字段） |
| `ioperf_stats` | 全局统计（原子变量） |

## 3. 数据结构

### 3.1 ioperf_bdev

```c
struct ioperf_bdev {
    struct spdk_bdev           bdev;
    TAILQ_ENTRY(ioperf_bdev)   tailq;

    /* 配置参数 */
    uint32_t                   num_threads;          /* IO 路由线程数 */
    uint64_t                   read_latency_us;      /* 读延迟（微秒） */
    uint64_t                   write_latency_us;     /* 写延迟（微秒） */
    uint64_t                   max_iops;             /* 全局最大 IOPS */
    uint64_t                   max_bandwidth_mb;     /* 全局最大带宽 (MB/s) */
    bool                       enable_validation;   /* 数据校验 */

    /* 全局统计（原子变量） */
    SPDK_ATOMIC(uint64_t)      total_io;
    SPDK_ATOMIC(uint64_t)      total_bytes;
};
```

### 3.2 ioperf_io_channel

```c
struct ioperf_io_channel {
    struct spdk_poller              *poller;
    TAILQ_HEAD(, ioperf_io_ctx)     wait_queue;       /* 等待队列 */
    uint64_t                         queued_io;        /* 队列中的 IO 数 */
    uint64_t                        last_time;        /* 用于速率计算 */
    uint64_t                        token_bucket;     /* 令牌桶 */
    uint32_t                        thread_id;        /* 线程 ID */
};
```

### 3.3 ioperf_io_ctx（100+ 字段）

```c
struct ioperf_io_ctx {
    TAILQ_ENTRY(ioperf_io_ctx)  link;
    struct spdk_bdev_io        *bio;           /* 原始 bdev IO */

    /* 100+ 字段（用于模拟内存访问） */
    uint64_t                   field_001;
    uint64_t                   field_002;
    uint64_t                   field_003;
    /* ... 共 100+ 个字段 ... */
    uint64_t                   field_128;

    /* 路由信息 */
    uint32_t                   target_thread;
};
```

## 4. 核心流程

### 4.1 IO 提交路径（免锁）

```c
static void
ioperf_submit_request(struct spdk_io_channel *ch, struct spdk_bdev_io *bdev_io)
{
    struct ioperf_io_ctx *ctx = (struct ioperf_io_ctx *)bdev_io->driver_ctx;
    struct ioperf_bdev *ioperf = ioperf_get_bdev();

    /* 1. LBA hash 路由到目标线程 */
    uint64_t lba = bdev_io->u.bdev.offset_blocks;
    uint32_t target_thread = (lba * 2654435761ULL) % ioperf->num_threads;

    /* 2. 获取目标线程的 channel */
    struct ioperf_io_channel *target_ch = get_thread_channel(target_thread);

    /* 3. 检查速率限制 */
    if (!rate_limit_check(target_ch, bdev_io)) {
        /* 加入等待队列 */
        TAILQ_INSERT_TAIL(&target_ch->wait_queue, ctx, link);
        target_ch->queued_io++;
        return;
    }

    /* 4. 赋值 100+ 字段 */
    fill_all_fields(ctx);

    /* 5. DIF 数据校验 */
    if (ioperf->enable_validation) {
        if (bdev_io->type == SPDK_BDEV_IO_TYPE_WRITE) {
            spdk_dif_generate(...);
        } else if (bdev_io->type == SPDK_BDEV_IO_TYPE_READ) {
            spdk_dif_verify(...);
        }
    }

    /* 6. 模拟延迟 */
    spdk_timer_submit(ioperf->timer, ctx, ioperf->latency_us);

    /* 7. 更新统计 */
    spdk_atomic_fetch_add(&ioperf->total_io, 1);
}
```

### 4.2 速率限制（令牌桶 + 每线程队列）

```c
static bool
rate_limit_check(struct ioperf_io_channel *ch, struct spdk_bdev_io *bdev_io)
{
    struct ioperf_bdev *ioperf = ioperf_get_bdev();
    uint64_t now = spdk_get_ticks();
    uint64_t elapsed = now - ch->last_time;

    /* 补充令牌 */
    uint64_t tokens_per_tick = ioperf->max_bandwidth_mb * 1024 * 1024 / spdk_get_ticks_hz();
    ch->token_bucket += elapsed * tokens_per_tick;
    ch->last_time = now;

    /* 检查 IOPS 和带宽 */
    uint64_t io_size = bdev_io->u.bdev.num_blocks * bdev_io->bdev->blocklen;

    if (ch->queued_io < MAX_QUEUED_IO && ch->token_bucket >= io_size) {
        ch->token_bucket -= io_size;
        return true;
    }
    return false;
}
```

### 4.3 等待队列处理（poller）

```c
static int
ioperf_poll(void *arg)
{
    struct ioperf_io_channel *ch = arg;
    struct ioperf_io_ctx *ctx;
    struct ioperf_bdev *ioperf = ioperf_get_bdev();

    /* 检查是否有可用令牌 */
    while (!TAILQ_EMPTY(&ch->wait_queue)) {
        ctx = TAILQ_FIRST(&ch->wait_queue);
        uint64_t io_size = ctx->bio->u.bdev.num_blocks * ctx->bio->bdev->blocklen;

        if (ch->token_bucket >= io_size) {
            TAILQ_REMOVE(&ch->wait_queue, ctx, link);
            ch->queued_io--;
            ch->token_bucket -= io_size;

            /* 赋值字段并完成 IO */
            fill_all_fields(ctx);
            spdk_bdev_io_complete(ctx->bio, SPDK_BDEV_IO_STATUS_SUCCESS);
        } else {
            break;
        }
    }
    return 0;
}
```

### 4.4 LBA Hash 路由

```c
static uint32_t
ioperf_hash_lba(uint64_t lba, uint32_t num_threads)
{
    /* 简单 hash: (lba * prime) % num_threads
     * prime = 2654435761 (Knuth's golden ratio)
     */
    return (lba * 2654435761ULL) % num_threads;
}
```

### 4.5 100+ 字段赋值

```c
static void
fill_all_fields(struct ioperf_io_ctx *ctx)
{
    /* 每个字段都赋值，模拟内存访问 */
    ctx->field_001 = ctx->bio->u.bdev.offset_blocks;
    ctx->field_002 = ctx->bio->u.bdev.num_blocks;
    ctx->field_003 = ctx->bio->bdev->blocklen;
    /* ... 赋值全部 100+ 字段 ... */
    ctx->field_128 = spdk_get_ticks();
}
```

## 5. 配置方式

### 5.1 编译时宏定义

```c
// bdev_ioperf.h
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
```

### 5.2 RPC 创建

```bash
# 创建 ioperf bdev
rpc.py bdev_ioperf_create -b ioperf0 -n 16384 -s 4096 -t 4 -r 100 -w 50 -v

# 参数说明：
# -b, --name        bdev 名称
# -n, --num-blocks  块数量
# -s, --block-size  块大小
# -t, --num-threads 路由线程数
# -r, --read-latency-us  读延迟（微秒）
# -w, --write-latency-us 写延迟（微秒）
# -v, --enable-validation 启用数据校验
```

## 6. 文件结构

```
module/bdev/ioperf/
├── bdev_ioperf.h          # 头文件（结构体定义、宏定义）
├── bdev_ioperf.c          # 主模块实现
├── bdev_ioperf_rpc.c      # RPC 处理
└── Makefile               # 构建文件
```

## 7. 模块注册

```c
static struct spdk_bdev_module ioperf_if = {
    .name = "ioperf",
    .module_init = ioperf_init,
    .module_fini = ioperf_finish,
    .config_json = ioperf_config_json,
    .async_fini = true,
    .get_ctx_size = ioperf_get_ctx_size,
};

SPDK_BDEV_MODULE_REGISTER(ioperf, &ioperf_if)
```

## 8. 依赖

- `spdk/bdev.h`
- `spdk/bdev_module.h`
- `spdk/env.h`
- `spdk/thread.h`
- `spdk/json.h`
- `spdk/timer.h`（延迟模拟）
- `spdk/dif.h`（数据校验）

## 9. 实现步骤

1. 创建 `module/bdev/ioperf/` 目录
2. 编写 `Makefile`
3. 编写 `bdev_ioperf.h`（结构体和宏定义）
4. 编写 `bdev_ioperf.c`（主模块实现）
5. 编写 `bdev_ioperf_rpc.c`（RPC 接口）
6. 添加到构建系统
7. 编译测试
