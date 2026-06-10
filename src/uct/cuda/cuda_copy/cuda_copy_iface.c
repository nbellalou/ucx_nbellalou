/**
 * Copyright (c) NVIDIA CORPORATION & AFFILIATES, 2017-2019. ALL RIGHTS RESERVED.
 * See file LICENSE for terms.
 */

#ifdef HAVE_CONFIG_H
#  include "config.h"
#endif

#include "cuda_copy_iface.h"
#include "cuda_copy_md.h"
#include "cuda_copy_ep.h"

#include <uct/cuda/base/cuda_iface.h>
#include <uct/cuda/base/cuda_md.h>
#include <ucs/config/parser.h>
#include <ucs/type/class.h>
#include <ucs/sys/math.h>
#include <ucs/sys/string.h>
#include <ucs/async/eventfd.h>
#include <ucs/arch/cpu.h>


#define UCT_CUDA_COPY_IFACE_OVERHEAD 0
#define UCT_CUDA_COPY_IFACE_LATENCY  ucs_linear_func_make(8e-6, 0)

#define UCT_CUDA_COPY_FALLBACK_H2D_BW (8300.0 * UCS_MBYTE)
#define UCT_CUDA_COPY_FALLBACK_D2H_BW (11660.0 * UCS_MBYTE)
#define UCT_CUDA_COPY_PINNED_HOST_BW  (48.0 * UCS_GBYTE)


static ucs_config_field_t uct_cuda_copy_iface_config_table[] = {

    {"", "", NULL,
     ucs_offsetof(uct_cuda_copy_iface_config_t, super),
     UCS_CONFIG_TYPE_TABLE(uct_iface_config_table)},

    {"MAX_POLL", "16",
     "Max number of event completions to pick during cuda events polling",
     ucs_offsetof(uct_cuda_copy_iface_config_t, max_poll), UCS_CONFIG_TYPE_UINT},

    {"MAX_EVENTS", "inf",
     "Max number of cuda events. -1 is infinite",
     ucs_offsetof(uct_cuda_copy_iface_config_t, max_cuda_events), UCS_CONFIG_TYPE_UINT},

    /* TODO: 1. Add separate keys for shared and dedicated bandwidth
             2. Remove the "dflt" key (use pref_loc for managed memory) */
    {"BW", "10000MBs,h2d:auto,d2h:auto,d2d:320GBs",
     "Effective memory bandwidth. h2d/d2h 'auto' uses a conservative "
     "topology-aware model; explicit values keep legacy node-shared scope", 0,
     UCS_CONFIG_TYPE_KEY_VALUE(UCS_CONFIG_TYPE_BW,
         {"h2d", "host to device bandwidth",
          ucs_offsetof(uct_cuda_copy_iface_config_t, bw.h2d)},
         {"d2h", "device to host bandwidth",
          ucs_offsetof(uct_cuda_copy_iface_config_t, bw.d2h)},
         {"d2d", "device to device bandwidth",
          ucs_offsetof(uct_cuda_copy_iface_config_t, bw.d2d)},
         {"default", "any other memory types combinations bandwidth",
          ucs_offsetof(uct_cuda_copy_iface_config_t, bw.dflt)},
         {NULL})},

    {NULL}
};

static int
uct_cuda_copy_mem_is_host(ucs_memory_type_t mem_type)
{
    return mem_type == UCS_MEMORY_TYPE_HOST;
}

static int
uct_cuda_copy_mem_is_cuda(ucs_memory_type_t mem_type)
{
    return mem_type == UCS_MEMORY_TYPE_CUDA;
}

static double
uct_cuda_copy_legacy_dir_bw(ucs_memory_type_t src_mem_type,
                            ucs_memory_type_t dst_mem_type)
{
    if (uct_cuda_copy_mem_is_cuda(src_mem_type) &&
        uct_cuda_copy_mem_is_host(dst_mem_type)) {
        return UCT_CUDA_COPY_FALLBACK_D2H_BW;
    }

    return UCT_CUDA_COPY_FALLBACK_H2D_BW;
}

static int
uct_cuda_copy_perf_attr_memory_class(const uct_perf_attr_t *perf_attr,
                                     int local,
                                     uct_perf_attr_memory_class_t *mem_class_p)
{
    if (local) {
        if (!(perf_attr->field_mask & UCT_PERF_ATTR_FIELD_LOCAL_MEMORY_CLASS)) {
            return 0;
        }

        *mem_class_p = perf_attr->local_memory_class;
    } else {
        if (!(perf_attr->field_mask & UCT_PERF_ATTR_FIELD_REMOTE_MEMORY_CLASS)) {
            return 0;
        }

        *mem_class_p = perf_attr->remote_memory_class;
    }

    return 1;
}

static ucs_sys_device_t
uct_cuda_copy_perf_attr_sys_dev(const uct_perf_attr_t *perf_attr, int local)
{
    if (local) {
        return UCT_ATTR_VALUE(PERF, perf_attr, local_sys_device,
                              LOCAL_SYS_DEVICE, UCS_SYS_DEVICE_ID_UNKNOWN);
    }

    return UCT_ATTR_VALUE(PERF, perf_attr, remote_sys_device,
                          REMOTE_SYS_DEVICE, UCS_SYS_DEVICE_ID_UNKNOWN);
}

static ucs_status_t
uct_cuda_copy_auto_dir_bw(const uct_perf_attr_t *perf_attr,
                          ucs_memory_type_t src_mem_type,
                          ucs_memory_type_t dst_mem_type, int src_is_local,
                          int dst_is_local, double *bw_p,
                          uct_perf_attr_bandwidth_scope_t *scope_p,
                          ucs_sys_device_t *scope_id_p)
{
    const int src_is_cuda = uct_cuda_copy_mem_is_cuda(src_mem_type);
    const int dst_is_cuda = uct_cuda_copy_mem_is_cuda(dst_mem_type);
    const int src_is_host = uct_cuda_copy_mem_is_host(src_mem_type);
    const int dst_is_host = uct_cuda_copy_mem_is_host(dst_mem_type);
    uct_perf_attr_memory_class_t host_mem_class;
    ucs_sys_device_t cuda_sys_dev;
    int host_is_local, cuda_is_local;
    double pcie_bw;

    if (!((src_is_cuda && dst_is_host) || (src_is_host && dst_is_cuda))) {
        return UCS_ERR_UNSUPPORTED;
    }

    host_is_local = src_is_host ? src_is_local : dst_is_local;
    cuda_is_local = src_is_cuda ? src_is_local : dst_is_local;

    cuda_sys_dev = uct_cuda_copy_perf_attr_sys_dev(perf_attr, cuda_is_local);
    if (!uct_cuda_copy_perf_attr_memory_class(perf_attr, host_is_local,
                                              &host_mem_class)) {
        return UCS_ERR_UNSUPPORTED;
    }

    if ((cuda_sys_dev == UCS_SYS_DEVICE_ID_UNKNOWN) ||
        (host_mem_class == UCT_PERF_ATTR_MEMORY_CLASS_UNKNOWN)) {
        return UCS_ERR_UNSUPPORTED;
    }

    if ((host_mem_class != UCT_PERF_ATTR_MEMORY_CLASS_INTERNAL) &&
        (host_mem_class != UCT_PERF_ATTR_MEMORY_CLASS_REGISTERED)) {
        return UCS_ERR_UNSUPPORTED;
    }

    if (ucs_topo_get_sys_device_pci_bw(cuda_sys_dev, &pcie_bw) != UCS_OK) {
        return UCS_ERR_UNSUPPORTED;
    }

    /* Keep the default below the finite PCIe and pinned-host caps. */
    *bw_p        = ucs_min(pcie_bw, UCT_CUDA_COPY_PINNED_HOST_BW);
    *bw_p       *= src_is_host ? 0.78 : 0.82;
    *scope_p     = UCT_PERF_ATTR_BW_SCOPE_ACCELERATOR;
    *scope_id_p  = cuda_sys_dev;

    return UCS_OK;
}

/* Forward declaration for the delete function */
static void UCS_CLASS_DELETE_FUNC_NAME(uct_cuda_copy_iface_t)(uct_iface_t*);


static ucs_status_t uct_cuda_copy_iface_get_address(uct_iface_h tl_iface,
                                                    uct_iface_addr_t *iface_addr)
{
    uct_cuda_copy_iface_t *iface = ucs_derived_of(tl_iface, uct_cuda_copy_iface_t);

    *(uct_cuda_copy_iface_addr_t*)iface_addr = iface->id;
    return UCS_OK;
}

static int uct_cuda_copy_iface_is_reachable_v2(
        const uct_iface_h tl_iface,
        const uct_iface_is_reachable_params_t *params)
{
    uct_cuda_copy_iface_t *iface = ucs_derived_of(tl_iface,
                                                  uct_cuda_copy_iface_t);
    uct_cuda_copy_iface_addr_t *addr;

    if (!uct_iface_is_reachable_params_addrs_valid(params)) {
        return 0;
    }

    addr = (uct_cuda_copy_iface_addr_t*)params->iface_addr;
    if (addr == NULL) {
        uct_iface_fill_info_str_buf(params, "device address is empty");
        return 0;
    }

    if (iface->id != *addr) {
        uct_iface_fill_info_str_buf(params, "iface mismatch");
        return 0;
    }

    return uct_iface_scope_is_reachable(tl_iface, params);
}

static ucs_status_t uct_cuda_copy_iface_query(uct_iface_h tl_iface,
                                              uct_iface_attr_t *iface_attr)
{
    uct_cuda_copy_iface_t *iface = ucs_derived_of(tl_iface, uct_cuda_copy_iface_t);

    uct_base_iface_query(&iface->super.super, iface_attr);

    iface_attr->iface_addr_len          = sizeof(uct_cuda_copy_iface_addr_t);
    iface_attr->device_addr_len         = 0;
    iface_attr->ep_addr_len             = 0;
    iface_attr->cap.flags               = UCT_IFACE_FLAG_CONNECT_TO_IFACE |
                                          UCT_IFACE_FLAG_GET_SHORT |
                                          UCT_IFACE_FLAG_PUT_SHORT |
                                          UCT_IFACE_FLAG_GET_ZCOPY |
                                          UCT_IFACE_FLAG_PUT_ZCOPY |
                                          UCT_IFACE_FLAG_PENDING;

    iface_attr->cap.event_flags         = UCT_IFACE_FLAG_EVENT_SEND_COMP |
                                          UCT_IFACE_FLAG_EVENT_RECV      |
                                          UCT_IFACE_FLAG_EVENT_FD;

    iface_attr->cap.put.max_short       = UINT_MAX;
    iface_attr->cap.put.max_bcopy       = 0;
    iface_attr->cap.put.min_zcopy       = 0;
    iface_attr->cap.put.max_zcopy       = SIZE_MAX;
    iface_attr->cap.put.opt_zcopy_align = 1;
    iface_attr->cap.put.align_mtu       = iface_attr->cap.put.opt_zcopy_align;
    iface_attr->cap.put.max_iov         = 1;

    iface_attr->cap.get.max_short       = UINT_MAX;
    iface_attr->cap.get.max_bcopy       = 0;
    iface_attr->cap.get.min_zcopy       = 0;
    iface_attr->cap.get.max_zcopy       = SIZE_MAX;
    iface_attr->cap.get.opt_zcopy_align = 1;
    iface_attr->cap.get.align_mtu       = iface_attr->cap.get.opt_zcopy_align;
    iface_attr->cap.get.max_iov         = 1;

    iface_attr->cap.am.max_short        = 0;
    iface_attr->cap.am.max_bcopy        = 0;
    iface_attr->cap.am.min_zcopy        = 0;
    iface_attr->cap.am.max_zcopy        = 0;
    iface_attr->cap.am.opt_zcopy_align  = 1;
    iface_attr->cap.am.align_mtu        = iface_attr->cap.am.opt_zcopy_align;
    iface_attr->cap.am.max_hdr          = 0;
    iface_attr->cap.am.max_iov          = 1;

    iface_attr->latency                 = UCT_CUDA_COPY_IFACE_LATENCY;
    iface_attr->bandwidth.dedicated     = 0;
    iface_attr->bandwidth.shared        = iface->config.bw.dflt;
    iface_attr->overhead                = UCT_CUDA_COPY_IFACE_OVERHEAD;
    iface_attr->priority                = 0;

    return UCS_OK;
}

static uct_iface_ops_t uct_cuda_copy_iface_ops = {
    .ep_get_short             = uct_cuda_copy_ep_get_short,
    .ep_put_short             = uct_cuda_copy_ep_put_short,
    .ep_get_zcopy             = uct_cuda_copy_ep_get_zcopy,
    .ep_put_zcopy             = uct_cuda_copy_ep_put_zcopy,
    .ep_pending_add           = (uct_ep_pending_add_func_t)ucs_empty_function_return_busy,
    .ep_pending_purge         = (uct_ep_pending_purge_func_t)ucs_empty_function,
    .ep_flush                 = uct_cuda_base_ep_flush,
    .ep_fence                 = uct_base_ep_fence,
    .ep_create                = UCS_CLASS_NEW_FUNC_NAME(uct_cuda_copy_ep_t),
    .ep_destroy               = UCS_CLASS_DELETE_FUNC_NAME(uct_cuda_copy_ep_t),
    .iface_flush              = uct_cuda_base_iface_flush,
    .iface_fence              = uct_base_iface_fence,
    .iface_progress_enable    = uct_base_iface_progress_enable,
    .iface_progress_disable   = uct_base_iface_progress_disable,
    .iface_progress           = uct_cuda_base_iface_progress,
    .iface_event_fd_get       = uct_cuda_base_iface_event_fd_get,
    .iface_event_arm          = uct_cuda_base_iface_event_fd_arm,
    .iface_close              = UCS_CLASS_DELETE_FUNC_NAME(uct_cuda_copy_iface_t),
    .iface_query              = uct_cuda_copy_iface_query,
    .iface_get_device_address = (uct_iface_get_device_address_func_t)ucs_empty_function_return_success,
    .iface_get_address        = uct_cuda_copy_iface_get_address,
    .iface_is_reachable       = uct_base_iface_is_reachable
};

static ucs_status_t
uct_cuda_copy_estimate_perf(uct_iface_h tl_iface, uct_perf_attr_t *perf_attr)
{
    uct_cuda_copy_iface_t *iface   = ucs_derived_of(tl_iface,
                                                    uct_cuda_copy_iface_t);
    uct_ep_operation_t op          = UCT_ATTR_VALUE(PERF, perf_attr, operation,
                                                    OPERATION, UCT_EP_OP_LAST);
    ucs_memory_type_t src_mem_type = UCT_ATTR_VALUE(PERF, perf_attr,
                                                    local_memory_type,
                                                    LOCAL_MEMORY_TYPE,
                                                    UCS_MEMORY_TYPE_UNKNOWN);
    ucs_memory_type_t dst_mem_type = UCT_ATTR_VALUE(PERF, perf_attr,
                                                    remote_memory_type,
                                                    REMOTE_MEMORY_TYPE,
                                                    UCS_MEMORY_TYPE_UNKNOWN);
    int zcopy                      = uct_ep_op_is_zcopy(op);
    const double latency           = 1.8e-6;
    const double overhead          = 4.0e-6;
    /* stream synchronization factor */
    const double ss_factor         = zcopy ? 1 : 0.95;
    uct_ppn_bandwidth_t bandwidth  = {};
    uct_perf_attr_bandwidth_scope_t bandwidth_scope =
            UCT_PERF_ATTR_BW_SCOPE_UNKNOWN;
    ucs_sys_device_t bandwidth_scope_id = UCS_SYS_DEVICE_ID_UNKNOWN;
    int src_is_local = 1, dst_is_local = 0;

    if ((src_mem_type == UCS_MEMORY_TYPE_HOST) &&
        (dst_mem_type == UCS_MEMORY_TYPE_HOST)) {
        ucs_trace("src_mem_type:%s to dst_mem_type:%s is not supported",
                  ucs_memory_type_names[src_mem_type],
                  ucs_memory_type_names[dst_mem_type]);
        return UCS_ERR_UNSUPPORTED;
    }

    if (uct_perf_attr_has_bandwidth(perf_attr->field_mask)) {
        if (uct_ep_op_is_fetch(op)) {
            ucs_swap(&src_mem_type, &dst_mem_type);
            ucs_swap(&src_is_local, &dst_is_local);
        }

        bandwidth.dedicated = 0;
        if ((src_mem_type == UCS_MEMORY_TYPE_HOST) &&
            (dst_mem_type == UCS_MEMORY_TYPE_CUDA)) {
            if (UCS_CONFIG_DBL_IS_AUTO(iface->config.bw.h2d)) {
                if (uct_cuda_copy_auto_dir_bw(perf_attr, src_mem_type,
                                              dst_mem_type, src_is_local,
                                              dst_is_local, &bandwidth.shared,
                                              &bandwidth_scope,
                                              &bandwidth_scope_id) != UCS_OK) {
                    bandwidth.shared = uct_cuda_copy_legacy_dir_bw(
                            src_mem_type, dst_mem_type);
                }
            } else {
                bandwidth.shared = iface->config.bw.h2d;
                bandwidth_scope  = UCT_PERF_ATTR_BW_SCOPE_NODE;
            }
            bandwidth.shared *= ss_factor;
        } else if ((src_mem_type == UCS_MEMORY_TYPE_CUDA) &&
                   (dst_mem_type == UCS_MEMORY_TYPE_HOST)) {
            if (UCS_CONFIG_DBL_IS_AUTO(iface->config.bw.d2h)) {
                if (uct_cuda_copy_auto_dir_bw(perf_attr, src_mem_type,
                                              dst_mem_type, src_is_local,
                                              dst_is_local, &bandwidth.shared,
                                              &bandwidth_scope,
                                              &bandwidth_scope_id) != UCS_OK) {
                    bandwidth.shared = uct_cuda_copy_legacy_dir_bw(
                            src_mem_type, dst_mem_type);
                }
            } else {
                bandwidth.shared = iface->config.bw.d2h;
                bandwidth_scope  = UCT_PERF_ATTR_BW_SCOPE_NODE;
            }
            bandwidth.shared *= ss_factor;
        } else if ((src_mem_type == UCS_MEMORY_TYPE_CUDA) &&
                   (dst_mem_type == UCS_MEMORY_TYPE_CUDA)) {
            bandwidth.shared = iface->config.bw.d2d;
        } else {
            bandwidth.shared = iface->config.bw.dflt;
        }
    }

    if (perf_attr->field_mask & UCT_PERF_ATTR_FIELD_BANDWIDTH) {
        perf_attr->bandwidth = bandwidth;
    }

    if (perf_attr->field_mask & UCT_PERF_ATTR_FIELD_PATH_BANDWIDTH) {
        perf_attr->path_bandwidth = bandwidth;
    }

    if (perf_attr->field_mask & UCT_PERF_ATTR_FIELD_SEND_PRE_OVERHEAD) {
        perf_attr->send_pre_overhead = overhead;
    }

    if (perf_attr->field_mask & UCT_PERF_ATTR_FIELD_SEND_POST_OVERHEAD) {
        /* In case of sync mem copy, the send operation CPU overhead includes
           the latency of waiting for the copy to complete */
        perf_attr->send_post_overhead = zcopy ? 0 : latency;
    }

    if (perf_attr->field_mask & UCT_PERF_ATTR_FIELD_RECV_OVERHEAD) {
        perf_attr->recv_overhead = 0;
    }

    if (perf_attr->field_mask & UCT_PERF_ATTR_FIELD_LATENCY) {
        /* In case of async mem copy, the latency is not part of the overhead
           and it's a standalone property */
        perf_attr->latency = ucs_linear_func_make(zcopy ? latency : 0.0, 0.0);
    }

    if (perf_attr->field_mask & UCT_PERF_ATTR_FIELD_MAX_INFLIGHT_EPS) {
        perf_attr->max_inflight_eps = SIZE_MAX;
    }

    if (perf_attr->field_mask & UCT_PERF_ATTR_FIELD_FLAGS) {
        perf_attr->flags = 0;
    }

    if (perf_attr->field_mask & UCT_PERF_ATTR_FIELD_BANDWIDTH_SCOPE) {
        perf_attr->bandwidth_scope    = bandwidth_scope;
        perf_attr->bandwidth_scope_id = bandwidth_scope_id;
    }

    return UCS_OK;
}

static uct_iface_internal_ops_t uct_cuda_copy_iface_internal_ops = {
    .iface_query_v2        = uct_iface_base_query_v2,
    .iface_estimate_perf   = uct_cuda_copy_estimate_perf,
    .iface_vfs_refresh     = (uct_iface_vfs_refresh_func_t)ucs_empty_function,
    .ep_query              = (uct_ep_query_func_t)ucs_empty_function_return_unsupported,
    .ep_invalidate         = (uct_ep_invalidate_func_t)ucs_empty_function_return_unsupported,
    .ep_connect_to_ep_v2   = (uct_ep_connect_to_ep_v2_func_t)ucs_empty_function_return_unsupported,
    .iface_is_reachable_v2 = uct_cuda_copy_iface_is_reachable_v2,
    .ep_is_connected       = uct_base_ep_is_connected,
    .ep_get_device_ep      = (uct_ep_get_device_ep_func_t)ucs_empty_function_return_unsupported
};

static uct_cuda_ctx_rsc_t * uct_cuda_copy_ctx_rsc_create(uct_iface_h tl_iface)
{
    uct_cuda_copy_ctx_rsc_t *ctx_rsc;
    ucs_memory_type_t src, dst;

    ctx_rsc = ucs_malloc(sizeof(*ctx_rsc), "uct_cuda_copy_ctx_rsc_t");
    if (ctx_rsc == NULL) {
        ucs_error("failed to allocate cuda copy context resource struct");
        return NULL;
    }

    ucs_memory_type_for_each(src) {
        ucs_memory_type_for_each(dst) {
            uct_cuda_base_queue_desc_init(&ctx_rsc->queue_desc[src][dst]);
        }
    }

    ctx_rsc->short_stream = NULL;
    return &ctx_rsc->super;
}

static void uct_cuda_copy_ctx_rsc_destroy(uct_iface_h tl_iface,
                                          uct_cuda_ctx_rsc_t *cuda_ctx_rsc)
{
    uct_cuda_copy_ctx_rsc_t *ctx_rsc = ucs_derived_of(cuda_ctx_rsc,
                                                      uct_cuda_copy_ctx_rsc_t);
    ucs_memory_type_t src, dst;

    ucs_memory_type_for_each(src) {
        ucs_memory_type_for_each(dst) {
            uct_cuda_base_queue_desc_destroy(cuda_ctx_rsc,
                                             &ctx_rsc->queue_desc[src][dst]);
        }
    }

    uct_cuda_base_stream_destroy(cuda_ctx_rsc, &ctx_rsc->short_stream);
    ucs_free(ctx_rsc);
}

static uct_cuda_iface_ops_t uct_cuda_iface_ops = {
    .create_rsc     = uct_cuda_copy_ctx_rsc_create,
    .destroy_rsc    = uct_cuda_copy_ctx_rsc_destroy,
    .complete_event = (uct_cuda_complete_event_fn_t)ucs_empty_function
};

static UCS_CLASS_INIT_FUNC(uct_cuda_copy_iface_t, uct_md_h md, uct_worker_h worker,
                           const uct_iface_params_t *params,
                           const uct_iface_config_t *tl_config)
{
    uct_cuda_copy_iface_config_t *config = ucs_derived_of(tl_config,
                                                          uct_cuda_copy_iface_config_t);
    ucs_status_t status;

    UCS_CLASS_CALL_SUPER_INIT(uct_cuda_iface_t, &uct_cuda_copy_iface_ops,
                              &uct_cuda_copy_iface_internal_ops, md, worker,
                              params, tl_config, "cuda_copy");

    status = uct_cuda_base_check_device_name(params);
    if (status != UCS_OK) {
        return status;
    }

    self->id                           = ucs_generate_uuid((uintptr_t)self);
    self->config.bw                    = config->bw;
    self->super.ops                    = &uct_cuda_iface_ops;
    self->super.config.max_events      = config->max_cuda_events;
    self->super.config.max_poll        = config->max_poll;
    self->super.config.event_desc_size = sizeof(uct_cuda_event_desc_t);
    UCS_STATIC_BITMAP_RESET_ALL(&self->streams_to_sync);
    return UCS_OK;
}

static UCS_CLASS_CLEANUP_FUNC(uct_cuda_copy_iface_t)
{
}

UCS_CLASS_DEFINE(uct_cuda_copy_iface_t, uct_cuda_iface_t);
UCS_CLASS_DEFINE_NEW_FUNC(uct_cuda_copy_iface_t, uct_iface_t, uct_md_h, uct_worker_h,
                          const uct_iface_params_t*, const uct_iface_config_t*);
static UCS_CLASS_DEFINE_DELETE_FUNC(uct_cuda_copy_iface_t, uct_iface_t);


UCT_TL_DEFINE(&uct_cuda_copy_component, cuda_copy, uct_cuda_base_query_devices,
              uct_cuda_copy_iface_t, "CUDA_COPY_",
              uct_cuda_copy_iface_config_table, uct_cuda_copy_iface_config_t);
