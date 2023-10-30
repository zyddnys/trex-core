/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2022 NVIDIA Corporation & Affiliates
 */

#include "mlx5dr_internal.h"

static void mlx5dr_table_init_next_ft_attr(struct mlx5dr_table *tbl,
					   struct mlx5dr_cmd_ft_create_attr *ft_attr)
{
	ft_attr->type = tbl->fw_ft_type;
	if (tbl->type == MLX5DR_TABLE_TYPE_FDB)
		ft_attr->level = tbl->ctx->caps->fdb_ft.max_level - 1;
	else
		ft_attr->level = tbl->ctx->caps->nic_ft.max_level - 1;
	ft_attr->rtc_valid = true;
}

/* Call this under ctx->ctrl_lock */
static int
mlx5dr_table_up_default_fdb_miss_tbl(struct mlx5dr_table *tbl)
{
	struct mlx5dr_cmd_ft_create_attr ft_attr = {0};
	struct mlx5dr_cmd_forward_tbl *default_miss;
	struct mlx5dr_context *ctx = tbl->ctx;
	uint8_t tbl_type = tbl->type;
	uint32_t vport;

	if (tbl->type != MLX5DR_TABLE_TYPE_FDB)
		return 0;

	if (ctx->common_res[tbl_type].default_miss) {
		ctx->common_res[tbl_type].default_miss->refcount++;
		return 0;
	}

	ft_attr.type = tbl->fw_ft_type;
	ft_attr.level = tbl->ctx->caps->fdb_ft.max_level; /* The last level */
	ft_attr.rtc_valid = false;

	assert(ctx->caps->eswitch_manager);
	vport = ctx->caps->eswitch_manager_vport_number;

	default_miss = mlx5dr_cmd_miss_ft_create(mlx5dr_context_get_local_ibv(ctx),
						 &ft_attr, vport);
	if (!default_miss) {
		DR_LOG(ERR, "Failed to default miss table type: 0x%x", tbl_type);
		return rte_errno;
	}

	ctx->common_res[tbl_type].default_miss = default_miss;
	ctx->common_res[tbl_type].default_miss->refcount++;
	return 0;
}

/* Called under pthread_spin_lock(&ctx->ctrl_lock) */
static void mlx5dr_table_down_default_fdb_miss_tbl(struct mlx5dr_table *tbl)
{
	struct mlx5dr_cmd_forward_tbl *default_miss;
	struct mlx5dr_context *ctx = tbl->ctx;
	uint8_t tbl_type = tbl->type;

	if (tbl->type != MLX5DR_TABLE_TYPE_FDB)
		return;

	default_miss = ctx->common_res[tbl_type].default_miss;
	if (--default_miss->refcount)
		return;

	mlx5dr_cmd_miss_ft_destroy(default_miss);

	simple_free(default_miss);
	ctx->common_res[tbl_type].default_miss = NULL;
}

static int
mlx5dr_table_connect_to_default_miss_tbl(struct mlx5dr_table *tbl,
					 struct mlx5dr_devx_obj *ft)
{
	struct mlx5dr_cmd_ft_modify_attr ft_attr = {0};
	int ret;

	assert(tbl->type == MLX5DR_TABLE_TYPE_FDB);

	mlx5dr_cmd_set_attr_connect_miss_tbl(tbl->ctx,
					     tbl->fw_ft_type,
					     tbl->type,
					     &ft_attr);

	/* Connect to next */
	ret = mlx5dr_cmd_flow_table_modify(ft, &ft_attr);
	if (ret) {
		DR_LOG(ERR, "Failed to connect FT to default FDB FT");
		return errno;
	}

	return 0;
}

struct mlx5dr_devx_obj *
mlx5dr_table_create_default_ft(struct ibv_context *ibv,
			       struct mlx5dr_table *tbl)
{
	struct mlx5dr_cmd_ft_create_attr ft_attr = {0};
	struct mlx5dr_devx_obj *ft_obj;
	int ret;

	mlx5dr_table_init_next_ft_attr(tbl, &ft_attr);

	ft_obj = mlx5dr_cmd_flow_table_create(ibv, &ft_attr);
	if (ft_obj && tbl->type == MLX5DR_TABLE_TYPE_FDB) {
		/* Take/create ref over the default miss */
		ret = mlx5dr_table_up_default_fdb_miss_tbl(tbl);
		if (ret) {
			DR_LOG(ERR, "Failed to get default fdb miss");
			goto free_ft_obj;
		}
		ret = mlx5dr_table_connect_to_default_miss_tbl(tbl, ft_obj);
		if (ret) {
			DR_LOG(ERR, "Failed connecting to default miss tbl");
			goto down_miss_tbl;
		}
	}

	return ft_obj;

down_miss_tbl:
	mlx5dr_table_down_default_fdb_miss_tbl(tbl);
free_ft_obj:
	mlx5dr_cmd_destroy_obj(ft_obj);
	return NULL;
}

static int
mlx5dr_table_init_check_hws_support(struct mlx5dr_context *ctx,
				    struct mlx5dr_table *tbl)
{
	if (!(ctx->flags & MLX5DR_CONTEXT_FLAG_HWS_SUPPORT)) {
		DR_LOG(ERR, "HWS not supported, cannot create mlx5dr_table");
		rte_errno = EOPNOTSUPP;
		return rte_errno;
	}

	if (mlx5dr_context_shared_gvmi_used(ctx) && tbl->type == MLX5DR_TABLE_TYPE_FDB) {
		DR_LOG(ERR, "FDB with shared port resources is not supported");
		rte_errno = EOPNOTSUPP;
		return rte_errno;
	}

	return 0;
}

static int
mlx5dr_table_shared_gvmi_resource_create(struct mlx5dr_context *ctx,
					 enum mlx5dr_table_type type,
					 struct mlx5dr_context_shared_gvmi_res *gvmi_res)
{
	struct mlx5dr_cmd_ft_create_attr ft_attr = {0};
	uint32_t calculated_ft_id;
	int ret;

	if (!mlx5dr_context_shared_gvmi_used(ctx))
		return 0;

	ft_attr.type = mlx5dr_table_get_res_fw_ft_type(type, false);
	ft_attr.level = ctx->caps->nic_ft.max_level - 1;
	ft_attr.rtc_valid = true;

	gvmi_res->end_ft =
		mlx5dr_cmd_flow_table_create(mlx5dr_context_get_local_ibv(ctx),
					     &ft_attr);
	if (!gvmi_res->end_ft) {
		DR_LOG(ERR, "Failed to create end-ft");
		return rte_errno;
	}

	calculated_ft_id =
		mlx5dr_table_get_res_fw_ft_type(type, false) << FT_ID_FT_TYPE_OFFSET;
	calculated_ft_id |= gvmi_res->end_ft->id;

	/* create alias to that FT */
	ret = mlx5dr_matcher_create_aliased_obj(ctx,
						ctx->local_ibv_ctx,
						ctx->ibv_ctx,
						ctx->caps->vhca_id,
						calculated_ft_id,
						MLX5_GENERAL_OBJ_TYPE_FT_ALIAS,
						&gvmi_res->aliased_end_ft);
	if (ret) {
		DR_LOG(ERR, "Failed to create alias end-ft");
		goto free_end_ft;
	}

	return 0;

free_end_ft:
	mlx5dr_cmd_destroy_obj(gvmi_res->end_ft);

	return rte_errno;
}

static void
mlx5dr_table_shared_gvmi_resourse_destroy(struct mlx5dr_context *ctx,
					  struct mlx5dr_context_shared_gvmi_res *gvmi_res)
{
	if (!mlx5dr_context_shared_gvmi_used(ctx))
		return;

	if (gvmi_res->aliased_end_ft) {
		mlx5dr_cmd_destroy_obj(gvmi_res->aliased_end_ft);
		gvmi_res->aliased_end_ft = NULL;
	}
	if (gvmi_res->end_ft) {
		mlx5dr_cmd_destroy_obj(gvmi_res->end_ft);
		gvmi_res->end_ft = NULL;
	}
}

/* called under spinlock ctx->ctrl_lock */
static struct mlx5dr_context_shared_gvmi_res *
mlx5dr_table_get_shared_gvmi_res(struct mlx5dr_context *ctx, enum mlx5dr_table_type type)
{
	int ret;

	if (!mlx5dr_context_shared_gvmi_used(ctx))
		return NULL;

	if (ctx->gvmi_res[type].aliased_end_ft) {
		ctx->gvmi_res[type].refcount++;
		return &ctx->gvmi_res[type];
	}

	ret = mlx5dr_table_shared_gvmi_resource_create(ctx, type, &ctx->gvmi_res[type]);
	if (ret) {
		DR_LOG(ERR, "Failed to create shared gvmi res for type: %d", type);
		goto out;
	}

	ctx->gvmi_res[type].refcount = 1;

	return &ctx->gvmi_res[type];

out:
	return NULL;
}

/* called under spinlock ctx->ctrl_lock */
static void mlx5dr_table_put_shared_gvmi_res(struct mlx5dr_table *tbl)
{
	struct mlx5dr_context *ctx = tbl->ctx;

	if (!mlx5dr_context_shared_gvmi_used(ctx))
		return;

	if (--ctx->gvmi_res[tbl->type].refcount)
		return;

	mlx5dr_table_shared_gvmi_resourse_destroy(ctx, &ctx->gvmi_res[tbl->type]);
}

static void mlx5dr_table_uninit_shared_ctx_res(struct mlx5dr_table *tbl)
{
	struct mlx5dr_context *ctx = tbl->ctx;

	if (!mlx5dr_context_shared_gvmi_used(ctx))
		return;

	mlx5dr_cmd_destroy_obj(tbl->local_ft);

	mlx5dr_table_put_shared_gvmi_res(tbl);
}

/* called under spin_lock ctx->ctrl_lock */
static int mlx5dr_table_init_shared_ctx_res(struct mlx5dr_context *ctx, struct mlx5dr_table *tbl)
{
	struct mlx5dr_cmd_ft_modify_attr ft_attr = {0};
	int ret;

	if (!mlx5dr_context_shared_gvmi_used(ctx))
		return 0;

	/* create local-ft for root access */
	tbl->local_ft =
		mlx5dr_table_create_default_ft(mlx5dr_context_get_local_ibv(ctx), tbl);
	if (!tbl->local_ft) {
		DR_LOG(ERR, "Failed to create local-ft");
		return rte_errno;
	}

	if (!mlx5dr_table_get_shared_gvmi_res(tbl->ctx, tbl->type)) {
		DR_LOG(ERR, "Failed to shared gvmi resources");
		goto clean_local_ft;
	}

	/* On shared gvmi the default behavior is jump to alias end ft */
	mlx5dr_cmd_set_attr_connect_miss_tbl(tbl->ctx,
					     tbl->fw_ft_type,
					     tbl->type,
					     &ft_attr);

	ret = mlx5dr_cmd_flow_table_modify(tbl->ft, &ft_attr);
	if (ret) {
		DR_LOG(ERR, "Failed to point table to its default miss");
		goto clean_shared_res;
	}

	return 0;

clean_shared_res:
	mlx5dr_table_put_shared_gvmi_res(tbl);
clean_local_ft:
	mlx5dr_table_destroy_default_ft(tbl, tbl->local_ft);
	return rte_errno;
}

void mlx5dr_table_destroy_default_ft(struct mlx5dr_table *tbl,
				     struct mlx5dr_devx_obj *ft_obj)
{
	mlx5dr_cmd_destroy_obj(ft_obj);
	mlx5dr_table_down_default_fdb_miss_tbl(tbl);
}

static int mlx5dr_table_init(struct mlx5dr_table *tbl)
{
	struct mlx5dr_context *ctx = tbl->ctx;
	int ret;

	if (mlx5dr_table_is_root(tbl))
		return 0;

	ret = mlx5dr_table_init_check_hws_support(ctx, tbl);
	if (ret)
		return ret;

	switch (tbl->type) {
	case MLX5DR_TABLE_TYPE_NIC_RX:
		tbl->fw_ft_type = FS_FT_NIC_RX;
		break;
	case MLX5DR_TABLE_TYPE_NIC_TX:
		tbl->fw_ft_type = FS_FT_NIC_TX;
		break;
	case MLX5DR_TABLE_TYPE_FDB:
		tbl->fw_ft_type = FS_FT_FDB;
		break;
	default:
		assert(0);
		break;
	}

	pthread_spin_lock(&ctx->ctrl_lock);
	tbl->ft = mlx5dr_table_create_default_ft(tbl->ctx->ibv_ctx, tbl);
	if (!tbl->ft) {
		DR_LOG(ERR, "Failed to create flow table devx object");
		pthread_spin_unlock(&ctx->ctrl_lock);
		return rte_errno;
	}

	ret = mlx5dr_table_init_shared_ctx_res(ctx, tbl);
	if (ret)
		goto tbl_destroy;

	ret = mlx5dr_action_get_default_stc(ctx, tbl->type);
	if (ret)
		goto free_shared_ctx;

	pthread_spin_unlock(&ctx->ctrl_lock);

	return 0;

free_shared_ctx:
	mlx5dr_table_uninit_shared_ctx_res(tbl);
tbl_destroy:
	mlx5dr_table_destroy_default_ft(tbl, tbl->ft);
	pthread_spin_unlock(&ctx->ctrl_lock);
	return rte_errno;
}

static void mlx5dr_table_uninit(struct mlx5dr_table *tbl)
{
	if (mlx5dr_table_is_root(tbl))
		return;
	pthread_spin_lock(&tbl->ctx->ctrl_lock);
	mlx5dr_action_put_default_stc(tbl->ctx, tbl->type);
	mlx5dr_table_uninit_shared_ctx_res(tbl);
	mlx5dr_table_destroy_default_ft(tbl, tbl->ft);
	pthread_spin_unlock(&tbl->ctx->ctrl_lock);
}

struct mlx5dr_table *mlx5dr_table_create(struct mlx5dr_context *ctx,
					 struct mlx5dr_table_attr *attr)
{
	struct mlx5dr_table *tbl;
	int ret;

	if (attr->type > MLX5DR_TABLE_TYPE_FDB) {
		DR_LOG(ERR, "Invalid table type %d", attr->type);
		return NULL;
	}

	tbl = simple_malloc(sizeof(*tbl));
	if (!tbl) {
		rte_errno = ENOMEM;
		return NULL;
	}

	tbl->ctx = ctx;
	tbl->type = attr->type;
	tbl->level = attr->level;
	LIST_INIT(&tbl->head);

	ret = mlx5dr_table_init(tbl);
	if (ret) {
		DR_LOG(ERR, "Failed to initialise table");
		goto free_tbl;
	}

	pthread_spin_lock(&ctx->ctrl_lock);
	LIST_INSERT_HEAD(&ctx->head, tbl, next);
	pthread_spin_unlock(&ctx->ctrl_lock);

	return tbl;

free_tbl:
	simple_free(tbl);
	return NULL;
}

int mlx5dr_table_destroy(struct mlx5dr_table *tbl)
{
	struct mlx5dr_context *ctx = tbl->ctx;

	pthread_spin_lock(&ctx->ctrl_lock);
	LIST_REMOVE(tbl, next);
	pthread_spin_unlock(&ctx->ctrl_lock);
	mlx5dr_table_uninit(tbl);
	simple_free(tbl);

	return 0;
}
