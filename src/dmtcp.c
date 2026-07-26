/**
 * @file dmtcp.c
 * @brief DMOD lifecycle (dmod_init()/_deinit()) and dmtcp_connect() (active open)
 *
 * Everything else lives in the other dmtcp_*.c files - see
 * dmtcp_internal.h's top comment for the full file map.
 */
#include "dmod.h"
#include "dmtcp_internal.h"
#include <errno.h>

dmod_dmtcp_api_declaration(1.0, int, _connect, ( const dmip_addr_t* dst, uint16_t dst_port, uint16_t src_port,
                                                  const dmtcp_conn_callbacks_t* callbacks, void* user_data, dmtcp_conn_t* out_conn ))
{
    if (dst == NULL || dst_port == 0 || out_conn == NULL)
        return -EINVAL;
    if (dst->family == dmip_family_v6)
        return -ENOSYS; /* no dmip_v6_send() yet - see dmtcp.h */
    if (dst->family != dmip_family_v4)
        return -EINVAL;

    uint16_t local_port = src_port;
    if (local_port == 0)
    {
        int result = dmtcp_conn_table_alloc_ephemeral_port(dst, dst_port, &local_port);
        if (result != 0)
            return result;
    }
    else if (dmtcp_conn_table_port_in_use_for_peer(local_port, dst, dst_port))
    {
        return -EADDRINUSE;
    }

    dmip_addr_t local_addr = { 0 };
    int result = dmip_v4_get_source_address(dst, &local_addr);
    if (result != 0)
        return result;

    struct dmtcp_conn* conn = dmtcp_conn_table_create();
    if (conn == NULL)
        return -ENOMEM;

    conn->local_addr = local_addr;
    conn->local_port = local_port;
    conn->peer_addr = *dst;
    conn->peer_port = dst_port;
    conn->iss = dmosi_get_tick_count();
    conn->snd_una = conn->iss;
    conn->snd_nxt = conn->iss + 1u;
    conn->state = dmtcp_state_syn_sent;
    if (callbacks != NULL)
    {
        conn->callbacks = *callbacks;
    }
    conn->user_data = user_data;

    if (dmtcp_conn_table_insert(conn) != 0)
    {
        dmtcp_conn_table_destroy(conn);
        return -EADDRINUSE;
    }

    dmosi_mutex_lock(conn->lock);
    result = dmtcp_send_segment(&conn->local_addr, conn->local_port, &conn->peer_addr, conn->peer_port,
                                 conn->iss, 0, DMTCP_FLAG_SYN, conn->rcv_wnd, NULL, 0, DMTCP_DEFAULT_ARP_TIMEOUT_MS);
    if (result == 0)
    {
        dmtcp_arm_rto(conn);
    }
    dmosi_mutex_unlock(conn->lock);

    if (result != 0)
    {
        /* First SYN attempt failed synchronously - unlike a later RTO retry
         * failure, there's still a caller stack frame to report this to, so
         * tear the just-created TCB down and propagate the error (see
         * dmtcp_connect()'s own doc comment in dmtcp.h). */
        dmtcp_conn_table_remove(conn);
        dmtcp_conn_table_destroy(conn);
        return result;
    }

    *out_conn = conn;
    return 0;
}

int dmod_init(const Dmod_Config_t *Config)
{
    (void)Config;

    if (dmtcp_listen_table_init() != 0 || dmtcp_conn_table_init() != 0)
    {
        DMOD_LOG_ERROR("Failed to allocate dmtcp state\n");
        return -1;
    }

    int result = dmtcp_input_register();
    if (result != 0)
    {
        DMOD_LOG_ERROR("dmtcp: cannot register as the TCP protocol handler (%d)\n", result);
        return -1;
    }

    DMOD_LOG_INFO("DMTCP initialized\n");
    return 0;
}

int dmod_deinit(void)
{
    dmtcp_input_unregister();

    dmtcp_conn_table_deinit();
    dmtcp_listen_table_deinit();

    DMOD_LOG_INFO("DMTCP deinitialized\n");
    return 0;
}
