#!/bin/bash
# Simple ioperf test using bdevperf

NAME="ioperf0"
NUM_THREADS=2
RUNTIME=5

# Generate config
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
            "num_blocks": 131072,
            "block_size": 512,
            "num_threads": $NUM_THREADS,
            "read_latency_us": 100,
            "write_latency_us": 100
          }
        }
      ]
    }
  ]
}
EOF

echo "Testing ioperf bdev with $NUM_THREADS threads for $RUNTIME seconds..."
./test/bdev/bdevperf/bdevperf --json /tmp/ioperf_config.json -T "$NAME" -q 32 -o 4096 -w randread -t $RUNTIME -S 1 --iova-mode va -m 0x3

rm -f /tmp/ioperf_config.json