/**
 * @file dmtcp_close.c
 * @brief dmtcp_close()/_abort() and the TIME_WAIT timer
 */
#include "dmod.h"
#include "dmtcp_internal.h"
#include <errno.h>

dmod_dmtcp_api_declaration(1.0, int, _close, ( dmtcp_conn_t conn ))
{
    if (conn == NULL || conn->magic != DMTCP_CONN_MAGIC)
        return -EINVAL;

    dmosi_mutex_lock(conn->lock);

    if (conn->fin_sent)
    {
        dmosi_mutex_unlock(conn->lock); /* idempotent - already closing/closed */
        return 0;
    }

    if (conn->state == dmtcp_state_syn_sent)
    {
        /* RFC793 "CLOSE in SYN-SENT" - nothing was ever sent on the wire to abort */
        conn->fin_sent = true;
        conn->fin_acked = true;
        dmtcp_disarm_rto(conn);
        dmosi_mutex_unlock(conn->lock);
        dmtcp_conn_table_terminate(conn, dmtcp_terminal_closed, 0, dmtcp_teardown_context_normal);
        return 0;
    }

    conn->state = (conn->state == dmtcp_state_close_wait) ? dmtcp_state_last_ack : dmtcp_state_fin_wait_1;
    conn->fin_sent = true;

    int result = dmtcp_send_segment(&conn->local_addr, conn->local_port, &conn->peer_addr, conn->peer_port,
                                     conn->snd_nxt, conn->rcv_nxt, (uint8_t)(DMTCP_FLAG_FIN | DMTCP_FLAG_ACK), conn->rcv_wnd,
                                     NULL, 0, DMTCP_DEFAULT_ARP_TIMEOUT_MS);
    conn->snd_nxt += 1u; /* the FIN itself consumes one sequence number */
    dmtcp_arm_rto(conn);

    dmosi_mutex_unlock(conn->lock);
    return result;
}

dmod_dmtcp_api_declaration(1.0, int, _abort, ( dmtcp_conn_t conn ))
{
    if (conn == NULL || conn->magic != DMTCP_CONN_MAGIC)
        return -EINVAL;

    dmosi_mutex_lock(conn->lock);
    dmtcp_send_segment(&conn->local_addr, conn->local_port, &conn->peer_addr, conn->peer_port,
                        conn->snd_nxt, conn->rcv_nxt, DMTCP_FLAG_RST, 0, NULL, 0, DMTCP_DEFAULT_ARP_TIMEOUT_MS);
    dmosi_mutex_unlock(conn->lock);

    dmtcp_conn_table_terminate(conn, dmtcp_terminal_reset, 0, dmtcp_teardown_context_normal);
    return 0;
}

/**
 * @brief dmosi_timer_callback_t for conn->time_wait_timer
 *
 * Runs in timer/interrupt context, on this timer's own worker thread - see
 * dmtcp_teardown_context_time_wait_timer's doc comment for why that timer
 * must not be destroyed from within this call.
 */
static void dmtcp_time_wait_timer_callback(void* arg)
{
    struct dmtcp_conn* conn = (struct dmtcp_conn*)arg;
    if (conn == NULL || conn->magic != DMTCP_CONN_MAGIC)
        return;

    dmosi_mutex_lock(conn->lock);
    bool still_time_wait = (conn->state == dmtcp_state_time_wait);
    dmosi_mutex_unlock(conn->lock);

    if (!still_time_wait)
        return; /* stale fire */

    dmtcp_conn_table_terminate(conn, dmtcp_terminal_closed, 0, dmtcp_teardown_context_time_wait_timer);
}

dmosi_timer_t dmtcp_close_create_time_wait_timer(struct dmtcp_conn* conn)
{
    return dmosi_timer_create(dmtcp_time_wait_timer_callback, conn, DMTCP_TIME_WAIT_MS, false);
}
