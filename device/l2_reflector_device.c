/*
 * Copyright (c) 2022 NVIDIA CORPORATION AND AFFILIATES.  All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without modification, are permitted
 * provided that the following conditions are met:
 *     * Redistributions of source code must retain the above copyright notice, this list of
 *       conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright notice, this list of
 *       conditions and the following disclaimer in the documentation and/or other materials
 *       provided with the distribution.
 *     * Neither the name of the NVIDIA CORPORATION nor the names of its contributors may be used
 *       to endorse or promote products derived from this software without specific prior written
 *       permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND
 * FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL NVIDIA CORPORATION BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS;
 * OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
 * STRICT LIABILITY, OR TOR (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 */

#include <stddef.h>

#include <libflexio-libc/stdio.h>
#include <libflexio-libc/string.h>
#include <libflexio-dev/flexio_dev.h>
#include <libflexio-dev/flexio_dev_err.h>
#include <libflexio-dev/flexio_dev_queue_access.h>
#include <dpaintrin.h>

#include "../common/l2_reflector_common.h"



#define NB_MAC_ADDRESS_BYTES 6
#define NB_UDP_PORT_BYTES 2
#define MSS 0
#define CHECKSUM 0
#define BATCH_SIZE 8	// BATCH_SIZE 必须是SQ CQ entry数的因数



flexio_dev_rpc_handler_t l2_reflector_dev_init;	       /* Device initialization function */
flexio_dev_event_handler_t l2_reflector_event_handler; /* Event handler function */

/* CQ Context */
struct cq_ctx_t {
	uint32_t cq_number;		  /* CQ number */
	struct flexio_dev_cqe64 *cq_ring; /* CQEs buffer */
	struct flexio_dev_cqe64 *cqe;	  /* Current CQE */
	uint32_t cq_idx;		  /* Current CQE IDX */
	uint8_t cq_hw_owner_bit;	  /* HW/SW ownership */
	uint32_t *cq_dbr;		  /* CQ doorbell record */
};

/* RQ Context */
struct rq_ctx_t {
	uint32_t rq_number;			     /* RQ number */
	struct flexio_dev_wqe_rcv_data_seg *rq_ring; /* WQEs buffer */
	uint32_t *rq_dbr;			     /* RQ doorbell record */
};

/* SQ Context */
struct sq_ctx_t {
	uint32_t sq_number;		   /* SQ number */
	uint32_t sq_wqe_seg_idx;	   /* WQE segment index */
	union flexio_dev_sqe_seg *sq_ring; /* SQEs buffer */
	uint32_t *sq_dbr;		   /* SQ doorbell record */
	uint32_t sq_pi;			   /* SQ producer index */
};

/* SQ data buffer */
// struct dt_ctx_t {
// 	void *sq_tx_buff;     /* SQ TX buffer */
// 	uint32_t tx_buff_idx; /* TX buffer index */
// };

struct host_rq_ctx_t {
    uint32_t rkey;            /* receive memory key, used for receive queue */
    uint32_t rq_window_id;   // often same as sq_window_id;

    void *host_rx_buff;
    flexio_uintptr_t dpa_rx_buff;
};

/* Device context */
struct device_context{
	uint32_t lkey;		  /* Local memory key */
	uint32_t is_initialized;  /* Initialization flag */
	struct cq_ctx_t rqcq_ctx; /* RQ CQ context */
	struct cq_ctx_t sqcq_ctx; /* SQ CQ context */
	struct rq_ctx_t rq_ctx;	  /* RQ context */
	struct sq_ctx_t sq_ctx;	  /* SQ context */
	struct host_rq_ctx_t host_rq_ctx;
	// struct dt_ctx_t dt_ctx;	  /* DT context */
	uint32_t packets_count;	  /* Number of processed packets */
};

static struct device_context dev_ctxs[MAX_THREADS] = {0};

// struct flexio_dev_wqe_rcv_data_seg *buffer_ring_base_addr=dev_ctx.rq_ctx.rq_ring;
// int ptr_head_of_tx=0;

/*
 * Initialize the CQ context
 *
 * @app_cq [in]: CQ HW context
 * @ctx [out]: CQ context
 */
static void init_cq(const struct app_transfer_cq app_cq, struct cq_ctx_t *ctx)
{
	ctx->cq_number = app_cq.cq_num;
	ctx->cq_ring = (struct flexio_dev_cqe64 *)app_cq.cq_ring_daddr;
	ctx->cq_dbr = (uint32_t *)app_cq.cq_dbr_daddr;

	ctx->cqe = ctx->cq_ring; /* Points to the first CQE */
	ctx->cq_idx = 0;
	ctx->cq_hw_owner_bit = 0x1;
}

/*
 * Initialize the RQ context
 *
 * @app_rq [in]: RQ HW context
 * @ctx [out]: RQ context
 */
static void init_rq(const struct app_transfer_wq app_rq, struct rq_ctx_t *ctx)
{
	ctx->rq_number = app_rq.wq_num;
	ctx->rq_ring = (struct flexio_dev_wqe_rcv_data_seg *)app_rq.wq_ring_daddr;
	ctx->rq_dbr = (uint32_t *)app_rq.wq_dbr_daddr;
}

/*
 * Initialize the SQ context
 *
 * @app_sq [in]: SQ HW context
 * @ctx [out]: SQ context
 */
static void init_sq(const struct app_transfer_wq app_sq, struct sq_ctx_t *ctx)
{
	ctx->sq_number = app_sq.wq_num;
	ctx->sq_ring = (union flexio_dev_sqe_seg *)app_sq.wq_ring_daddr;
	ctx->sq_dbr = (uint32_t *)app_sq.wq_dbr_daddr;

	ctx->sq_wqe_seg_idx = 0;
	ctx->sq_dbr++;
}

/*
 * Get next data buffer entry
 *
 * @dt_ctx [in]: Data transfer context
 * @dt_idx_mask [in]: Data transfer segment index mask
 * @log_dt_entry_sz [in]: Log of data transfer entry size
 * @return: Data buffer entry
 */
// static void *get_next_dte(struct dt_ctx_t *dt_ctx, uint32_t dt_idx_mask, uint32_t log_dt_entry_sz)
// {
// 	uint32_t mask = ((dt_ctx->tx_buff_idx++ & dt_idx_mask) << log_dt_entry_sz);
// 	char *buff_p = (char *)dt_ctx->sq_tx_buff;

// 	return buff_p + mask;
// }
























/*
 * Get next SQE from the SQ ring
 *
 * @sq_ctx [in]: SQ context
 * @sq_idx_mask [in]: SQ index mask
 * @return: pointer to next SQE
 */
static inline void *get_next_sqe(struct sq_ctx_t *sq_ctx, uint32_t sq_idx_mask)
{
	return &sq_ctx->sq_ring[sq_ctx->sq_wqe_seg_idx++ & sq_idx_mask];
}

/*
 * Increase consumer index of the CQ,
 * Once a CQE is polled, the consumer index is increased.
 * Upon completing a CQ epoch, the HW owner bit is flipped.
 *
 * @cq_ctx [in]: CQ context
 * @cq_idx_mask [in]: CQ index mask which indicates when the CQ is full
 */

// uint32_t ntohl32(uint32_t netlong) {
//     return ((netlong & 0xFF000000) >> 24) |
//            ((netlong & 0x00FF0000) >> 8)  |
//            ((netlong & 0x0000FF00) << 8)  |
//            ((netlong & 0x000000FF) << 24);
// }

// uint64_t ntohl64(uint64_t netlonglong) {
//     return ((netlonglong & 0xFF00000000000000ULL) >> 56) |
//            ((netlonglong & 0x00FF000000000000ULL) >> 40) |
//            ((netlonglong & 0x0000FF0000000000ULL) >> 24) |
//            ((netlonglong & 0x000000FF00000000ULL) >> 8)  |
//            ((netlonglong & 0x00000000FF000000ULL) << 8)  |
//            ((netlonglong & 0x0000000000FF0000ULL) << 24) |
//            ((netlonglong & 0x000000000000FF00ULL) << 40) |
//            ((netlonglong & 0x00000000000000FFULL) << 56);
// }

/*
 * Called by host to initialize the device context
 *
 * @data [in]: pointer to the device context from the host
 * @return: This function always returns 0
 */

flexio_uintptr_t get_host_buffer_with_dtctx(struct flexio_dev_thread_ctx *dtctx, uint32_t window_id, uint32_t mkey, void *haddr)
{
	flexio_uintptr_t host_buffer = 0;
	
	if(flexio_dev_window_config(dtctx, (uint16_t)window_id, mkey))
	{
		flexio_dev_print("DEVICE:: Window config failed!!!\n");
	}

	if(flexio_dev_window_ptr_acquire(dtctx, (uint64_t)haddr, &host_buffer))
	{
		flexio_dev_print("DEVICE:: Cannot obtain window ptr!!!\n");
	}

	return host_buffer;
}

inline static void init_send_sq(struct sq_ctx_t *sq_ctx, uint32_t entry_size, uint32_t lkey) {
    for (uint32_t i = 0; i < entry_size; i++) {
        union flexio_dev_sqe_seg *swqe;
        swqe = get_next_sqe(sq_ctx, L2_SQ_IDX_MASK);
        flexio_dev_swqe_seg_ctrl_set(swqe, i, sq_ctx->sq_number,
            MLX5_CTRL_SEG_CE_CQE_ON_CQE_ERROR, FLEXIO_CTRL_SEG_SEND_EN);

        swqe = get_next_sqe(sq_ctx, L2_SQ_IDX_MASK);
        flexio_dev_swqe_seg_eth_set(swqe, 0, 0, 0, NULL);

        swqe = get_next_sqe(sq_ctx, L2_SQ_IDX_MASK);
        flexio_dev_swqe_seg_mem_ptr_data_set(swqe, 0, lkey, 0);

        swqe = get_next_sqe(sq_ctx, L2_SQ_IDX_MASK);
    }
    sq_ctx->sq_wqe_seg_idx = 0;
}

__dpa_rpc__ uint64_t l2_reflector_device_init(uint64_t data)
{
	struct flexio_dev_thread_ctx *dtctx;
	flexio_dev_get_thread_ctx(&dtctx);

	struct l2_reflector_data *shared_data = (struct l2_reflector_data *)data;
	struct device_context *dev_ctx = &dev_ctxs[shared_data->idx];

	dev_ctx->lkey = shared_data->sq_data.wqd_mkey_id;
	dev_ctx->host_rq_ctx.rkey=shared_data->rq_data.wqd_mkey_id;
	////////
	dev_ctx->host_rq_ctx.rq_window_id=shared_data->flexio_window_id;
	dev_ctx->host_rq_ctx.host_rx_buff=(void *)shared_data->rq_data.wqd_daddr;
	// memset((void *)shared_data->rq_data.wqd_daddr, 0, 128*64);
	
	init_cq(shared_data->rq_cq_data, &dev_ctx->rqcq_ctx);
	init_rq(shared_data->rq_data, &dev_ctx->rq_ctx);
	init_cq(shared_data->sq_cq_data, &dev_ctx->sqcq_ctx);
	init_sq(shared_data->sq_data, &dev_ctx->sq_ctx);

	////////
	init_send_sq(&dev_ctx->sq_ctx, LOG2VALUE(L2_LOG_SQ_RING_DEPTH), dev_ctx->lkey);

	dev_ctx->is_initialized = 1;



	flexio_dev_print("DEVICE:: DEVICE INITIALIZED #%d, %d\n", shared_data->idx, flexio_dev_get_thread_id(dtctx));

	return 0;
}



inline static union flexio_dev_sqe_seg *get_next_data_sqe(struct sq_ctx_t *sq_ctx, uint32_t sq_idx_mask) {
    union flexio_dev_sqe_seg *res = &sq_ctx->sq_ring[(sq_ctx->sq_wqe_seg_idx + 2) & sq_idx_mask];
    sq_ctx->sq_wqe_seg_idx += 4;
    return res;
}

/*
 * This function is called when a new packet is received to RQ's CQ.
 * Upon receiving a packet, the function will iterate over all received packets and process them.
 * Once all packets in the CQ are processed, the CQ will be rearmed to receive new packets events.
 */
void __dpa_global__ l2_reflector_device_event_handler(uint64_t thread_index)
{
	struct flexio_dev_thread_ctx *dtctx;
	flexio_dev_get_thread_ctx(&dtctx);
	flexio_dev_print("DEVICE:: DEVICE REACHING #%d, %ld\n", flexio_dev_get_thread_id(dtctx), thread_index);

	struct device_context *dev_ctx = &dev_ctxs[thread_index];
	
	int i;
	uint32_t cqe_cnt=0;
	struct flexio_dev_cqe64 *cqe_now;
	int rq_wqe_idx;
	uint32_t data_sz[BATCH_SIZE];
	char *data_host_ptr[BATCH_SIZE];
	char *data_dpa_ptr[BATCH_SIZE];
	// int cnt=0;
	// char tmp;
	union flexio_dev_sqe_seg *swqe[BATCH_SIZE*4];
	size_t src_mac;
	size_t dst_mac;
	// uint16_t src_udpport;
	// uint16_t dst_udpport;

	if (dev_ctx->is_initialized == 0)
	{
		flexio_dev_print("DEVICE:: OHNO\n");
		flexio_dev_thread_reschedule();
	}
		
	
	

	dev_ctx->host_rq_ctx.dpa_rx_buff=get_host_buffer_with_dtctx(dtctx, dev_ctx->host_rq_ctx.rq_window_id, dev_ctx->host_rq_ctx.rkey, dev_ctx->host_rq_ctx.host_rx_buff);

	
								
	while(1)
	{				
		// flexio_dev_print("%d\n", cnt++);
		if(flexio_dev_cqe_get_owner(&(dev_ctx->rqcq_ctx.cq_ring[dev_ctx->rqcq_ctx.cq_idx+cqe_cnt & L2_CQ_IDX_MASK])) != dev_ctx->rqcq_ctx.cq_hw_owner_bit)
		{
			// flexio_dev_print("owner: %d, %d, %d\n", flexio_dev_cqe_get_owner(&(dev_ctx.rqcq_ctx.cq_ring[dev_ctx.rqcq_ctx.cq_idx+cqe_cnt & L2_CQ_IDX_MASK])), dev_ctx.rqcq_ctx.cq_hw_owner_bit, dev_ctx.rqcq_ctx.cq_idx+cqe_cnt);
			cqe_cnt++;
		}

		if(cqe_cnt%BATCH_SIZE==0 && cqe_cnt!=0)
		{
			cqe_cnt=0;
			// flexio_dev_print("dev_ctx.rqcq_ctx.cq_idx: %d\n", dev_ctx.rqcq_ctx.cq_idx);

			// 获取RQ CQE
			for(i=0; i<BATCH_SIZE; i++)
			{
				cqe_now=&(dev_ctx->rqcq_ctx.cq_ring[dev_ctx->rqcq_ctx.cq_idx+i & L2_CQ_IDX_MASK]);
				rq_wqe_idx = flexio_dev_cqe_get_wqe_counter(cqe_now);
				data_host_ptr[i]=flexio_dev_rwqe_get_addr(&dev_ctx->rq_ctx.rq_ring[rq_wqe_idx & L2_RQ_IDX_MASK]);
				data_sz[i]=flexio_dev_cqe_get_byte_cnt(cqe_now);
				data_dpa_ptr[i]=(char *)((flexio_uintptr_t)data_host_ptr[i] - (flexio_uintptr_t)dev_ctx->host_rq_ctx.host_rx_buff + dev_ctx->host_rq_ctx.dpa_rx_buff);
				// flexio_dev_print("%lx, %lx\n", dev_ctx.host_rq_ctx.dpa_rx_buff, (flexio_uintptr_t)dev_ctx.host_rq_ctx.host_rx_buff);
			}
			// flexio_dev_print("222\n");

			// flexio_dev_print("%d\n", cnt++);
			// flexio_dev_print("%d.%d.%d.%d\n", (uint8_t)data_dpa_ptr[0][26+64], (uint8_t)data_dpa_ptr[0][27+64], (uint8_t)data_dpa_ptr[0][28+64], (uint8_t)data_dpa_ptr[0][29+64]);
			// flexio_dev_print("%d.%d.%d.%d\n", (uint8_t)data_dpa_ptr[0][30+64], (uint8_t)data_dpa_ptr[0][31+64], (uint8_t)data_dpa_ptr[0][32+64], (uint8_t)data_dpa_ptr[0][33+64]);
			// flexio_dev_print("%d ", ((uint8_t)data_dpa_ptr[0][34]<<8)|(uint8_t)data_dpa_ptr[0][35]);
			// flexio_dev_print("%d\n\n", ((uint8_t)data_dpa_ptr[0][36]<<8)|(uint8_t)data_dpa_ptr[0][37]);

			// flexio_dev_print("1 %lx\n", ((dev_ctx.host_rq_ctx.dpa_rx_buff)));
			// flexio_dev_print("2 %lx\n", *((uint64_t *)(dev_ctx.host_rq_ctx.dpa_rx_buff)));
			
			// flexio_dev_print("333\n");

			// 交换MAC
			// for(i=0; i<BATCH_SIZE; i++)
			// {
			// 	for (int byte = 0; byte < NB_MAC_ADDRESS_BYTES; byte++) {
			// 		tmp = data_ptr[i][byte];
			// 		data_ptr[i][byte] = data_ptr[i][byte + NB_MAC_ADDRESS_BYTES];
			// 		/* dst and src MACs are aligned one after the other in the ether header */
			// 		data_ptr[i][byte + NB_MAC_ADDRESS_BYTES] = tmp;
			// 	}
			// }

			for(i=0; i<BATCH_SIZE; i++)
			{
				src_mac = *(size_t *)(data_dpa_ptr[i]);
				dst_mac = *(size_t *)(data_dpa_ptr[i] + 6);
				*(size_t *)(data_dpa_ptr[i]) = dst_mac;
				*(size_t *)(data_dpa_ptr[i] + 6) = (src_mac & 0x0000FFFFFFFFFFFF) | (0x0008ll << 48);

				// 交换UDP PORT
				// src_udpport = *(uint16_t *)(data_dpa_ptr[i]+34);
				// dst_udpport = *(uint16_t *)(data_dpa_ptr[i]+36);
				// *(uint16_t *)(data_dpa_ptr[i]+34) = dst_udpport;
				// *(uint16_t *)(data_dpa_ptr[i]+36) = (src_udpport & 0xFFFF);

				// for (int byte = 34; byte < 34+NB_UDP_PORT_BYTES; byte++) {
				// 	char tmp = data_ptr[i][byte];
				// 	data_ptr[i][byte] = data_ptr[i][byte + NB_UDP_PORT_BYTES];
				// 	/* dst and src MACs are aligned one after the other in the ether header */
				// 	data_ptr[i][byte + NB_UDP_PORT_BYTES] = tmp;
				// }
			}

			// 生成新SQ CQE
			/* Take first segment for SQ WQE (3 segments will be used) */
			for(i=0; i<BATCH_SIZE; i++)
			{
				swqe[i]=get_next_data_sqe(&dev_ctx->sq_ctx, L2_SQ_IDX_MASK);
			}
			
			/* Fill out 1-st segment (Control) */
			// for(i=0; i<BATCH_SIZE; i++)
			// {
			// 	flexio_dev_swqe_seg_ctrl_set(
			// 		swqe[i*4],
			// 		dev_ctx->sq_ctx.sq_pi+i,
			// 		dev_ctx->sq_ctx.sq_number,
			// 		MLX5_CTRL_SEG_CE_CQE_ON_CQE_ERROR,
			// 		FLEXIO_CTRL_SEG_SEND_EN
			// 	);
			// }
			/* Fill out 2-nd segment (Ethernet) */
			// for(i=0; i<BATCH_SIZE; i++)
			// {
			// 	flexio_dev_swqe_seg_eth_set(swqe[i*4+1], MSS, CHECKSUM, 0, NULL);
			// }
			/* Fill out 3-rd segment (Data) */
			for(i=0; i<BATCH_SIZE; i++)
			{
				swqe[i]->mem_ptr_send_data.byte_count = cpu_to_be32(data_sz[i]);
    			swqe[i]->mem_ptr_send_data.addr = cpu_to_be64((uint64_t)data_host_ptr[i]);
				// flexio_dev_swqe_seg_mem_ptr_data_set(swqe[i], data_sz[i], dev_ctx->lkey, (uint64_t)data_host_ptr[i]);
			}
			
			/* Ring DB */
			dev_ctx->sq_ctx.sq_pi+=BATCH_SIZE;
			__dpa_thread_memory_writeback();
    		__dpa_thread_window_writeback();
			// __dpa_thread_fence(__DPA_MEMORY, __DPA_W, __DPA_W);
			flexio_dev_qp_sq_ring_db(dtctx, dev_ctx->sq_ctx.sq_pi, dev_ctx->sq_ctx.sq_number);
			// 没有一个批量递增的API，使这里很占时间而又不能被优化
			for(i=0; i<BATCH_SIZE; i++)
			{
				flexio_dev_dbr_rq_inc_pi(dev_ctx->rq_ctx.rq_dbr);
			}
			
			
			// 更新SCQ的头指针并向硬件同步
			dev_ctx->rqcq_ctx.cq_idx+=BATCH_SIZE;	// cq_idx就是consumer pointer (index)
			// flexio_dev_print("increasing: %d\n", dev_ctx.rqcq_ctx.cq_idx);
			/* check for wrap around */
			if (!(dev_ctx->rqcq_ctx.cq_idx & L2_CQ_IDX_MASK))
			{
				// flexio_dev_print("flapping: %d, %d\n", dev_ctx.rqcq_ctx.cq_idx, dev_ctx.rqcq_ctx.cq_hw_owner_bit);
				dev_ctx->rqcq_ctx.cq_hw_owner_bit = !dev_ctx->rqcq_ctx.cq_hw_owner_bit;	
			}
			flexio_dev_dbr_cq_set_ci(dev_ctx->rqcq_ctx.cq_dbr, dev_ctx->rqcq_ctx.cq_idx);

			// flexio_dev_print("Finished one round\n");
		}
	}

    flexio_dev_thread_reschedule();
}
