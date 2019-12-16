/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright 2019 Mellanox Technologies, Ltd
 */
#include <string.h>
#include <assert.h>

#include <rte_malloc.h>
#include <rte_errno.h>

#include "mlx5_vdpa_utils.h"
#include "mlx5_vdpa.h"


static int
mlx5_vdpa_virtq_unset(struct mlx5_vdpa_virtq *virtq)
{
	int ret __rte_unused;
	unsigned i;

	if (virtq->virtq) {
		ret = mlx5_devx_cmd_destroy(virtq->virtq);
		assert(!ret);
		virtq->virtq = NULL;
	}
	for (i = 0; i < RTE_DIM(virtq->umems); ++i) {
		if (virtq->umems[i].obj) {
			ret = mlx5_glue->devx_umem_dereg(virtq->umems[i].obj);
			assert(!ret);
		}
		if (virtq->umems[i].buf)
			rte_free(virtq->umems[i].buf);
	}
	memset(&virtq->umems, 0, sizeof(virtq->umems));
	if (virtq->cq.cq)
		mlx5_vdpa_cq_destroy(&virtq->cq);
	return 0;
}

void
mlx5_vdpa_virtqs_release(struct mlx5_vdpa_priv *priv)
{
	struct mlx5_vdpa_virtq *entry;
	struct mlx5_vdpa_virtq *next;
	int ret __rte_unused;

	entry = SLIST_FIRST(&priv->virtq_list);
	while (entry) {
		next = SLIST_NEXT(entry, next);
		mlx5_vdpa_virtq_unset(entry);
		SLIST_REMOVE(&priv->virtq_list, entry, mlx5_vdpa_virtq, next);
		rte_free(entry);
		entry = next;
	}
	SLIST_INIT(&priv->virtq_list);
	if (priv->tis) {
		ret = mlx5_devx_cmd_destroy(priv->tis);
		assert(!ret);
		priv->tis = NULL;
	}
	if (priv->td) {
		ret = mlx5_devx_cmd_destroy(priv->td);
		assert(!ret);
		priv->td = NULL;
	}
	priv->features = 0;
}

int
mlx5_vdpa_virtq_state_modify(struct mlx5_vdpa_virtq *virtq, int state)
{
	struct mlx5_devx_virtq_attr attr = {
			.type = MLX5_VIRTQ_MODIFY_TYPE_STATE,
			.state = state ? MLX5_VIRTQ_STATE_RDY :
					 MLX5_VIRTQ_STATE_SUSPEND,
			.queue_index = virtq->index,
	};

	if (virtq->state == state)
		return 0;
	if (mlx5_devx_cmd_modify_virtq(virtq->virtq, &attr)) {
		DRV_LOG(ERR, "Failed to modify virtq %d state to %u.",
			virtq->index, state);
		return -1;
	}
	virtq->state = state;
	return 0;
}

static uint64_t
mlx5_vdpa_hva_to_gpa(struct rte_vhost_memory *mem, uint64_t hva)
{
	struct rte_vhost_mem_region *reg;
	uint32_t i;
	uint64_t gpa = 0;

	for (i = 0; i < mem->nregions; i++) {
		reg = &mem->regions[i];
		if (hva >= reg->host_user_addr &&
		    hva < reg->host_user_addr + reg->size) {
			gpa = hva - reg->host_user_addr + reg->guest_phys_addr;
			break;
		}
	}
	return gpa;
}

static int
mlx5_vdpa_virtq_setup(struct mlx5_vdpa_priv *priv,
		      struct mlx5_vdpa_virtq *virtq, int index)
{
	struct rte_vhost_vring vq;
	struct mlx5_devx_virtq_attr attr = {0};
	uint64_t gpa;
	int ret;
	unsigned i;
	uint16_t last_avail_idx;
	uint16_t last_used_idx;

	ret = rte_vhost_get_vhost_vring(priv->vid, index, &vq);
	if (ret)
		return -1;
	virtq->index = index;
	attr.tso_ipv4 = !!(priv->features & (1ULL << VIRTIO_NET_F_HOST_TSO4));
	attr.tso_ipv6 = !!(priv->features & (1ULL << VIRTIO_NET_F_HOST_TSO6));
	attr.tx_csum = !!(priv->features & (1ULL << VIRTIO_NET_F_CSUM));
	attr.rx_csum = !!(priv->features & (1ULL << VIRTIO_NET_F_GUEST_CSUM));
	attr.virtio_version_1_0 = !!(priv->features & (1ULL <<
							VIRTIO_F_VERSION_1));
	attr.type = (priv->features & (1ULL << VIRTIO_F_RING_PACKED)) ?
			MLX5_VIRTQ_TYPE_PACKED : MLX5_VIRTQ_TYPE_SPLIT;
	/*
	 * No need CQ creation when the guest in poll mode or when the
	 * capability allows it.
	 */
	attr.event_mode = vq.callfd != -1 || !(priv->caps.event_mode & (1 <<
					       MLX5_VIRTQ_EVENT_MODE_NO_MSIX)) ?
						      MLX5_VIRTQ_EVENT_MODE_CQ :
						  MLX5_VIRTQ_EVENT_MODE_NO_MSIX;
	if (attr.event_mode == MLX5_VIRTQ_EVENT_MODE_CQ) {
		ret = mlx5_vdpa_cq_create(priv, vq.size, vq.callfd,
					  &virtq->cq);
		if (ret) {
			DRV_LOG(ERR, "Failed to create CQ for virtq %d.",
				index);
			return -1;
		}
		attr.cq_id = virtq->cq.cq->id;
	} else {
		DRV_LOG(INFO, "Virtq %d is, for sure, working by poll mode, no"
			" need CQ and event mechanism.", index);
	}
	/* Setup 3 UMEMs for each virtq. */
	for (i = 0; i < RTE_DIM(virtq->umems); ++i) {
		virtq->umems[i].size = priv->caps.umems[i].a * vq.size +
							  priv->caps.umems[i].b;
		assert(virtq->umems[i].size);
		virtq->umems[i].buf = rte_zmalloc(__func__,
						  virtq->umems[i].size, 4096);
		if (!virtq->umems[i].buf) {
			DRV_LOG(ERR, "Cannot allocate umem %d memory for virtq"
				" %u.", i, index);
			goto error;
		}
		virtq->umems[i].obj = mlx5_glue->devx_umem_reg(priv->ctx,
							virtq->umems[i].buf,
							virtq->umems[i].size,
							IBV_ACCESS_LOCAL_WRITE);
		if (!virtq->umems[i].obj) {
			DRV_LOG(ERR, "Failed to register umem %d for virtq %u.",
				i, index);
			goto error;
		}
		attr.umems[i].id = virtq->umems[i].obj->umem_id;
		attr.umems[i].offset = 0;
		attr.umems[i].size = virtq->umems[i].size;
	}
	if (attr.type == MLX5_VIRTQ_TYPE_SPLIT) {
		gpa = mlx5_vdpa_hva_to_gpa(priv->vmem,
					   (uint64_t)(uintptr_t)vq.desc);
		if (!gpa) {
			DRV_LOG(ERR, "Failed to get descriptor ring GPA.");
			goto error;
		}
		attr.desc_addr = gpa;
		gpa = mlx5_vdpa_hva_to_gpa(priv->vmem,
					   (uint64_t)(uintptr_t)vq.used);
		if (!gpa) {
			DRV_LOG(ERR, "Failed to get GPA for used ring.");
			goto error;
		}
		attr.used_addr = gpa;
		gpa = mlx5_vdpa_hva_to_gpa(priv->vmem,
					   (uint64_t)(uintptr_t)vq.avail);
		if (!gpa) {
			DRV_LOG(ERR, "Failed to get GPA for available ring.");
			goto error;
		}
		attr.available_addr = gpa;
	}
	rte_vhost_get_vring_base(priv->vid, index, &last_avail_idx,
				 &last_used_idx);
	DRV_LOG(INFO, "vid %d: Init last_avail_idx=%d, last_used_idx=%d for "
		"virtq %d.", priv->vid, last_avail_idx, last_used_idx, index);
	attr.hw_available_index = last_avail_idx;
	attr.hw_used_index = last_used_idx;
	attr.q_size = vq.size;
	attr.mkey = priv->gpa_mkey_index;
	attr.tis_id = priv->tis->id;
	attr.queue_index = index;
	virtq->virtq = mlx5_devx_cmd_create_virtq(priv->ctx, &attr);
	if (!virtq->virtq)
		goto error;
	if (mlx5_vdpa_virtq_state_modify(virtq, 1))
		goto error;
	return 0;
error:
	mlx5_vdpa_virtq_unset(virtq);
	return -1;
}

static int
mlx5_vdpa_features_validate(struct mlx5_vdpa_priv *priv)
{
	if (priv->features & (1ULL << VIRTIO_F_RING_PACKED)) {
		if (!(priv->caps.virtio_queue_type & (1 <<
						     MLX5_VIRTQ_TYPE_PACKED))) {
			DRV_LOG(ERR, "Failed to configur PACKED mode for vdev "
				"%d - it was not reported by HW/driver"
				" capability.", priv->vid);
			return -ENOTSUP;
		}
	}
	if (priv->features & (1ULL << VIRTIO_NET_F_HOST_TSO4)) {
		if (!priv->caps.tso_ivp4) {
			DRV_LOG(ERR, "Failed to enable TSO4 for vdev %d - TSO4"
				" was not reported by HW/driver capability.",
				priv->vid);
			return -ENOTSUP;
		}
	}
	if (priv->features & (1ULL << VIRTIO_NET_F_HOST_TSO6)) {
		if (!priv->caps.tso_ivp6) {
			DRV_LOG(ERR, "Failed to enable TSO6 for vdev %d - TSO6"
				" was not reported by HW/driver capability.",
				priv->vid);
			return -ENOTSUP;
		}
	}
	if (priv->features & (1ULL << VIRTIO_NET_F_CSUM)) {
		if (!priv->caps.tx_csum) {
			DRV_LOG(ERR, "Failed to enable CSUM for vdev %d - CSUM"
				" was not reported by HW/driver capability.",
				priv->vid);
			return -ENOTSUP;
		}
	}
	if (priv->features & (1ULL << VIRTIO_NET_F_GUEST_CSUM)) {
		if (!priv->caps.rx_csum) {
			DRV_LOG(ERR, "Failed to enable GUEST CSUM for vdev %d"
				" GUEST CSUM was not reported by HW/driver "
				"capability.", priv->vid);
			return -ENOTSUP;
		}
	}
	if (priv->features & (1ULL << VIRTIO_F_VERSION_1)) {
		if (!priv->caps.virtio_version_1_0) {
			DRV_LOG(ERR, "Failed to enable version 1 for vdev %d "
				"version 1 was not reported by HW/driver"
				" capability.", priv->vid);
			return -ENOTSUP;
		}
	}
	return 0;
}

int
mlx5_vdpa_virtqs_prepare(struct mlx5_vdpa_priv *priv)
{
	struct mlx5_devx_tis_attr tis_attr = {0};
	struct mlx5_vdpa_virtq *virtq;
	uint32_t i;
	uint16_t nr_vring = rte_vhost_get_vring_num(priv->vid);
	int ret = rte_vhost_get_negotiated_features(priv->vid, &priv->features);

	if (ret || mlx5_vdpa_features_validate(priv)) {
		DRV_LOG(ERR, "Failed to configure negotiated features.");
		return -1;
	}
	priv->td = mlx5_devx_cmd_create_td(priv->ctx);
	if (!priv->td) {
		DRV_LOG(ERR, "Failed to create transpprt domain.");
		return -rte_errno;
	}
	tis_attr.transport_domain = priv->td->id;
	priv->tis = mlx5_devx_cmd_create_tis(priv->ctx, &tis_attr);
	if (!priv->tis) {
		DRV_LOG(ERR, "Failed to create TIS.");
		goto error;
	}
	for (i = 0; i < nr_vring; i++) {
		virtq = rte_zmalloc(__func__, sizeof(*virtq), 0);
		if (!virtq || mlx5_vdpa_virtq_setup(priv, virtq, i)) {
			if (virtq)
				rte_free(virtq);
			goto error;
		}
		SLIST_INSERT_HEAD(&priv->virtq_list, virtq, next);
	}
	priv->nr_virtqs = nr_vring;
	return 0;
error:
	mlx5_vdpa_virtqs_release(priv);
	return -1;
}
