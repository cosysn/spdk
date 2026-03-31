#!/bin/bash
# ioperf bdev performance test script

set -e

# Default values
NAME="ioperf0"
NUM_BLOCKS=131072  # 64MB
BLOCK_SIZE=512
NUM_THREADS=1
READ_LATENCY=100   # 100us
WRITE_LATENCY=100  # 100us
IO_SIZE=4096
QUEUE_DEPTH=32
RUNTIME=10
WORKLOAD="randread"
PCI_ADDR=""
RPC_SOCKET="/var/tmp/spdk.sock"

# SPDK build directory
SPDK_DIR="$(cd "$(dirname "$0")/../.." && pwd)"
BUILD_DIR="$SPDK_DIR/build"

usage() {
    echo "Usage: $0 [options]"
    echo "Options:"
    echo "  -n <name>       bdev name (default: ioperf0)"
    echo "  -s <size>       total size in MB (default: 64)"
    echo "  -b <blocksize>  block size in bytes (default: 512)"
    echo "  -t <threads>    number of threads (default: 1)"
    echo "  -r <latency>   read latency in us (default: 100)"
    echo "  -w <latency>   write latency in us (default: 100)"
    echo "  -o <iosize>    IO size in bytes (default: 4096)"
    echo "  -q <qd>        queue depth (default: 32)"
    echo "  -T <time>      runtime in seconds (default: 10)"
    echo "  -W <workload>  workload: randread, randwrite, read, write (default: randread)"
    echo "  -d <pci>       NVMe PCIe address (e.g., 0000:04:00.0)"
    echo "  -h             show this help"
    exit 1
}

while getopts "n:s:b:t:r:w:o:q:T:W:d:h" opt; do
    case $opt in
        n) NAME="$OPTARG";;
        s) NUM_BLOCKS=$((OPTARG * 1024 * 1024 / BLOCK_SIZE));;
        b) BLOCK_SIZE="$OPTARG";;
        t) NUM_THREADS="$OPTARG";;
        r) READ_LATENCY="$OPTARG";;
        w) WRITE_LATENCY="$OPTARG";;
        o) IO_SIZE="$OPTARG";;
        q) QUEUE_DEPTH="$OPTARG";;
        T) RUNTIME="$OPTARG";;
        W) WORKLOAD="$OPTARG";;
        d) PCI_ADDR="$OPTARG";;
        h) usage;;
        *) usage;;
    esac
done

# Generate JSON config file
generate_config() {
    cat > /tmp/ioperf_config.json << EOF
{
  "subsystems": [
    {
      "subsystem": "bdev",
      "config": [
        {
          "method": "bdev_ioperf_create",
          "params": {
            "name": "$NAME",
            "num_blocks": $NUM_BLOCKS,
            "block_size": $BLOCK_SIZE,
            "num_threads": $NUM_THREADS,
            "read_latency_us": $READ_LATENCY,
            "write_latency_us": $WRITE_LATENCY
          }
        }
      ]
    }
  ]
}
EOF
}

# Kill existing spdk_tgt
kill_spdk_tgt() {
    pkill -f "spdk_tgt.*$RPC_SOCKET" 2>/dev/null || true
    sleep 1
}

# Start spdk_tgt with config
start_spdk_tgt() {
    kill_spdk_tgt

    echo "Starting spdk_tgt with ioperf bdev..."
    sudo "$BUILD_DIR/spdk_tgt" -m 0x1 -S "$RPC_SOCKET" -f /tmp/ioperf_config.json &
    sleep 3
}

echo "=== ioperf bdev Performance Test ==="
echo "Name: $NAME"
echo "Size: $((NUM_BLOCKS * BLOCK_SIZE / 1024 / 1024)) MB"
echo "Block size: $BLOCK_SIZE"
echo "Threads: $NUM_THREADS"
echo "Read latency: ${READ_LATENCY}us"
echo "Write latency: ${WRITE_LATENCY}us"
echo "IO size: $IO_SIZE"
echo "Queue depth: $QUEUE_DEPTH"
echo "Runtime: $RUNTIME seconds"
echo "Workload: $WORKLOAD"
echo ""

# Generate config
generate_config

# Start spdk_tgt
start_spdk_tgt

# Determine bdev argument
if [ -n "$PCI_ADDR" ]; then
    BDEV_ARG="-r trtype:PCIe traddr:$PCI_ADDR"
else
    BDEV_ARG="-b $NAME"
fi

# Run bdevperf
echo "Running performance test..."
sudo "$BUILD_DIR/examples/bdevperf" $BDEV_ARG -q "$QUEUE_DEPTH" -o "$IO_SIZE" -w "$WORKLOAD" -t "$RUNTIME" -L

# Cleanup
echo "Cleaning up..."
kill_spdk_tgt
rm -f /tmp/ioperf_config.json

echo "Done!"
