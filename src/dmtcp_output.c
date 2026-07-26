/**
 * @file dmtcp_output.c
 * @brief Segment transmission, the outbound byte-stream buffer, and the
 *        retransmission timer
 */
#include "dmod.h"
#include "dmtcp_internal.h"
#include <string.h>
#include <errno.h>

static void write_u16_be(uint8_t* p, uint16_t value)
{
    p[0] = (uint8_t)(value >> 8);
    p[1] = (uint8_t)(value & 0xFFu);
}

/**
 * @brief Build one [pseudo-header][TCP header][payload] buffer, checksum
 *        it, and hand dmip_send() a pointer past the (never-transmitted)
 *        pseudo-header prefix - the same "one buffer, no extra copy" shape
 *        dmudp_send() uses
 */
int dmtcp_send_segment(const dmip_addr_t* src, uint16_t src_port, const dmip_addr_t* dst, uint16_t dst_port,
                        uint32_t seq, uint32_t ack, uint8_t flags, uint16_t window,
                        const uint8_t* payload, size_t payload_len, uint32_t arp_timeout_ms)
{
    if (src == NULL || dst == NULL || dst_port == 0 || (payload == NULL && payload_len > 0))
        return -EINVAL;
    if (dst->family == dmip_family_v6)
        return -ENOSYS; /* no dmip_v6_send() yet - see dmtcp.h/dmip.h */
    if (dst->family != dmip_family_v4)
        return -EINVAL;

    size_t seg_len = DMTCP_HEADER_LEN + payload_len;
    size_t total = DMTCP_V4_PSEUDO_HEADER_LEN + seg_len;
    uint8_t* buf = Dmod_Malloc(total);
    if (buf == NULL)
        return -ENOMEM;

    dmtcp_wire_write_v4_pseudo_header(buf, src, dst, (uint16_t)seg_len);

    uint8_t* segment = buf + DMTCP_V4_PSEUDO_HEADER_LEN;
    dmtcp_header_t header = { .src_port = src_port, .dst_port = dst_port, .seq_num = seq, .ack_num = ack, .flags = flags, .window = window };
    dmtcp_build_header(segment, seg_len, &header);
    if (payload_len > 0)
    {
        memcpy(segment + DMTCP_HEADER_LEN, payload, payload_len);
    }

    write_u16_be(&segment[16], dmip_checksum(buf, total));

    dmip_header_t ip_header = { 0 };
    ip_header.family = dmip_family_v4;
    ip_header.header.v4.ttl = DMIP_DEFAULT_TTL;
    ip_header.header.v4.protocol = DMIP_PROTO_TCP;
    ip_header.header.v4.identification = dmip_v4_next_identification();
    ip_header.header.v4.src = *src;
    ip_header.header.v4.dst = *dst;

    int result = dmip_send(&ip_header, segment, seg_len, arp_timeout_ms);
    Dmod_Free(buf);
    return result;
}

void dmtcp_arm_rto(struct dmtcp_conn* conn)
{
    conn->retransmit_count = 0;
    dmosi_timer_start(conn->rto_timer); /* dmosi_timer_start()/_reset() are equivalent: start if
                                          * dormant, else restart the countdown from now */
}

void dmtcp_disarm_rto(struct dmtcp_conn* conn)
{
    dmosi_timer_stop(conn->rto_timer);
    conn->retransmit_count = 0;
}

void dmtcp_flush_send_buffer(struct dmtcp_conn* conn)
{
    while (conn->send_buffer_sent < conn->send_buffer_len)
    {
        uint32_t outstanding = (uint32_t)conn->send_buffer_sent;
        if (outstanding >= conn->snd_wnd)
            break; /* peer's window is full - no zero-window probe/persist timer, see docs/dmtcp.md */

        size_t queued = conn->send_buffer_len - conn->send_buffer_sent;
        size_t room = (size_t)conn->snd_wnd - outstanding;
        size_t chunk = queued < room ? queued : room;
        if (chunk > DMTCP_MAX_SEGMENT_LEN)
        {
            chunk = DMTCP_MAX_SEGMENT_LEN;
        }

        /* Fire-and-forget, like every other automatic segment this module sends
         * (handle_new_syn()'s SYN-ACK, the immediate ACKs in process_segment_for_conn()):
         * once handed to dmip, these bytes occupy send sequence space whether or not
         * the transmission actually reaches the peer - that's exactly what the
         * retransmission timer (armed below) exists to recover from. Bailing out of
         * the loop on a transient send failure without advancing snd_nxt/arming the
         * timer would leave the connection with data queued but no retry in flight. */
        dmtcp_send_segment(&conn->local_addr, conn->local_port, &conn->peer_addr, conn->peer_port,
                            conn->snd_nxt, conn->rcv_nxt, DMTCP_FLAG_ACK, conn->rcv_wnd,
                            conn->send_buffer + conn->send_buffer_sent, chunk, DMTCP_DEFAULT_ARP_TIMEOUT_MS);

        conn->snd_nxt += (uint32_t)chunk;
        conn->send_buffer_sent += chunk;
        dmtcp_arm_rto(conn);
    }
}

dmod_dmtcp_api_declaration(1.0, int, _send, ( dmtcp_conn_t conn, const void* data, size_t data_len ))
{
    if (conn == NULL || conn->magic != DMTCP_CONN_MAGIC || (data == NULL && data_len > 0))
        return -EINVAL;

    dmosi_mutex_lock(conn->lock);

    int result;
    if (conn->fin_sent)
    {
        result = -ENETDOWN;
    }
    else if (conn->state != dmtcp_state_established && conn->state != dmtcp_state_close_wait)
    {
        result = -ENOTCONN;
    }
    else
    {
        size_t space = DMTCP_SEND_BUFFER_LEN - conn->send_buffer_len;
        size_t to_copy = data_len < space ? data_len : space;
        if (to_copy > 0)
        {
            memcpy(conn->send_buffer + conn->send_buffer_len, data, to_copy);
            conn->send_buffer_len += to_copy;
        }
        dmtcp_flush_send_buffer(conn);
        result = (int)to_copy;
    }

    dmosi_mutex_unlock(conn->lock);
    return result;
}

/* ============================================================================
 *                      Retransmission timer
 * ========================================================================== */

static void resend_syn(struct dmtcp_conn* conn)
{
    uint8_t flags = DMTCP_FLAG_SYN;
    uint32_t ack = 0;
    if (conn->state == dmtcp_state_syn_received)
    {
        flags |= DMTCP_FLAG_ACK;
        ack = conn->rcv_nxt;
    }
    dmtcp_send_segment(&conn->local_addr, conn->local_port, &conn->peer_addr, conn->peer_port,
                        conn->iss, ack, flags, conn->rcv_wnd, NULL, 0, DMTCP_DEFAULT_ARP_TIMEOUT_MS);
}

static void resend_data(struct dmtcp_conn* conn)
{
    size_t offset = 0;
    while (offset < conn->send_buffer_sent)
    {
        size_t chunk = conn->send_buffer_sent - offset;
        if (chunk > DMTCP_MAX_SEGMENT_LEN)
        {
            chunk = DMTCP_MAX_SEGMENT_LEN;
        }

        dmtcp_send_segment(&conn->local_addr, conn->local_port, &conn->peer_addr, conn->peer_port,
                            conn->snd_una + (uint32_t)offset, conn->rcv_nxt, DMTCP_FLAG_ACK, conn->rcv_wnd,
                            conn->send_buffer + offset, chunk, DMTCP_DEFAULT_ARP_TIMEOUT_MS);
        offset += chunk;
    }
}

static void resend_fin(struct dmtcp_conn* conn)
{
    dmtcp_send_segment(&conn->local_addr, conn->local_port, &conn->peer_addr, conn->peer_port,
                        conn->snd_nxt - 1u, conn->rcv_nxt, (uint8_t)(DMTCP_FLAG_FIN | DMTCP_FLAG_ACK), conn->rcv_wnd,
                        NULL, 0, DMTCP_DEFAULT_ARP_TIMEOUT_MS);
}

/**
 * @brief Resend whatever is currently the single outstanding thing for
 *        `conn`'s state - the SYN/SYN-ACK during the handshake, otherwise
 *        any unacked data (go-back-N: the entire unacked prefix is resent,
 *        there is no SACK to resend a narrower range) followed by the FIN
 *        if one is outstanding
 */
static void retransmit_now(struct dmtcp_conn* conn)
{
    if (conn->state == dmtcp_state_syn_sent || conn->state == dmtcp_state_syn_received)
    {
        resend_syn(conn);
        return;
    }

    if (conn->send_buffer_sent > 0)
    {
        resend_data(conn);
    }
    if (conn->fin_sent && !conn->fin_acked)
    {
        resend_fin(conn);
    }
}

/**
 * @brief dmosi_timer_callback_t for conn->rto_timer
 *
 * Runs in timer/interrupt context (per dmosi_timer_callback_t's own doc
 * comment) - a different context than the dmip rx-thread callback that
 * drives everything else in this module, and the one place conn->lock is
 * taken from two different contexts (see docs/dmtcp.md).
 *
 * When the retry limit is exceeded, dmtcp_teardown_context_rto_timer is
 * passed to dmtcp_conn_table_terminate() so it stops (rather than
 * destroys) conn->rto_timer: destroying it here would join this very
 * timer's own worker thread - the thread this callback is currently
 * executing on - a self-join. See dmtcp_teardown_context_t's doc comment
 * for the full reasoning and docs/dmtcp.md for the resulting bounded
 * resource tradeoff.
 */
static void dmtcp_rto_timer_callback(void* arg)
{
    struct dmtcp_conn* conn = (struct dmtcp_conn*)arg;
    if (conn == NULL || conn->magic != DMTCP_CONN_MAGIC)
        return;

    dmosi_mutex_lock(conn->lock);

    bool outstanding = (conn->state == dmtcp_state_syn_sent || conn->state == dmtcp_state_syn_received)
                     || conn->send_buffer_sent > 0
                     || (conn->fin_sent && !conn->fin_acked);
    if (!outstanding)
    {
        dmosi_mutex_unlock(conn->lock);
        return; /* stale fire - nothing to do */
    }

    conn->retransmit_count++;
    if (conn->retransmit_count > DMTCP_MAX_RETRANSMITS)
    {
        dmosi_mutex_unlock(conn->lock);
        dmtcp_conn_table_terminate(conn, dmtcp_terminal_error, -ETIMEDOUT, dmtcp_teardown_context_rto_timer);
        return;
    }

    retransmit_now(conn);
    dmosi_timer_start(conn->rto_timer);
    dmosi_mutex_unlock(conn->lock);
}

dmosi_timer_t dmtcp_output_create_rto_timer(struct dmtcp_conn* conn)
{
    return dmosi_timer_create(dmtcp_rto_timer_callback, conn, DMTCP_RTO_MS, false);
}
