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

#include <stdio.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>

#include <pciaccess.h>

#include <rte_config.h>
#include <rte_cycles.h>
#include <rte_mempool.h>
#include <rte_malloc.h>
#include <rte_lcore.h>

#include "spdk/file.h"
#include "spdk/nvme.h"
#include "spdk/pci.h"
#include "spdk/string.h"

#if HAVE_LIBAIO
#include <libaio.h>
#include <sys/stat.h>
#include <fcntl.h>
#endif

struct ctrlr_entry {
	struct nvme_controller	*ctrlr;
	struct ctrlr_entry	*next;
	char			name[1024];
};

enum entry_type {
	ENTRY_TYPE_NVME_NS,
	ENTRY_TYPE_AIO_FILE,
};

struct ns_entry {
	enum entry_type		type;

	union {
		struct {
			struct nvme_controller	*ctrlr;
			struct nvme_namespace	*ns;
		} nvme;
#if HAVE_LIBAIO
		struct {
			int			fd;
		} aio;
#endif
	} u;

	struct ns_entry		*next;
	uint32_t		io_size_blocks;
	uint64_t		size_in_ios;
	char			name[1024];
};

struct ns_worker_ctx {
	struct ns_entry		*entry;
	uint64_t		io_completed;
	uint64_t		current_queue_depth;
	uint64_t		offset_in_ios;
	bool			is_draining;

#if HAVE_LIBAIO
	struct io_event		*events;
	io_context_t		ctx;
#endif
	struct ns_worker_ctx	*next;
};

struct perf_task {
	struct ns_worker_ctx	*ns_ctx;
	void			*buf;
#if HAVE_LIBAIO
	struct iocb		iocb;
#endif
};

struct worker_thread {
	struct ns_worker_ctx 	*ns_ctx;
	struct worker_thread	*next;
	unsigned		lcore;
};

struct rte_mempool *request_mempool;
static struct rte_mempool *task_pool;

static struct ctrlr_entry *g_controllers = NULL;
static struct ns_entry *g_namespaces = NULL;
static int g_num_namespaces = 0;
static struct worker_thread *g_workers = NULL;
static int g_num_workers = 0;

static uint64_t g_tsc_rate;

static int g_io_size_bytes;
static int g_rw_percentage;
static int g_is_random;
static int g_queue_depth;
static int g_time_in_sec;
static uint32_t g_max_completions;

static const char *g_core_mask;

static int g_aio_optind; /* Index of first AIO filename in argv */

static void
task_complete(struct perf_task *task);

static void
register_ns(struct nvme_controller *ctrlr, struct pci_device *pci_dev, struct nvme_namespace *ns)
{
	struct ns_entry *entry;
	const struct nvme_controller_data *cdata;

	entry = malloc(sizeof(struct ns_entry));
	if (entry == NULL) {
		perror("ns_entry malloc");
		exit(1);
	}

	cdata = nvme_ctrlr_get_data(ctrlr);

	entry->type = ENTRY_TYPE_NVME_NS;
	entry->u.nvme.ctrlr = ctrlr;
	entry->u.nvme.ns = ns;
	entry->size_in_ios = nvme_ns_get_size(ns) /
			     g_io_size_bytes;
	entry->io_size_blocks = g_io_size_bytes / nvme_ns_get_sector_size(ns);

	snprintf(entry->name, 44, "%-20.20s (%-20.20s)", cdata->mn, cdata->sn);

	g_num_namespaces++;
	entry->next = g_namespaces;
	g_namespaces = entry;
}

static void
register_ctrlr(struct nvme_controller *ctrlr, struct pci_device *pci_dev)
{
	int nsid, num_ns;
	struct ctrlr_entry *entry = malloc(sizeof(struct ctrlr_entry));

	if (entry == NULL) {
		perror("ctrlr_entry malloc");
		exit(1);
	}

	entry->ctrlr = ctrlr;
	entry->next = g_controllers;
	g_controllers = entry;

	num_ns = nvme_ctrlr_get_num_ns(ctrlr);
	for (nsid = 1; nsid <= num_ns; nsid++) {
		register_ns(ctrlr, pci_dev, nvme_ctrlr_get_ns(ctrlr, nsid));
	}

}

#if HAVE_LIBAIO
static int
register_aio_file(const char *path)
{
	struct ns_entry *entry;

	int flags, fd;
	uint64_t size;
	uint32_t blklen;

	if (g_rw_percentage == 100) {
		flags = O_RDONLY;
	} else {
		flags = O_RDWR;
	}

	flags |= O_DIRECT;

	fd = open(path, flags);
	if (fd < 0) {
		fprintf(stderr, "Could not open AIO device %s: %s\n", path, strerror(errno));
		return -1;
	}

	size = file_get_size(fd);
	if (size == 0) {
		fprintf(stderr, "Could not determine size of AIO device %s\n", path);
		close(fd);
		return -1;
	}

	blklen = dev_get_blocklen(fd);
	if (blklen == 0) {
		fprintf(stderr, "Could not determine block size of AIO device %s\n", path);
		close(fd);
		return -1;
	}

	entry = malloc(sizeof(struct ns_entry));
	if (entry == NULL) {
		close(fd);
		perror("aio ns_entry malloc");
		return -1;
	}

	entry->type = ENTRY_TYPE_AIO_FILE;
	entry->u.aio.fd = fd;
	entry->size_in_ios = size / g_io_size_bytes;
	entry->io_size_blocks = g_io_size_bytes / blklen;

	snprintf(entry->name, sizeof(entry->name), "%s", path);

	g_num_namespaces++;
	entry->next = g_namespaces;
	g_namespaces = entry;

	return 0;
}

static int
aio_submit(io_context_t aio_ctx, struct iocb *iocb, int fd, enum io_iocb_cmd cmd, void *buf,
	   unsigned long nbytes, uint64_t offset, void *cb_ctx)
{
	iocb->aio_fildes = fd;
	iocb->aio_reqprio = 0;
	iocb->aio_lio_opcode = cmd;
	iocb->u.c.buf = buf;
	iocb->u.c.nbytes = nbytes;
	iocb->u.c.offset = offset;
	iocb->data = cb_ctx;

	if (io_submit(aio_ctx, 1, &iocb) < 0) {
		perror("io_submit");
		return -1;
	}

	return 0;
}

static void
aio_check_io(struct ns_worker_ctx *ns_ctx)
{
	int count, i;
	struct timespec timeout;

	timeout.tv_sec = 0;
	timeout.tv_nsec = 0;

	count = io_getevents(ns_ctx->ctx, 1, g_queue_depth, ns_ctx->events, &timeout);
	if (count < 0) {
		fprintf(stderr, "io_getevents error\n");
		exit(1);
	}

	for (i = 0; i < count; i++) {
		task_complete(ns_ctx->events[i].data);
	}
}
#endif /* HAVE_LIBAIO */

static void task_ctor(struct rte_mempool *mp, void *arg, void *__task, unsigned id)
{
	struct perf_task *task = __task;
	task->buf = rte_malloc(NULL, g_io_size_bytes, 0x200);
	if (task->buf == NULL) {
		fprintf(stderr, "task->buf rte_malloc failed\n");
		exit(1);
	}
}

static void io_complete(void *ctx, const struct nvme_completion *completion);

static __thread unsigned int seed = 0;

static void
submit_single_io(struct ns_worker_ctx *ns_ctx)
{
	struct perf_task	*task = NULL;
	uint64_t		offset_in_ios;
	int			rc;
	struct ns_entry		*entry = ns_ctx->entry;

	if (rte_mempool_get(task_pool, (void **)&task) != 0) {
		fprintf(stderr, "task_pool rte_mempool_get failed\n");
		exit(1);
	}

	task->ns_ctx = ns_ctx;

	if (g_is_random) {
		offset_in_ios = rand_r(&seed) % entry->size_in_ios;
	} else {
		offset_in_ios = ns_ctx->offset_in_ios++;
		if (ns_ctx->offset_in_ios == entry->size_in_ios) {
			ns_ctx->offset_in_ios = 0;
		}
	}

	if ((g_rw_percentage == 100) ||
	    (g_rw_percentage != 0 && ((rand_r(&seed) % 100) < g_rw_percentage))) {
#if HAVE_LIBAIO
		if (entry->type == ENTRY_TYPE_AIO_FILE) {
			rc = aio_submit(ns_ctx->ctx, &task->iocb, entry->u.aio.fd, IO_CMD_PREAD, task->buf,
					g_io_size_bytes, offset_in_ios * g_io_size_bytes, task);
		} else
#endif
		{
			//rc = nvme_ns_cmd_read(entry->u.nvme.ns, task->buf, offset_in_ios * entry->io_size_blocks,
			//		      entry->io_size_blocks, io_complete, task);
			// @yzy
			rc = nvme_ns_cmd_read_by_id(entry->u.nvme.ns, task->buf, offset_in_ios * entry->io_size_blocks,
					      entry->io_size_blocks, io_complete, task, 0);
			rc = nvme_ns_cmd_read_by_id(entry->u.nvme.ns, task->buf, offset_in_ios * entry->io_size_blocks,
					      entry->io_size_blocks, io_complete, task, 1);
		}
	} else {
#if HAVE_LIBAIO
		if (entry->type == ENTRY_TYPE_AIO_FILE) {
			rc = aio_submit(ns_ctx->ctx, &task->iocb, entry->u.aio.fd, IO_CMD_PWRITE, task->buf,
					g_io_size_bytes, offset_in_ios * g_io_size_bytes, task);
		} else
#endif
		{
			//rc = nvme_ns_cmd_write(entry->u.nvme.ns, task->buf, offset_in_ios * entry->io_size_blocks,
			//		       entry->io_size_blocks, io_complete, task);
			// @yzy
			rc = nvme_ns_cmd_write_by_id(entry->u.nvme.ns, task->buf, offset_in_ios * entry->io_size_blocks,
					       entry->io_size_blocks, io_complete, task, 0);
			rc = nvme_ns_cmd_write_by_id(entry->u.nvme.ns, task->buf, offset_in_ios * entry->io_size_blocks,
					       entry->io_size_blocks, io_complete, task, 1);
		}
	}

	if (rc != 0) {
		fprintf(stderr, "starting I/O failed\n");
	}

	ns_ctx->current_queue_depth++;
}

static void
task_complete(struct perf_task *task)
{
	struct ns_worker_ctx	*ns_ctx;

	ns_ctx = task->ns_ctx;
	ns_ctx->current_queue_depth--;
	ns_ctx->io_completed++;

	rte_mempool_put(task_pool, task);

	/*
	 * is_draining indicates when time has expired for the test run
	 * and we are just waiting for the previously submitted I/O
	 * to complete.  In this case, do not submit a new I/O to replace
	 * the one just completed.
	 */
	if (!ns_ctx->is_draining) {
		submit_single_io(ns_ctx);
	}
}

static void
io_complete(void *ctx, const struct nvme_completion *completion)
{
	task_complete((struct perf_task *)ctx);
}

static void
check_io(struct ns_worker_ctx *ns_ctx)
{
#if HAVE_LIBAIO
	if (ns_ctx->entry->type == ENTRY_TYPE_AIO_FILE) {
		aio_check_io(ns_ctx);
	} else
#endif
	{
		//nvme_ctrlr_process_io_completions(ns_ctx->entry->u.nvme.ctrlr, g_max_completions);
		// @yzy
		nvme_ctrlr_process_io_completions_by_id(ns_ctx->entry->u.nvme.ctrlr, g_max_completions, 0);
		nvme_ctrlr_process_io_completions_by_id(ns_ctx->entry->u.nvme.ctrlr, g_max_completions, 1);
	}
}

static void
submit_io(struct ns_worker_ctx *ns_ctx, int queue_depth)
{
	while (queue_depth-- > 0) {
		submit_single_io(ns_ctx);
	}
}

static void
drain_io(struct ns_worker_ctx *ns_ctx)
{
	ns_ctx->is_draining = true;
	while (ns_ctx->current_queue_depth > 0) {
		check_io(ns_ctx);
	}
}

static int
work_fn(void *arg)
{
	uint64_t tsc_end = rte_get_timer_cycles() + g_time_in_sec * g_tsc_rate;
	struct worker_thread *worker = (struct worker_thread *)arg;
	struct ns_worker_ctx *ns_ctx = NULL;

	printf("Starting thread on core %u\n", worker->lcore);

	if (nvme_register_io_thread() != 0) {
		fprintf(stderr, "nvme_register_io_thread() failed on core %u\n", worker->lcore);
		return -1;
	}

	/* Submit initial I/O for each namespace. */
	ns_ctx = worker->ns_ctx;
	while (ns_ctx != NULL) {
		submit_io(ns_ctx, g_queue_depth);
		ns_ctx = ns_ctx->next;
	}

	while (1) {
		/*
		 * Check for completed I/O for each controller. A new
		 * I/O will be submitted in the io_complete callback
		 * to replace each I/O that is completed.
		 */
		ns_ctx = worker->ns_ctx;
		while (ns_ctx != NULL) {
			check_io(ns_ctx);
			ns_ctx = ns_ctx->next;
		}

		if (rte_get_timer_cycles() > tsc_end) {
			break;
		}
	}

	ns_ctx = worker->ns_ctx;
	while (ns_ctx != NULL) {
		drain_io(ns_ctx);
		ns_ctx = ns_ctx->next;
	}

	nvme_unregister_io_thread();

	return 0;
}

static void usage(char *program_name)
{
	printf("%s options", program_name);
#if HAVE_LIBAIO
	printf(" [AIO device(s)]...");
#endif
	printf("\n");
	printf("\t[-q io depth]\n");
	printf("\t[-s io size in bytes]\n");
	printf("\t[-w io pattern type, must be one of\n");
	printf("\t\t(read, write, randread, randwrite, rw, randrw)]\n");
	printf("\t[-M rwmixread (100 for reads, 0 for writes)]\n");
	printf("\t[-t time in seconds]\n");
	printf("\t[-c core mask for I/O submission/completion.]\n");
	printf("\t\t(default: 1)]\n");
	printf("\t[-m max completions per poll]\n");
	printf("\t\t(default: 0 - unlimited)\n");
}

static void
print_stats(void)
{
	float io_per_second, mb_per_second;
	float total_io_per_second, total_mb_per_second;
	struct worker_thread	*worker;
	struct ns_worker_ctx	*ns_ctx;

	total_io_per_second = 0;
	total_mb_per_second = 0;

	worker = g_workers;
	while (worker) {
		ns_ctx = worker->ns_ctx;
		while (ns_ctx) {
			io_per_second = (float)ns_ctx->io_completed / g_time_in_sec;
			mb_per_second = io_per_second * g_io_size_bytes / (1024 * 1024);
			printf("%-43.43s from core %u: %10.2f IO/s %10.2f MB/s\n",
			       ns_ctx->entry->name, worker->lcore,
			       io_per_second, mb_per_second);
			total_io_per_second += io_per_second;
			total_mb_per_second += mb_per_second;
			ns_ctx = ns_ctx->next;
		}
		worker = worker->next;
	}
	printf("========================================================\n");
	printf("%-55s: %10.2f IO/s %10.2f MB/s\n",
	       "Total", total_io_per_second, total_mb_per_second);
}

static int
parse_args(int argc, char **argv)
{
	const char *workload_type;
	int op;
	bool mix_specified = false;

	/* default value*/
	g_queue_depth = 0;
	g_io_size_bytes = 0;
	workload_type = NULL;
	g_time_in_sec = 0;
	g_rw_percentage = -1;
	g_core_mask = NULL;
	g_max_completions = 0;

	while ((op = getopt(argc, argv, "c:m:q:s:t:w:M:")) != -1) {
		switch (op) {
		case 'c':
			g_core_mask = optarg;
			break;
		case 'm':
			g_max_completions = atoi(optarg);
			break;
		case 'q':
			g_queue_depth = atoi(optarg);
			break;
		case 's':
			g_io_size_bytes = atoi(optarg);
			break;
		case 't':
			g_time_in_sec = atoi(optarg);
			break;
		case 'w':
			workload_type = optarg;
			break;
		case 'M':
			g_rw_percentage = atoi(optarg);
			mix_specified = true;
			break;
		default:
			usage(argv[0]);
			return 1;
		}
	}

	if (!g_queue_depth) {
		usage(argv[0]);
		return 1;
	}
	if (!g_io_size_bytes) {
		usage(argv[0]);
		return 1;
	}
	if (!workload_type) {
		usage(argv[0]);
		return 1;
	}
	if (!g_time_in_sec) {
		usage(argv[0]);
		return 1;
	}

	if (strcmp(workload_type, "read") &&
	    strcmp(workload_type, "write") &&
	    strcmp(workload_type, "randread") &&
	    strcmp(workload_type, "randwrite") &&
	    strcmp(workload_type, "rw") &&
	    strcmp(workload_type, "randrw")) {
		fprintf(stderr,
			"io pattern type must be one of\n"
			"(read, write, randread, randwrite, rw, randrw)\n");
		return 1;
	}

	if (!strcmp(workload_type, "read") ||
	    !strcmp(workload_type, "randread")) {
		g_rw_percentage = 100;
	}

	if (!strcmp(workload_type, "write") ||
	    !strcmp(workload_type, "randwrite")) {
		g_rw_percentage = 0;
	}

	if (!strcmp(workload_type, "read") ||
	    !strcmp(workload_type, "randread") ||
	    !strcmp(workload_type, "write") ||
	    !strcmp(workload_type, "randwrite")) {
		if (mix_specified) {
			fprintf(stderr, "Ignoring -M option... Please use -M option"
				" only when using rw or randrw.\n");
		}
	}

	if (!strcmp(workload_type, "rw") ||
	    !strcmp(workload_type, "randrw")) {
		if (g_rw_percentage < 0 || g_rw_percentage > 100) {
			fprintf(stderr,
				"-M must be specified to value from 0 to 100 "
				"for rw or randrw.\n");
			return 1;
		}
	}

	if (!strcmp(workload_type, "read") ||
	    !strcmp(workload_type, "write") ||
	    !strcmp(workload_type, "rw")) {
		g_is_random = 0;
	} else {
		g_is_random = 1;
	}

	g_aio_optind = optind;
	optind = 1;
	return 0;
}

static int
register_workers(void)
{
	unsigned lcore;
	struct worker_thread *worker;
	struct worker_thread *prev_worker;

	worker = malloc(sizeof(struct worker_thread));
	if (worker == NULL) {
		perror("worker_thread malloc");
		return -1;
	}

	memset(worker, 0, sizeof(struct worker_thread));
	worker->lcore = rte_get_master_lcore();

	g_workers = worker;
	g_num_workers = 1;

	RTE_LCORE_FOREACH_SLAVE(lcore) {
		prev_worker = worker;
		worker = malloc(sizeof(struct worker_thread));
		if (worker == NULL) {
			perror("worker_thread malloc");
			return -1;
		}

		memset(worker, 0, sizeof(struct worker_thread));
		worker->lcore = lcore;
		prev_worker->next = worker;
		g_num_workers++;
	}

	return 0;
}

static int
register_controllers(void)
{
	struct pci_device_iterator	*pci_dev_iter;
	struct pci_device		*pci_dev;
	struct pci_id_match		match;
	int				rc;

	printf("Initializing NVMe Controllers\n");

	pci_system_init();

	match.vendor_id =	PCI_MATCH_ANY;
	match.subvendor_id =	PCI_MATCH_ANY;
	match.subdevice_id =	PCI_MATCH_ANY;
	match.device_id =	PCI_MATCH_ANY;
	match.device_class =	NVME_CLASS_CODE;
	match.device_class_mask = 0xFFFFFF;

	pci_dev_iter = pci_id_match_iterator_create(&match);

	rc = 0;
	while ((pci_dev = pci_device_next(pci_dev_iter))) {
		struct nvme_controller *ctrlr;

		if (pci_device_has_non_null_driver(pci_dev)) {
			fprintf(stderr, "non-null kernel driver attached to nvme\n");
			fprintf(stderr, " controller at pci bdf %d:%d:%d\n",
				pci_dev->bus, pci_dev->dev, pci_dev->func);
			fprintf(stderr, " skipping...\n");
			continue;
		}

		pci_device_probe(pci_dev);

		ctrlr = nvme_attach(pci_dev);
		if (ctrlr == NULL) {
			fprintf(stderr, "nvme_attach failed for controller at pci bdf %d:%d:%d\n",
				pci_dev->bus, pci_dev->dev, pci_dev->func);
			rc = 1;
			continue;
		}

		register_ctrlr(ctrlr, pci_dev);
	}

	pci_iterator_destroy(pci_dev_iter);

	return rc;
}

static void
unregister_controllers(void)
{
	struct ctrlr_entry *entry = g_controllers;

	while (entry) {
		struct ctrlr_entry *next = entry->next;
		nvme_detach(entry->ctrlr);
		free(entry);
		entry = next;
	}
}

static int
register_aio_files(int argc, char **argv)
{
#if HAVE_LIBAIO
	int i;

	/* Treat everything after the options as files for AIO */
	for (i = g_aio_optind; i < argc; i++) {
		if (register_aio_file(argv[i]) != 0) {
			return 1;
		}
	}
#endif /* HAVE_LIBAIO */

	return 0;
}

static int
associate_workers_with_ns(void)
{
	struct ns_entry		*entry = g_namespaces;
	struct worker_thread	*worker = g_workers;
	struct ns_worker_ctx	*ns_ctx;
	int			i, count;

	count = g_num_namespaces > g_num_workers ? g_num_namespaces : g_num_workers;

	for (i = 0; i < count; i++) {
		ns_ctx = malloc(sizeof(struct ns_worker_ctx));
		if (!ns_ctx) {
			return -1;
		}
		memset(ns_ctx, 0, sizeof(*ns_ctx));
#ifdef HAVE_LIBAIO
		ns_ctx->events = calloc(g_queue_depth, sizeof(struct io_event));
		if (!ns_ctx->events) {
			return -1;
		}
		ns_ctx->ctx = 0;
		if (io_setup(g_queue_depth, &ns_ctx->ctx) < 0) {
			perror("io_setup");
			return -1;
		}
#endif

		printf("Associating %s with lcore %d\n", entry->name, worker->lcore);
		ns_ctx->entry = entry;
		ns_ctx->next = worker->ns_ctx;
		worker->ns_ctx = ns_ctx;

		worker = worker->next;
		if (worker == NULL) {
			worker = g_workers;
		}

		entry = entry->next;
		if (entry == NULL) {
			entry = g_namespaces;
		}

	}

	return 0;
}

static char *ealargs[] = {
	"perf",
	"-c 0x1", /* This must be the second parameter. It is overwritten by index in main(). */
	"-n 4",
};

int main(int argc, char **argv)
{
	int rc;
	struct worker_thread *worker;

	rc = parse_args(argc, argv);
	if (rc != 0) {
		return rc;
	}

	ealargs[1] = sprintf_alloc("-c %s", g_core_mask ? g_core_mask : "0x1");
	if (ealargs[1] == NULL) {
		perror("ealargs sprintf_alloc");
		return 1;
	}

	rc = rte_eal_init(sizeof(ealargs) / sizeof(ealargs[0]), ealargs);

	free(ealargs[1]);

	if (rc < 0) {
		fprintf(stderr, "could not initialize dpdk\n");
		return 1;
	}

	request_mempool = rte_mempool_create("nvme_request", 8192,
					     nvme_request_size(), 128, 0,
					     NULL, NULL, NULL, NULL,
					     SOCKET_ID_ANY, 0);

	if (request_mempool == NULL) {
		fprintf(stderr, "could not initialize request mempool\n");
		return 1;
	}

	task_pool = rte_mempool_create("task_pool", 8192,
				       sizeof(struct perf_task),
				       64, 0, NULL, NULL, task_ctor, NULL,
				       SOCKET_ID_ANY, 0);

	g_tsc_rate = rte_get_timer_hz();

	if (register_workers() != 0) {
		return 1;
	}

	if (register_aio_files(argc, argv) != 0) {
		return 1;
	}

	if (register_controllers() != 0) {
		return 1;
	}

	if (associate_workers_with_ns() != 0) {
		return 1;
	}

	printf("Initialization complete. Launching workers.\n");

	/* Launch all of the slave workers */
	worker = g_workers->next;
	while (worker != NULL) {
		rte_eal_remote_launch(work_fn, worker, worker->lcore);
		worker = worker->next;
	}

	rc = work_fn(g_workers);

	worker = g_workers->next;
	while (worker != NULL) {
		if (rte_eal_wait_lcore(worker->lcore) < 0) {
			rc = -1;
		}
		worker = worker->next;
	}

	print_stats();

	unregister_controllers();

	if (rc != 0) {
		fprintf(stderr, "%s: errors occured\n", argv[0]);
	}

	return rc;
}
