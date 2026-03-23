/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2024 Intel Corporation.
 *   All rights reserved.
 */

#include "spdk/stdinc.h"

#include "spdk/bdev.h"
#include "spdk/json.h"
#include "spdk/jsonrpc.h"
#include "spdk/rpc.h"
#include "spdk/string.h"

#include "spdk/bdev_module.h"
#include "spdk/log.h"

#include "bdev_ioperf.h"

/* Forward declaration */
int bdev_ioperf_rpc_init(void);

/* RPC request context structure for bdev_ioperf_create */
struct rpc_bdev_ioperf_create_ctx {
	char *name;
	struct spdk_uuid uuid;
	uint64_t num_blocks;
	uint32_t block_size;
	uint32_t physical_block_size;
	uint32_t num_threads;
	uint64_t read_latency_us;
	uint64_t write_latency_us;
	bool enable_validation;
};

static void
free_rpc_bdev_ioperf_create(struct rpc_bdev_ioperf_create_ctx *ctx)
{
	free(ctx->name);
}

static const struct spdk_json_object_decoder rpc_bdev_ioperf_create_decoders[] = {
	{"name", offsetof(struct rpc_bdev_ioperf_create_ctx, name), spdk_json_decode_string},
	{"uuid", offsetof(struct rpc_bdev_ioperf_create_ctx, uuid), spdk_json_decode_uuid, true},
	{"num_blocks", offsetof(struct rpc_bdev_ioperf_create_ctx, num_blocks), spdk_json_decode_uint64, true},
	{"block_size", offsetof(struct rpc_bdev_ioperf_create_ctx, block_size), spdk_json_decode_uint32, true},
	{"physical_block_size", offsetof(struct rpc_bdev_ioperf_create_ctx, physical_block_size), spdk_json_decode_uint32, true},
	{"num_threads", offsetof(struct rpc_bdev_ioperf_create_ctx, num_threads), spdk_json_decode_uint32, true},
	{"read_latency_us", offsetof(struct rpc_bdev_ioperf_create_ctx, read_latency_us), spdk_json_decode_uint64, true},
	{"write_latency_us", offsetof(struct rpc_bdev_ioperf_create_ctx, write_latency_us), spdk_json_decode_uint64, true},
	{"enable_validation", offsetof(struct rpc_bdev_ioperf_create_ctx, enable_validation), spdk_json_decode_bool, true},
};

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

static void
rpc_bdev_ioperf_create(struct spdk_jsonrpc_request *request,
		       const struct spdk_json_val *params)
{
	struct rpc_bdev_ioperf_create_ctx req = {};
	struct ioperf_bdev_opts opts = {};
	struct spdk_json_write_ctx *w;
	struct spdk_bdev *bdev;
	int rc = 0;

	if (spdk_json_decode_object(params, rpc_bdev_ioperf_create_decoders,
				    SPDK_COUNTOF(rpc_bdev_ioperf_create_decoders),
				    &req)) {
		SPDK_ERRLOG("bdev_ioperf_create: failed to decode parameters\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						"Invalid parameters");
		goto cleanup;
	}

	/* Set defaults */
	if (req.num_blocks == 0) {
		req.num_blocks = 131072; /* default 512MB */
	}
	if (req.block_size == 0) {
		req.block_size = 512;
	}
	if (req.physical_block_size == 0) {
		req.physical_block_size = 512;
	}
	if (req.num_threads == 0) {
		req.num_threads = 4;
	}
	if (req.read_latency_us == 0) {
		req.read_latency_us = IOPERF_DEFAULT_READ_LATENCY_US;
	}
	if (req.write_latency_us == 0) {
		req.write_latency_us = IOPERF_DEFAULT_WRITE_LATENCY_US;
	}

	/* Copy to opts */
	opts.name = req.name;
	opts.uuid = req.uuid;
	opts.num_blocks = req.num_blocks;
	opts.block_size = req.block_size;
	opts.physical_block_size = req.physical_block_size;
	opts.num_threads = req.num_threads;
	opts.read_latency_us = req.read_latency_us;
	opts.write_latency_us = req.write_latency_us;
	opts.enable_validation = req.enable_validation;

	rc = bdev_ioperf_create(&bdev, &opts);
	if (rc != 0) {
		spdk_jsonrpc_send_error_response(request, rc, spdk_strerror(-rc));
		goto cleanup;
	}

	w = spdk_jsonrpc_begin_result(request);
	bdev_ioperf_create_json(&opts, w);
	spdk_jsonrpc_end_result(request, w);

cleanup:
	free_rpc_bdev_ioperf_create(&req);
}
SPDK_RPC_REGISTER("bdev_ioperf_create", rpc_bdev_ioperf_create, SPDK_RPC_RUNTIME)

int
bdev_ioperf_rpc_init(void)
{
	/* RPCs are registered via SPDK_RPC_REGISTER macro above */
	return 0;
}
