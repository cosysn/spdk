# ioperf bdev Performance Test Guide

## 环境配置

### 1. 分配大页内存
```bash
sudo HUGEMEM=8192 ./scripts/setup.sh
```
根据需要调整大小，8GB适合一般测试。

### 2. 配置CPU核心
使用 `-m` 参数分配CPU核心：
```bash
# 核心7-16的掩码
-m 0x1FF80
```

## 启动spdk_tgt

### 生成配置文件 (config_ioperf.json)
```json
{
  "subsystems": [
    {
      "subsystem": "bdev",
      "config": [
        {
          "method": "bdev_ioperf_create",
          "params": {
            "name": "ioperf0",
            "num_blocks": 131072,
            "block_size": 512,
            "num_threads": 1,
            "read_latency_us": 100,
            "write_latency_us": 100
          }
        }
      ]
    }
  ]
}
```

### 启动命令
```bash
sudo ./build/spdk_tgt -m 0x1FF80 -S /var/tmp/spdk.sock -f config_ioperf.json &
```

## IO测试

### 参数说明
- `-b ioperf0` - bdev名称
- `-q <qd>` - 队列深度
- `-o <size>` - IO大小(字节)
- `-w <workload>` - 负载类型: randread, randwrite, read, write
- `-t <time>` - 运行时间(秒)
- `-L` - 显示延迟统计

### 测试示例

#### 随机读测试
```bash
sudo ./build/examples/bdevperf -b ioperf0 -q 128 -o 4096 -w randread -t 30 -L
```

#### 随机写测试
```bash
sudo ./build/examples/bdevperf -b ioperf0 -q 128 -o 4096 -w randwrite -t 30 -L
```

#### 顺序读测试
```bash
sudo ./build/examples/bdevperf -b ioperf0 -q 128 -o 4096 -w read -t 30 -L
```

#### 顺序写测试
```bash
sudo ./build/examples/bdevperf -b ioperf0 -q 128 -o 4096 -w write -t 30 -L
```

## 使用测试脚本

也可以使用自动化测试脚本：
```bash
sudo ./test/bdev/ioperf_perf.sh -t 4 -r 100 -W randread -q 128 -T 30
```

### 脚本参数
- `-n <name>` - bdev名称
- `-s <size>` - 大小(MB)
- `-b <blocksize>` - 块大小
- `-t <threads>` - 线程数
- `-r <latency>` - 读延迟(us)
- `-w <latency>` - 写延迟(us)
- `-o <iosize>` - IO大小
- `-q <qd>` - 队列深度
- `-T <time>` - 运行时间
- `-W <workload>` - 负载类型
