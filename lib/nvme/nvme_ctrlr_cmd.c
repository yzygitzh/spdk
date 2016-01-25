/*-
 *   BSD LICENSE
 *
 *   Copyright(c) 2010-2015 Intel Corporation. All rights reserved.
 *   All rights reserved.
 *
 *   Redistribution and use in source and binary forms, with or without
 *   modification, are permitted provided that the following conditions
 *   are met:
 *
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in
 *       the documentation and/or other materials provided with the
 *       distribution.
 *     * Neither the name of Intel Corporation nor the names of its
 *       contributors may be used to endorse or promote products derived
 *       from this software without specific prior written permission.
 *
 *   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *   "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *   LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 *   A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 *   OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 *   SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 *   LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 *   DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 *   THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 *   (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 *   OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "nvme_internal.h"

int
nvme_ctrlr_cmd_io_raw(struct nvme_controller *ctrlr,
		      struct nvme_command *cmd,
		      void *buf, uint32_t len,
		      nvme_cb_fn_t cb_fn, void *cb_arg)
{
	struct nvme_request	*req;

	req = nvme_allocate_request(buf, len, cb_fn, cb_arg);

	if (req == NULL) {
		return ENOMEM;
	}

	memcpy(&req->cmd, cmd, sizeof(req->cmd));

	nvme_ctrlr_submit_io_request(ctrlr, req);
	return 0;
}

// @yzy
// new wrap function
int
nvme_ctrlr_cmd_io_raw_by_id(struct nvme_controller *ctrlr,
		      struct nvme_command *cmd,
		      void *buf, uint32_t len,
		      nvme_cb_fn_t cb_fn, void *cb_arg, int ioq_index)
{
	struct nvme_request	*req;

	req = nvme_allocate_request(buf, len, cb_fn, cb_arg);

	if (req == NULL) {
		return ENOMEM;
	}

	memcpy(&req->cmd, cmd, sizeof(req->cmd));

	return nvme_ctrlr_submit_io_request_by_id(ctrlr, req, ioq_index);
}

int
nvme_ctrlr_cmd_admin_raw(struct nvme_controller *ctrlr,
			 struct nvme_command *cmd,
			 void *buf, uint32_t len,
			 nvme_cb_fn_t cb_fn, void *cb_arg)
{
	struct nvme_request	*req;

	nvme_mutex_lock(&ctrlr->ctrlr_lock);
	req = nvme_allocate_request(buf, len, cb_fn, cb_arg);
	if (req == NULL) {
		nvme_mutex_unlock(&ctrlr->ctrlr_lock);
		return ENOMEM;
	}

	memcpy(&req->cmd, cmd, sizeof(req->cmd));

	nvme_ctrlr_submit_admin_request(ctrlr, req);

	nvme_mutex_unlock(&ctrlr->ctrlr_lock);
	return 0;
}

void
nvme_ctrlr_cmd_identify_controller(struct nvme_controller *ctrlr, void *payload,
				   nvme_cb_fn_t cb_fn, void *cb_arg)
{
	struct nvme_request *req;
	struct nvme_command *cmd;

	req = nvme_allocate_request(payload,
				    sizeof(struct nvme_controller_data),
				    cb_fn, cb_arg);

	cmd = &req->cmd;
	cmd->opc = NVME_OPC_IDENTIFY;

	/*
	 * TODO: create an identify command data structure, which
	 *  includes this CNS bit in cdw10.
	 */
	cmd->cdw10 = 1;

	nvme_ctrlr_submit_admin_request(ctrlr, req);
}

void
nvme_ctrlr_cmd_identify_namespace(struct nvme_controller *ctrlr, uint16_t nsid,
				  void *payload, nvme_cb_fn_t cb_fn, void *cb_arg)
{
	struct nvme_request *req;
	struct nvme_command *cmd;

	req = nvme_allocate_request(payload,
				    sizeof(struct nvme_namespace_data),
				    cb_fn, cb_arg);

	cmd = &req->cmd;
	cmd->opc = NVME_OPC_IDENTIFY;

	/*
	 * TODO: create an identify command data structure
	 */
	cmd->nsid = nsid;

	nvme_ctrlr_submit_admin_request(ctrlr, req);
}

void
nvme_ctrlr_cmd_create_io_cq(struct nvme_controller *ctrlr,
			    struct nvme_qpair *io_que, nvme_cb_fn_t cb_fn,
			    void *cb_arg)
{
	struct nvme_request *req;
	struct nvme_command *cmd;

	req = nvme_allocate_request(NULL, 0, cb_fn, cb_arg);

	cmd = &req->cmd;
	cmd->opc = NVME_OPC_CREATE_IO_CQ;

	/*
	 * TODO: create a create io completion queue command data
	 *  structure.
	 */
	cmd->cdw10 = ((io_que->num_entries - 1) << 16) | io_que->id;
	/*
	 * 0x2 = interrupts enabled
	 * 0x1 = physically contiguous
	 */
	cmd->cdw11 = (io_que->id << 16) | 0x1;
	cmd->dptr.prp.prp1 = io_que->cpl_bus_addr;

	nvme_ctrlr_submit_admin_request(ctrlr, req);
}

void
nvme_ctrlr_cmd_create_io_sq(struct nvme_controller *ctrlr,
			    struct nvme_qpair *io_que, nvme_cb_fn_t cb_fn, void *cb_arg)
{
	struct nvme_request *req;
	struct nvme_command *cmd;

	req = nvme_allocate_request(NULL, 0, cb_fn, cb_arg);

	cmd = &req->cmd;
	cmd->opc = NVME_OPC_CREATE_IO_SQ;

	/*
	 * TODO: create a create io submission queue command data
	 *  structure.
	 */
	cmd->cdw10 = ((io_que->num_entries - 1) << 16) | io_que->id;
	/* 0x1 = physically contiguous */
	cmd->cdw11 = (io_que->id << 16) | 0x1;
	cmd->dptr.prp.prp1 = io_que->cmd_bus_addr;

	nvme_ctrlr_submit_admin_request(ctrlr, req);
}

void
nvme_ctrlr_cmd_set_feature(struct nvme_controller *ctrlr, uint8_t feature,
			   uint32_t cdw11, void *payload, uint32_t payload_size,
			   nvme_cb_fn_t cb_fn, void *cb_arg)
{
	struct nvme_request *req;
	struct nvme_command *cmd;

	req = nvme_allocate_request(NULL, 0, cb_fn, cb_arg);

	cmd = &req->cmd;
	cmd->opc = NVME_OPC_SET_FEATURES;
	cmd->cdw10 = feature;
	cmd->cdw11 = cdw11;

	nvme_ctrlr_submit_admin_request(ctrlr, req);
}

void
nvme_ctrlr_cmd_get_feature(struct nvme_controller *ctrlr, uint8_t feature,
			   uint32_t cdw11, void *payload, uint32_t payload_size,
			   nvme_cb_fn_t cb_fn, void *cb_arg)
{
	struct nvme_request *req;
	struct nvme_command *cmd;

	req = nvme_allocate_request(NULL, 0, cb_fn, cb_arg);

	cmd = &req->cmd;
	cmd->opc = NVME_OPC_GET_FEATURES;
	cmd->cdw10 = feature;
	cmd->cdw11 = cdw11;

	nvme_ctrlr_submit_admin_request(ctrlr, req);
}

void
nvme_ctrlr_cmd_set_num_queues(struct nvme_controller *ctrlr,
			      uint32_t num_queues, nvme_cb_fn_t cb_fn, void *cb_arg)
{
	uint32_t cdw11;

	cdw11 = ((num_queues - 1) << 16) | (num_queues - 1);
	nvme_ctrlr_cmd_set_feature(ctrlr, NVME_FEAT_NUMBER_OF_QUEUES, cdw11,
				   NULL, 0, cb_fn, cb_arg);
}

void
nvme_ctrlr_cmd_set_async_event_config(struct nvme_controller *ctrlr,
				      union nvme_critical_warning_state state, nvme_cb_fn_t cb_fn,
				      void *cb_arg)
{
	uint32_t cdw11;

	cdw11 = state.raw;
	nvme_ctrlr_cmd_set_feature(ctrlr,
				   NVME_FEAT_ASYNC_EVENT_CONFIGURATION, cdw11, NULL, 0, cb_fn,
				   cb_arg);
}

void
nvme_ctrlr_cmd_get_log_page(struct nvme_controller *ctrlr, uint8_t log_page,
			    uint32_t nsid, void *payload, uint32_t payload_size, nvme_cb_fn_t cb_fn,
			    void *cb_arg)
{
	struct nvme_request *req;
	struct nvme_command *cmd;

	req = nvme_allocate_request(payload, payload_size, cb_fn, cb_arg);

	cmd = &req->cmd;
	cmd->opc = NVME_OPC_GET_LOG_PAGE;
	cmd->nsid = nsid;
	cmd->cdw10 = ((payload_size / sizeof(uint32_t)) - 1) << 16;
	cmd->cdw10 |= log_page;

	nvme_ctrlr_submit_admin_request(ctrlr, req);
}

void
nvme_ctrlr_cmd_get_error_page(struct nvme_controller *ctrlr,
			      struct nvme_error_information_entry *payload, uint32_t num_entries,
			      nvme_cb_fn_t cb_fn, void *cb_arg)
{

	nvme_assert(num_entries > 0, ("%s called with num_entries==0\n", __func__));

	/* Controller's error log page entries is 0-based. */
	nvme_assert(num_entries <= (ctrlr->cdata.elpe + 1u),
		    ("%s called with num_entries=%d but (elpe+1)=%d\n", __func__,
		     num_entries, ctrlr->cdata.elpe + 1));

	if (num_entries > (ctrlr->cdata.elpe + 1u))
		num_entries = ctrlr->cdata.elpe + 1u;

	nvme_ctrlr_cmd_get_log_page(ctrlr, NVME_LOG_ERROR,
				    NVME_GLOBAL_NAMESPACE_TAG, payload, sizeof(*payload) * num_entries,
				    cb_fn, cb_arg);
}

void
nvme_ctrlr_cmd_get_health_information_page(struct nvme_controller *ctrlr,
		uint32_t nsid, struct nvme_health_information_page *payload,
		nvme_cb_fn_t cb_fn, void *cb_arg)
{

	nvme_ctrlr_cmd_get_log_page(ctrlr, NVME_LOG_HEALTH_INFORMATION,
				    nsid, payload, sizeof(*payload), cb_fn, cb_arg);
}

void
nvme_ctrlr_cmd_get_firmware_page(struct nvme_controller *ctrlr,
				 struct nvme_firmware_page *payload, nvme_cb_fn_t cb_fn, void *cb_arg)
{

	nvme_ctrlr_cmd_get_log_page(ctrlr, NVME_LOG_FIRMWARE_SLOT,
				    NVME_GLOBAL_NAMESPACE_TAG, payload, sizeof(*payload), cb_fn,
				    cb_arg);
}

void
nvme_ctrlr_cmd_abort(struct nvme_controller *ctrlr, uint16_t cid,
		     uint16_t sqid, nvme_cb_fn_t cb_fn, void *cb_arg)
{
	struct nvme_request *req;
	struct nvme_command *cmd;

	req = nvme_allocate_request(NULL, 0, cb_fn, cb_arg);

	cmd = &req->cmd;
	cmd->opc = NVME_OPC_ABORT;
	cmd->cdw10 = (cid << 16) | sqid;

	nvme_ctrlr_submit_admin_request(ctrlr, req);
}
