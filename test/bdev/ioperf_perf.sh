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

# RPC socket
RPC_SOCKET="/var/tmp/spdk.sock"

usage() {
    echo "Usage: $0 [options]"
    echo "Options:"
    echo "  -n <name>       bdev name (default: ioperf0)"
    echo "  -s <size>       total size in MB (default: 64)"
    echo "  -b <blocksize>  block size in bytes (default: 512)"
    echo "  -t <threads>    number of threads (default: 1)"
    echo "  -r <latency>    read latency in us (default: 100)"
    echo "  -w <latency>    write latency in us (default: 100)"
    echo "  -o <iosize>     IO size in bytes (default: 4096)"
    echo "  -q <qd>         queue depth (default: 32)"
    echo "  -T <time>       runtime in seconds (default: 10)"
    echo "  -W <workload>   workload: randread, randwrite, read, write (default: randread)"
    echo "  -h              show this help"
    exit 1
}

while getopts "n:s:b:t:r:w:o:q:T:W:h" opt; do
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
        h) usage;;
        *) usage;;
    esac
done

# Function to send RPC using rpc.py
rpc() {
    local cmd="$1"
    if [ -S "$RPC_SOCKET" ]; then
        python3 scripts/rpc.py -s "$RPC_SOCKET" "$cmd" || true
    else
        echo "Error: RPC socket $RPC_SOCKET not found"
        exit 1
    fi
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

# Start spdk_tgt if not running
if [ ! -S "$RPC_SOCKET" ]; then
    echo "Starting spdk_tgt..."
    sudo ./build/spdk_tgt -m 0x1 -S /var/tmp &
    sleep 2
fi

# Construct ioperf bdev
echo "Constructing ioperf bdev..."
rpc "{\"method\": \"bdev_ioperf_create\", \"params\": {\"name\": \"$NAME\", \"num_blocks\": $NUM_BLOCKS, \"block_size\": $BLOCK_SIZE, \"num_threads\": $NUM_THREADS, \"read_latency_us\": $READ_LATENCY, \"write_latency_us\": $WRITE_LATENCY}}"

# Get bdev name
BDEV_NAME="$NAME"

# Run bdevperf
echo ""
echo "Running performance test..."
./build/examples/bdevperf -r trtype:PCIe traddr:0000:00:04.0 -b "$BDEV_NAME" -q "$QUEUE_DEPTH" -o "$IO_SIZE" -w "$WORKLOAD" -t "$RUNTIME" -L

# Delete ioperf bdev
echo ""
echo "Cleaning up..."
rpc "{\"method\": \"bdev_delete\", \"params\": {\"name\": \"$BDEV_NAME\"}}"

echo "Done!"
