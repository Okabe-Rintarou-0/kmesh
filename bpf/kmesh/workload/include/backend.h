/* SPDX-License-Identifier: (GPL-2.0-only OR BSD-2-Clause) */
/* Copyright Authors of Kmesh */

#ifndef __ROUTE_BACKEND_H__
#define __ROUTE_BACKEND_H__

#include "workload_common.h"
#include "encoder.h"
#include "tail_call.h"

#define TAIL_CALL_CONNECT4_INDEX 0

static inline backend_value *map_lookup_backend(const backend_key *key)
{
    return kmesh_map_lookup_elem(&map_of_backend, key);
}

static inline int waypoint_manager(ctx_buff_t *ctx, struct ctx_info *info, struct ip_addr *wp_addr, __u32 port)
{
    int ret;
    address_t target_addr;
    __u64 *sk = (__u64 *)ctx->sk;
    struct bpf_sock_tuple value_tuple = {0};

    if (ctx->family == AF_INET) {
        value_tuple.ipv4.daddr = info->vip.ip4;
        value_tuple.ipv4.dport = ctx->user_port;
    } else {
        bpf_memcpy(value_tuple.ipv6.daddr, info->vip.ip6, IPV6_ADDR_LEN);
        value_tuple.ipv6.dport = ctx->user_port;
    }
    ret = bpf_map_update_elem(&map_of_dst_info, &sk, &value_tuple, BPF_NOEXIST);
    if (ret) {
        BPF_LOG(ERR, BACKEND, "record metadata origin address and port failed, ret is %d\n", ret);
        return ret;
    }

    if (ctx->user_family == AF_INET)
        info->dnat_ip.ip4 = wp_addr->ip4;
    else
        bpf_memcpy(info->dnat_ip.ip6, wp_addr->ip6, IPV6_ADDR_LEN);
    info->dnat_port = port;
    info->via_waypoint = true;
    return 0;
}

static inline int backend_manager(ctx_buff_t *ctx, struct ctx_info *info, backend_value *backend_v, __u32 service_id, service_value *service_v)
{
    int ret;
    __u32 user_port = ctx->user_port;

    if (backend_v->wp_addr.ip4 != 0 && backend_v->waypoint_port != 0) {
        BPF_LOG(
            DEBUG,
            BACKEND,
            "find waypoint addr=[%pI4h:%u]",
            &backend_v->wp_addr.ip4,
            bpf_ntohs(backend_v->waypoint_port));
        ret = waypoint_manager(ctx, info, &backend_v->wp_addr, backend_v->waypoint_port);
        if (ret == -ENOEXEC) {
            BPF_LOG(ERR, BACKEND, "waypoint_manager failed, ret:%d\n", ret);
            return ret;
        }
    }

#pragma unroll
    for (__u32 i = 0; i < backend_v->service_count; i++) {
        if (i >= MAX_PORT_COUNT) {
            BPF_LOG(WARN, BACKEND, "exceed the max port count:%d", MAX_PORT_COUNT);
            return -EINVAL;
        }
        if (service_id == backend_v->service[i]) {
            BPF_LOG(DEBUG, BACKEND, "access the backend by service:%u\n", service_id);
#pragma unroll
            for (__u32 j = 0; j < MAX_PORT_COUNT; j++) {
                if (user_port == service_v->service_port[j]) {
                    if (ctx->user_family == AF_INET)
                        info->dnat_ip.ip4 = backend_v->addr.ip4;
                    else
                        bpf_memcpy(info->dnat_ip.ip6, backend_v->addr.ip6, IPV6_ADDR_LEN);
                    info->dnat_port = service_v->target_port[j];
                    info->via_waypoint = false;
                    BPF_LOG(
                        DEBUG,
                        BACKEND,
                        "get the backend addr=[%pI4h:%u]",
                        &info->dnat_ip.ip4,
                        bpf_ntohs(info->dnat_port));
                    return 0;
                }
            }
        }
    }

    BPF_LOG(ERR, BACKEND, "failed to get the backend\n");
    return -ENOENT;
}

#endif
