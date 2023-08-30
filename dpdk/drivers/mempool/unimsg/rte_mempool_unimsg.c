/* SPDX-License-Identifier: BSD-3-Clause
 * Some sort of Copyright
 */

#include <stdio.h>
#include <string.h>

#include <rte_errno.h>
#include <rte_malloc.h>
#include <rte_mempool.h>

#include "ring.h"

#define MAX_BULK_SIZE 2048

struct mempool_info {
	unsigned long base_addr;
	size_t tot_esize;
	size_t hdr_size;
	struct unimsg_ring *r;
};

/* TODO: this is awful, find a best API to set the ring */
struct unimsg_ring *rte_mempool_unimsg_ring;

static void
init_base_addr(struct rte_mempool *mp)
{
	struct mempool_info *mi = mp->pool_data;
	struct rte_mempool_memhdr *memhdr = STAILQ_FIRST(&mp->mem_list);
	if (!memhdr)
		rte_panic("Unimsg mempool cannot find base addr of mempool");

	mi->base_addr = (unsigned long)memhdr->addr;
}

static int
unimsg_enqueue(struct rte_mempool *mp, void * const *obj_table, unsigned n)
{
	unsigned items[MAX_BULK_SIZE];
	struct mempool_info *mi = mp->pool_data;

	if (!mi->base_addr)
		init_base_addr(mp);

	/* Map addresses to indexes in the pool */
	for (unsigned i = 0; i < n; i++) {
		items[i] = ((unsigned long)obj_table[i] - mi->base_addr)
			   / mi->tot_esize;
	}

	if (unimsg_ring_enqueue(mi->r, items, n))
		return -ENOBUFS;

	return 0;
}

static int
unimsg_dequeue(struct rte_mempool *mp, void **obj_table, unsigned n)
{
	unsigned items[MAX_BULK_SIZE];
	struct mempool_info *mi = mp->pool_data;

	if (!mi->base_addr)
		init_base_addr(mp);

	if (unimsg_ring_dequeue(mi->r, items, n))
		return -ENOBUFS;

	/* Map indexes in the pool to addresses */
	for (unsigned i = 0; i < n; i++) {
		obj_table[i] = (void *)(items[i] * mi->tot_esize + mi->hdr_size
					+ mi->base_addr);
	}

	return 0;
}

static unsigned
unimsg_get_count(const struct rte_mempool *mp)
{
	struct mempool_info *mi = mp->pool_data;

	return unimsg_ring_count(mi->r);
}

static int
unimsg_alloc(struct rte_mempool *mp)
{
	int ret;
	char rg_name[RTE_RING_NAMESIZE];
	struct mempool_info *mi;

	if (!rte_mempool_unimsg_ring) {
		RTE_LOG(ERR, MBUF, "Using Unimsg mempool requires first setting"
			"a ring in rte_mempool_unimsg_ring\n");
		rte_errno = EINVAL;
		return -rte_errno;
	}

	ret = snprintf(rg_name, sizeof(rg_name), "unimsg_%s", mp->name);
	if (ret < 0 || ret >= (int)sizeof(rg_name)) {
		rte_errno = ENAMETOOLONG;
		return -rte_errno;
	}

	mi = rte_zmalloc_socket(rg_name, sizeof(*mi), 0, mp->socket_id);
	if (!mi)
		return -ENOMEM;
	mi->r = rte_mempool_unimsg_ring;
	mi->tot_esize = mp->header_size + mp->elt_size + mp->trailer_size;
	mi->hdr_size = mp->header_size;

	mp->pool_data = mi;

	return 0;
}

static void
unimsg_free(struct rte_mempool *mp __rte_unused)
{
	/* Nothing to do here, the manager will free the memory */
}

static const struct rte_mempool_ops ops = {
	.name = "unimsg",
	.alloc = unimsg_alloc,
	.free = unimsg_free,
	.enqueue = unimsg_enqueue,
	.dequeue = unimsg_dequeue,
	.get_count = unimsg_get_count,
};
RTE_MEMPOOL_REGISTER_OPS(ops);
