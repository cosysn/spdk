# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

SPDK (Storage Performance Development Kit) is a set of tools and libraries for writing high-performance, scalable, user-mode storage applications. It achieves high performance by moving drivers into userspace and using polled mode instead of interrupts.

## Build Commands

```bash
# Install dependencies
./scripts/pkgdep.sh

# Configure and build
./configure
make

# Build with specific options
./configure --with-rdma           # Enable RDMA support
./configure --with-shared         # Build shared libraries
make CONFIG_DEBUG=y               # Build with debug logging

# Build FreeBSD (uses gmake instead of make)
./configure
gmake

# Run unit tests
./test/unit/unittest.sh

# Run integration tests (requires test configuration file)
sudo ./autotest.sh <test_config>
```

## Running Individual Unit Tests

Unit tests are located in `test/unit/` and are built during `make`. After building, you can run individual tests directly:

```bash
# Example: Run a specific unit test
./test/unit/lib/bdev/bdev.c/bdev_ut

# Example: Run bdev unit tests with valgrind
valgrind ./test/unit/lib/bdev/bdev.c/bdev_ut
```

## Setup Before Running SPDK Applications

SPDK requires hugepages and needs to unbind NVMe devices from kernel drivers:

```bash
sudo scripts/setup.sh              # Default setup
sudo HUGEMEM=8192 scripts/setup.sh # Configure specific memory size
```

## Key Architecture

### Core Libraries (`lib/`)

- **nvme** - NVMe driver (PCIe, TCP, RDMA transports)
- **nvmf** - NVMe over Fabrics target
- **bdev** - Block device layer (RAID, partition, GPT, logical volumes)
- **blob** - Blobstore (persistent object storage)
- **vhost** - vhost-scsi/vhost-blk target for QEMU/KVM
- **iscsi** - iSCSI target
- **virtio** - Virtio-SCSI driver
- **thread** - Threading and polling infrastructure
- **event** - Event framework and reactor
- **accel** - Acceleration framework (crypto, compression)
- **ftl** - Flash Translation Layer
- **jsonrpc** - JSON RPC interface for SPDK applications
- **sock** - Socket abstraction layer
- **rdma_provider** - RDMA abstraction

### Module System (`module/`)

SPDK uses a modular architecture where bdev, sock, and other components can be swapped:
- `module/bdev/` - Bdev plugins (ACPI, lvol, malloc, null, nvme, raid, etc.)
- `module/sock/` - Socket implementations (posix, vhost_user)
- `module/accel/` - Acceleration plugins
- `module/env_dpdk/` - DPDK environment implementation

### Test Structure (`test/`)

- `test/unit/` - Unit tests (C unit tests)
- `test/app/` - Application-level tests
- `test/nvmf/` - NVMe over Fabrics tests
- `test/bdev/` - Block device tests
- `test/common/` - Test utilities (autotest_common.sh)

### Applications (`app/`)

- `app/spdk_tgt/` - Combined target (NVMf, iSCSI, vhost)
- `app/nvmf_tgt/` - NVMe over Fabrics target
- `app/iscsi_tgt/` - iSCSI target
- `app/vhost/` - vhost target
- `app/fio/` - FIO plugin for SPDK

## Key Files

- `configure` - Configuration script
- `CONFIG` - Build configuration options
- `scripts/setup.sh` - Hugepages and device binding setup
- `scripts/pkgdep.sh` - Dependency installer
- `mk/config.mk` - Generated build config (created by configure)

## Python Bindings

Python bindings are in `python/` directory and built automatically. Install via pip:
```bash
pip install spdk
```

## Environment Variables

Common environment variables used in SPDK:
- `HUGEMEM` - Hugepage memory size in MB
- `PCI_BLOCKED` - Comma-separated list of PCI addresses to skip
- `LD_LIBRARY_PATH` - Path to SPDK/DPDK shared libraries

## DPDK Dependency

SPDK depends on DPDK for memory management and networking. DPDK is included as a submodule in `dpdk/`. Use `./scripts/pkgdep.sh` to fetch/build required dependencies.
