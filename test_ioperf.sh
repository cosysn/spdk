#!/bin/bash
# ioperf bdev performance test script
# Automatically tests sequential read/write, random read/write with various block sizes and queue depths

set -e

# Configuration
SPDK_DIR="${SPDK_DIR:-/home/ubuntu/spdk}"
BDEV_NAME="ioperf0"
NUM_BLOCKS=131072    # 512MB (512 * 131072 = 256MB)
BLOCK_SIZE=512
TEST_DURATION=10
CPU_MASK="0x3"      # Use 2 cores: core 0 and 1

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m'

# Results
RESULTS_FILE="ioperf_test_$(date +%Y%m%d_%H%M%S).txt"

echo -e "${BLUE}"
echo "=============================================="
echo "   ioperf bdev Performance Test"
echo "=============================================="
echo -e "${NC}"

# ============================================
# Cleanup function
# ============================================
cleanup() {
    echo "Cleaning up..."
    sudo killall -9 spdk_tgt 2>/dev/null || true
    sudo killall -9 bdevperf 2>/dev/null || true
    sleep 1
    sudo rm -f /var/tmp/spdk* /tmp/spdk* 2>/dev/null || true
}

# ============================================
# Setup hugepages
# ============================================
setup_hugepages() {
    echo "Setting up hugepages..."
    HUGEPAGES=$(cat /proc/sys/vm/nr_hugepages 2>/dev/null || echo 0)
    if [ "$HUGEPAGES" -lt 512 ]; then
        echo 512 | sudo tee /proc/sys/vm/nr_hugepages > /dev/null 2>&1 || true
    fi
    echo "Hugepages: $(cat /proc/sys/vm/nr_hugepages)"
}

# ============================================
# Start SPDK target
# ============================================
start_spdk_target() {
    echo "Starting SPDK target..."

    # Create config to enable ioperf bdev
    CONFIG_JSON="/tmp/ioperf_config.json"
    cat > "$CONFIG_JSON" << EOF
{
  "subsystems": [
    {
      "subsystem": "bdev",
      "config": [
        {
          "method": "bdev_ioperf_create",
          "params": {
            "name": "$BDEV_NAME",
            "num_blocks": $NUM_BLOCKS,
            "block_size": $BLOCK_SIZE,
            "num_threads": 4
          }
        }
      ]
    }
  ]
}
EOF

    # Start target in background
    sudo "$SPDK_DIR/build/bin/spdk_tgt" \
        -m $CPU_MASK \
        -s 512 \
        -c "$CONFIG_JSON" \
        > /tmp/spdk_tgt.log 2>&1 &
    SPDK_PID=$!

    # Wait for target to start
    sleep 3

    # Check if running
    if ! kill -0 $SPDK_PID 2>/dev/null; then
        echo -e "${RED}Error: spdk_tgt failed to start${NC}"
        cat /tmp/spdk_tgt.log
        exit 1
    fi

    echo "SPDK target started (PID: $SPDK_PID)"
}

# ============================================
# Run bdevperf test
# ============================================
run_test() {
    local test_name=$1
    local rw_type=$2
    local bs=$3
    local iod=$4

    echo -e "${YELLOW}Testing: $test_name (rw=$rw_type, bs=$bs, iod=$iod)${NC}"

    # Kill previous bdevperf
    sudo killall -9 bdevperf 2>/dev/null || true
    sleep 1

    # Run test
    sudo "$SPDK_DIR/build/examples/bdevperf" \
        -b "$BDEV_NAME" \
        -q $iod \
        -o $bs \
        -w $rw_type \
        -t $TEST_DURATION \
        -m $CPU_MASK \
        -s 256 \
        --no-huge \
        2>&1 | tee /tmp/bdevperf_output.txt

    # Extract results
    local iops bw lat
    iops=$(grep -i "IOPS" /tmp/bdevperf_output.txt | head -1 | awk '{print $NF}' | tr -d ',')
    bw=$(grep -i "MiB/s" /tmp/bdevperf_output.txt | head -1 | awk '{print $NF}' | tr -d ',')
    lat=$(grep -i "latency" /tmp/bdevperf_output.txt | head -1 | awk '{print $NF}' || echo "N/A")

    if [ -z "$iops" ] || [ "$iops" = "0" ]; then
        iops="FAILED"
        bw="FAILED"
    fi

    echo -e "${GREEN}Result: IOPS=$iops, BW=$bw MiB/s, Latency=$lat${NC}"
    echo ""

    # Store
    echo "$test_name|$rw_type|$bs|$iod|$iops|$bw|$lat" >> "$RESULTS_FILE"
}

# ============================================
# Main
# ============================================
main() {
    # Initialize results
    echo "Test|RW_Type|BlockSize|IO_Depth|IOPS|Bandwidth|Latency" > "$RESULTS_FILE"

    cleanup
    setup_hugepages
    start_spdk_target

    # Wait for bdev to be ready
    sleep 2

    echo ""
    echo -e "${GREEN}Starting tests...${NC}"
    echo ""

    # Test configurations
    local tests=(
        "Sequential Read|read|4096|1"
        "Sequential Write|write|4096|1"
        "Random Read|randread|4096|1"
        "Random Write|randwrite|4096|1"
        "Sequential Read QD32|read|4096|32"
        "Sequential Write QD32|write|4096|32"
        "Random Read QD32|randread|4096|32"
        "Random Write QD32|randwrite|4096|32"
        "4K Read QD64|read|4096|64"
        "4K Write QD64|write|4096|64"
        "4K RandRead QD64|randread|4096|64"
        "4K RandWrite QD64|randwrite|4096|64"
    )

    for test in "${tests[@]}"; do
        IFS='|' read -r test_name rw_type bs iod <<< "$test"
        run_test "$test_name" "$rw_type" "$bs" "$iod"
    done

    # Cleanup
    cleanup

    # Print summary
    echo ""
    echo -e "${BLUE}=============================================="
    echo "   Test Results Summary"
    echo "==============================================${NC}"
    column -t -s '|' "$RESULTS_FILE"

    echo ""
    echo -e "${GREEN}Results saved to: $RESULTS_FILE${NC}"
}

main "$@"