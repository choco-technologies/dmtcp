/**
 * @file dmtcp_input.c
 * @brief dmtcp_handle_ip_packet() - demux, new-connection handling, and the
 *        per-segment state-machine step for everything past the handshake
 *
 * Registered with dmip_register_protocol(DMIP_PROTO_TCP, ...) in
 * dmod_init() (see dmtcp.c). Runs inline, synchronously, on whatever
 * thread is pumping the interface a segment arrived on - see
 * dmip_protocol_handler_t in dmip.h - so everything here (including the
 * immediate ACKs/SYN-ACK/RST this file sends) happens in that same call,
 * with no queue or worker thread of its own.
 */
#include "dmod.h"
#include "dmtcp_internal.h"
#include <string.h>
#include <errno.h>

/**
 * @brief What a segment-processing step decided needs to happen after
 *        conn->lock is released - callbacks are never invoked while it's
 *        held, see dmtcp_internal.h's struct dmtcp_conn doc comment
 */
struct segment_result
{
    bool     completed_passive_handshake; /* fire conn->pending_accept_handler */
    bool     has_data;
    const uint8_t* data;
    size_t   data_len;
    bool     has_eof;      /* fire on_data(conn, NULL, 0, ...) */
    bool     completed_close; /* LAST_ACK's FIN got acked - fire dmtcp_terminal_closed */
};

static void send_immediate_ack(struct dmtcp_conn* conn)
{
    dmtcp_send_segment(&conn->local_addr, conn->local_port, &conn->peer_addr, conn->peer_port,
                        conn->snd_nxt, conn->rcv_nxt, DMTCP_FLAG_ACK, conn->rcv_wnd, NULL, 0, DMTCP_DEFAULT_ARP_TIMEOUT_MS);
}

static void discard_acked_prefix(struct dmtcp_conn* conn, uint32_t amount)
{
    if (amount == 0)
        return;

    size_t remaining = conn->send_buffer_len - amount;
    if (remaining > 0)
    {
        memmove(conn->send_buffer, conn->send_buffer + amount, remaining);
    }
    conn->send_buffer_len -= amount;
    conn->send_buffer_sent -= amount;
}

/**
 * @brief Apply the transition triggered by our own outstanding FIN just
 *        getting acked (assumes conn->fin_acked was just set true)
 */
static void apply_fin_acked_transition(struct dmtcp_conn* conn, struct segment_result* result)
{
    if (conn->state == dmtcp_state_fin_wait_1)
    {
        conn->state = dmtcp_state_fin_wait_2;
    }
    else if (conn->state == dmtcp_state_closing)
    {
        conn->state = dmtcp_state_time_wait;
        dmosi_timer_start(conn->time_wait_timer);
    }
    else if (conn->state == dmtcp_state_last_ack)
    {
        result->completed_close = true;
    }
}

/**
 * @brief RFC 793 §3.2 SND.UNA advancement - shared by the SYN_RECEIVED
 *        handshake-completing ACK, plain data ACKs, and our own FIN being
 *        acked, since they're all "the peer just acknowledged more of our
 *        send sequence space" (see docs/dmtcp.md)
 */
static void apply_ack(struct dmtcp_conn* conn, const dmtcp_header_t* hdr, struct segment_result* result)
{
    if ((hdr->flags & DMTCP_FLAG_ACK) == 0)
        return;

    conn->snd_wnd = hdr->window;

    int32_t delta = (int32_t)(hdr->ack_num - conn->snd_una);
    if (delta <= 0 || (uint32_t)delta > (conn->snd_nxt - conn->snd_una))
        return; /* old/duplicate ack, or acks something never sent - ignore */

    bool was_syn_received = (conn->state == dmtcp_state_syn_received);
    uint32_t fin_consumed = (conn->fin_sent && !conn->fin_acked) ? 1u : 0u;
    uint32_t data_outstanding = (uint32_t)conn->send_buffer_sent;
    uint32_t udelta = (uint32_t)delta;

    if (udelta >= data_outstanding + fin_consumed)
    {
        discard_acked_prefix(conn, data_outstanding);
        if (fin_consumed != 0)
        {
            conn->fin_acked = true;
        }
    }
    else
    {
        discard_acked_prefix(conn, udelta);
    }

    conn->snd_una = hdr->ack_num;
    conn->retransmit_count = 0;
    (conn->snd_una == conn->snd_nxt) ? dmtcp_disarm_rto(conn) : dmtcp_arm_rto(conn);

    if (was_syn_received && conn->state == dmtcp_state_syn_received && conn->snd_una == conn->snd_nxt)
    {
        conn->state = dmtcp_state_established;
        result->completed_passive_handshake = true;
    }

    dmtcp_flush_send_buffer(conn);

    if (conn->fin_acked && conn->state != dmtcp_state_fin_wait_2 && conn->state != dmtcp_state_time_wait)
    {
        apply_fin_acked_transition(conn, result);
    }
}

static void apply_peer_fin_transition(struct dmtcp_conn* conn)
{
    if (conn->state == dmtcp_state_established)
    {
        conn->state = dmtcp_state_close_wait;
    }
    else if (conn->state == dmtcp_state_fin_wait_1)
    {
        conn->state = dmtcp_state_closing; /* our FIN can't be acked yet - see apply_ack, which
                                             * would already have moved fin_wait_1 -> fin_wait_2
                                             * this same call if it had been */
    }
    else if (conn->state == dmtcp_state_fin_wait_2)
    {
        conn->state = dmtcp_state_time_wait;
        dmosi_timer_start(conn->time_wait_timer);
    }
}

/**
 * @brief In-order data delivery and in-order FIN handling (RFC 793 §3.2) -
 *        no reassembly buffer: anything out of order is simply not
 *        applied (see docs/dmtcp.md), relying on the always-sent ACK
 *        below to prompt the peer's own retransmission timer
 */
static void apply_incoming_data_and_fin(struct dmtcp_conn* conn, const dmtcp_header_t* hdr, const uint8_t* payload, size_t payload_len, struct segment_result* result)
{
    bool advanced = false;

    if (payload_len > 0 && hdr->seq_num == conn->rcv_nxt)
    {
        conn->rcv_nxt += (uint32_t)payload_len;
        result->has_data = true;
        result->data = payload;
        result->data_len = payload_len;
        advanced = true;
    }

    bool fin_in_order = (hdr->flags & DMTCP_FLAG_FIN) != 0 && hdr->seq_num + (uint32_t)payload_len == conn->rcv_nxt && !conn->peer_fin_received;
    if (fin_in_order)
    {
        conn->rcv_nxt += 1u;
        conn->peer_fin_received = true;
        result->has_eof = true;
        advanced = true;
        apply_peer_fin_transition(conn);
    }

    if (advanced || payload_len > 0 || (hdr->flags & DMTCP_FLAG_FIN) != 0)
    {
        send_immediate_ack(conn);
    }
}

/**
 * @brief RFC 793 active-open handshake completion: our SYN_SENT
 *        connection just received the peer's SYN-ACK
 *
 * Assumes conn->lock is held on entry; unlocks before returning (and
 * before firing on_established), same discipline as every other path here.
 * Simultaneous open (a bare SYN with no ACK arriving in SYN_SENT) is not
 * handled - a documented gap, see docs/dmtcp.md.
 */
static void handle_syn_sent_segment(struct dmtcp_conn* conn, const dmtcp_header_t* hdr)
{
    bool ack_ok = (hdr->flags & DMTCP_FLAG_ACK) != 0 && hdr->ack_num == conn->snd_nxt;
    bool has_syn = (hdr->flags & DMTCP_FLAG_SYN) != 0;

    if (!has_syn || !ack_ok)
    {
        dmosi_mutex_unlock(conn->lock);
        return;
    }

    conn->irs = hdr->seq_num;
    conn->rcv_nxt = hdr->seq_num + 1u;
    conn->snd_una = hdr->ack_num;
    conn->snd_wnd = hdr->window;
    conn->state = dmtcp_state_established;
    dmtcp_disarm_rto(conn);
    send_immediate_ack(conn);

    dmtcp_conn_callbacks_t callbacks = conn->callbacks;
    void* user_data = conn->user_data;
    dmosi_mutex_unlock(conn->lock);

    if (callbacks.on_established != NULL)
    {
        callbacks.on_established(conn, user_data);
    }
}

/**
 * @brief Drive one received segment through an already-tracked
 *        connection's state machine
 */
static void process_segment_for_conn(struct dmtcp_conn* conn, const dmtcp_header_t* hdr, const uint8_t* payload, size_t payload_len)
{
    dmosi_mutex_lock(conn->lock);
    if (conn->magic != DMTCP_CONN_MAGIC)
    {
        dmosi_mutex_unlock(conn->lock);
        return;
    }

    if ((hdr->flags & DMTCP_FLAG_RST) != 0)
    {
        /* Only honored on an exact sequence match (stricter than bare RFC793,
         * closer to RFC 5961's hardening) - see docs/dmtcp.md. */
        bool acceptable = hdr->seq_num == conn->rcv_nxt;
        dmosi_mutex_unlock(conn->lock);
        if (acceptable)
        {
            dmtcp_conn_table_terminate(conn, dmtcp_terminal_reset, 0, dmtcp_teardown_context_normal);
        }
        return;
    }

    if (conn->state == dmtcp_state_syn_sent)
    {
        handle_syn_sent_segment(conn, hdr); /* unlocks internally */
        return;
    }

    struct segment_result result = { 0 };
    apply_ack(conn, hdr, &result);
    apply_incoming_data_and_fin(conn, hdr, payload, payload_len, &result);

    dmtcp_conn_callbacks_t callbacks = conn->callbacks;
    void* user_data = conn->user_data;
    dmtcp_accept_handler_t accept_handler = result.completed_passive_handshake ? conn->pending_accept_handler : NULL;
    dmip_addr_t peer_addr = conn->peer_addr;
    uint16_t peer_port = conn->peer_port;
    dmnetif_iface_t iface = conn->iface;
    dmosi_mutex_unlock(conn->lock);

    if (accept_handler != NULL)
    {
        accept_handler(conn, &peer_addr, peer_port, iface);
    }
    if (result.has_data && callbacks.on_data != NULL)
    {
        callbacks.on_data(conn, result.data, result.data_len, user_data);
    }
    if (result.has_eof && callbacks.on_data != NULL)
    {
        callbacks.on_data(conn, NULL, 0, user_data);
    }
    if (result.completed_close)
    {
        dmtcp_conn_table_terminate(conn, dmtcp_terminal_closed, 0, dmtcp_teardown_context_normal);
    }
}

/**
 * @brief A SYN arrived on a listened port with no existing connection -
 *        create the half-open TCB and answer with our own SYN-ACK
 */
static void handle_new_syn(dmtcp_accept_handler_t listener, dmip_family_t family, dmnetif_iface_t iface,
                            const dmip_addr_t* local_addr, uint16_t local_port,
                            const dmip_addr_t* peer_addr, uint16_t peer_port, const dmtcp_header_t* hdr)
{
    if (family != dmip_family_v4)
    {
        DMOD_LOG_WARN("dmtcp: received IPv6 SYN, cannot complete a handshake - no dmip_v6_send() yet\n");
        return; /* no half-open TCB is ever created for a v6 SYN - see docs/dmtcp.md */
    }

    struct dmtcp_conn* conn = dmtcp_conn_table_create();
    if (conn == NULL)
        return;

    conn->local_addr = *local_addr;
    conn->local_port = local_port;
    conn->peer_addr = *peer_addr;
    conn->peer_port = peer_port;
    conn->iface = iface;
    conn->irs = hdr->seq_num;
    conn->rcv_nxt = hdr->seq_num + 1u;
    conn->iss = dmosi_get_tick_count();
    conn->snd_una = conn->iss;
    conn->snd_nxt = conn->iss + 1u;
    conn->state = dmtcp_state_syn_received;
    conn->pending_accept_handler = listener;

    if (dmtcp_conn_table_insert(conn) != 0)
    {
        dmtcp_conn_table_destroy(conn);
        return;
    }

    dmosi_mutex_lock(conn->lock);
    dmtcp_send_segment(&conn->local_addr, conn->local_port, &conn->peer_addr, conn->peer_port,
                        conn->iss, conn->rcv_nxt, (uint8_t)(DMTCP_FLAG_SYN | DMTCP_FLAG_ACK), conn->rcv_wnd,
                        NULL, 0, DMTCP_DEFAULT_ARP_TIMEOUT_MS);
    dmtcp_arm_rto(conn);
    dmosi_mutex_unlock(conn->lock);
}

/**
 * @brief RFC 793 §3.4: reply to a segment matching no tracked connection
 *
 * IPv6 has no dmip_v6_send() yet, so this is silently a no-op for that
 * family - the same gap dmudp/dmicmp already document for themselves.
 */
static void send_rst_for_unmatched(dmip_family_t family, const dmip_addr_t* local_addr, uint16_t local_port,
                                    const dmip_addr_t* peer_addr, uint16_t peer_port, const dmtcp_header_t* hdr, size_t payload_len)
{
    if (family != dmip_family_v4)
        return;

    if ((hdr->flags & DMTCP_FLAG_ACK) != 0)
    {
        dmtcp_send_segment(local_addr, local_port, peer_addr, peer_port, hdr->ack_num, 0, DMTCP_FLAG_RST, 0, NULL, 0, DMTCP_DEFAULT_ARP_TIMEOUT_MS);
    }
    else if ((hdr->flags & DMTCP_FLAG_SYN) != 0)
    {
        uint32_t seg_len = (uint32_t)payload_len + 1u; /* the SYN itself consumes one sequence number */
        dmtcp_send_segment(local_addr, local_port, peer_addr, peer_port, 0, hdr->seq_num + seg_len,
                            (uint8_t)(DMTCP_FLAG_RST | DMTCP_FLAG_ACK), 0, NULL, 0, DMTCP_DEFAULT_ARP_TIMEOUT_MS);
    }
    /* else: a bare data/FIN segment, no SYN/ACK, matching nothing - dropped, not RST'd
     * (documented gap in RFC793 §3.4's full off-ACK reply table, see docs/dmtcp.md) */
}

/**
 * @brief Exact 4-tuple match wins over any listener; a SYN on a listened
 *        port with no existing connection starts a new handshake;
 *        anything else matching nothing gets a RST (or is dropped, per
 *        RFC 793 §3.4 - see send_rst_for_unmatched())
 */
static void dispatch(dmip_family_t family, dmnetif_iface_t iface, const dmip_addr_t* local_addr, const dmip_addr_t* peer_addr,
                      const dmtcp_header_t* hdr, const uint8_t* payload, size_t payload_len)
{
    struct dmtcp_conn* conn = dmtcp_conn_table_find(local_addr, hdr->dst_port, peer_addr, hdr->src_port);
    if (conn != NULL)
    {
        process_segment_for_conn(conn, hdr, payload, payload_len);
        return;
    }

    if ((hdr->flags & DMTCP_FLAG_RST) != 0)
        return; /* RFC793 3.4 rule 1: never reply to a RST */

    bool is_pure_syn = (hdr->flags & DMTCP_FLAG_SYN) != 0 && (hdr->flags & DMTCP_FLAG_ACK) == 0;
    dmtcp_accept_handler_t listener = is_pure_syn ? dmtcp_listen_table_find(hdr->dst_port) : NULL;
    if (listener != NULL)
    {
        handle_new_syn(listener, family, iface, local_addr, hdr->dst_port, peer_addr, hdr->src_port, hdr);
        return;
    }

    send_rst_for_unmatched(family, local_addr, hdr->dst_port, peer_addr, hdr->src_port, hdr, payload_len);
}

/**
 * @brief dmip_protocol_handler_t registered for DMIP_PROTO_TCP - see dmod_init()
 *
 * Parses the enclosing IP header, then the TCP header, validates the
 * segment's checksum (mandatory for both families, unlike dmudp's IPv4
 * "0 means none" exception - see dmtcp.h), then dispatches. `packet` is
 * borrowed (see dmip_protocol_handler_t's own doc comment) - nothing here
 * keeps a pointer into it past this call other than what's passed inline
 * to a callback, which always returns before this function does.
 */
void dmtcp_handle_ip_packet(dmip_family_t family, dmnetif_iface_t iface, const uint8_t* packet, size_t packet_len)
{
    if (family == dmip_family_v4)
    {
        dmip_v4_header_t ip_header = { 0 };
        size_t header_len = 0;
        if (dmip_v4_parse_header(packet, packet_len, &ip_header, &header_len) != 0)
            return;

        const uint8_t* segment = packet + header_len;
        size_t segment_len = packet_len - header_len;
        if (segment_len < DMTCP_HEADER_LEN)
            return;

        dmtcp_header_t hdr = { 0 };
        size_t tcp_header_len = 0;
        if (dmtcp_parse_header(segment, segment_len, &hdr, &tcp_header_len) != 0)
            return;
        if (!dmtcp_v4_checksum_valid(&ip_header.src, &ip_header.dst, segment, segment_len))
            return;

        dispatch(dmip_family_v4, iface, &ip_header.dst, &ip_header.src, &hdr, segment + tcp_header_len, segment_len - tcp_header_len);
    }
    else
    {
        dmip_v6_header_t ip_header = { 0 };
        if (dmip_v6_parse_header(packet, packet_len, &ip_header) != 0)
            return;

        const uint8_t* segment = packet + DMIP_V6_HEADER_LEN;
        size_t segment_len = packet_len - DMIP_V6_HEADER_LEN;
        if (segment_len < DMTCP_HEADER_LEN)
            return;

        dmtcp_header_t hdr = { 0 };
        size_t tcp_header_len = 0;
        if (dmtcp_parse_header(segment, segment_len, &hdr, &tcp_header_len) != 0)
            return;
        if (!dmtcp_v6_checksum_valid(&ip_header.src, &ip_header.dst, segment, segment_len))
            return;

        dispatch(dmip_family_v6, iface, &ip_header.dst, &ip_header.src, &hdr, segment + tcp_header_len, segment_len - tcp_header_len);
    }
}

/**
 * @brief Register/unregister dmtcp_handle_ip_packet() with dmip
 *
 * Deliberately kept in the same translation unit as
 * dmtcp_handle_ip_packet() itself, rather than calling
 * dmip_register_protocol() directly from dmtcp.c's dmod_init(): this
 * loader does not correctly resolve a callback whose address is taken in
 * one .c file and handed to another module's registration API from a
 * different .c file within the same module - the call silently jumps to
 * an unrelocated address (an unrelated small offset) the first time dmip
 * invokes it. Every cross-module callback registration in this module
 * (this one, and the two dmosi_timer_create() calls in dmtcp_output.c/
 * dmtcp_close.c) follows the same rule: register from the same file that
 * defines the callback.
 */
int dmtcp_input_register(void)
{
    return dmip_register_protocol(DMIP_PROTO_TCP, dmtcp_handle_ip_packet);
}

void dmtcp_input_unregister(void)
{
    dmip_unregister_protocol(DMIP_PROTO_TCP);
}
