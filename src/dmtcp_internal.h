#ifndef DMTCP_INTERNAL_H
#define DMTCP_INTERNAL_H

#include "dmtcp.h"
#include "dmosi.h"

/**
 * @file dmtcp_internal.h
 * @brief Private state and cross-file plumbing for dmtcp
 *
 * The public opaque dmtcp_conn_t (struct dmtcp_conn*) is defined here, not
 * in include/dmtcp.h - callers only ever get a pointer, never see the
 * layout (see dmod-coding-conventions: opaque-handle + magic-guard
 * pattern, worked example in dm_sw_ring).
 *
 * TCP's state machine is materially bigger than a stateless dispatch
 * table, so it's split into cohesive files rather than one dmtcp.c:
 *
 *  - dmtcp_wire.c        header build/parse, checksum (public API + the
 *                        internal pseudo-header checksum helper shared by
 *                        the receive and send paths)
 *  - dmtcp_listen_table.c dmtcp_listen()/_listen_any()/_unlisten() and the
 *                        internal lookup dmtcp_handle_ip_packet() uses
 *  - dmtcp_conn_table.c  TCB allocation/lookup/insert/remove, the
 *                        ephemeral local-port allocator for dmtcp_connect(),
 *                        the shared "tear down and notify" helper, and the
 *                        simple per-connection accessors
 *  - dmtcp_output.c      dmtcp_send(), the retransmission timer, and the
 *                        low-level segment sender every other file calls
 *                        into (RST generation included)
 *  - dmtcp_input.c       dmtcp_handle_ip_packet() itself: demux, new-SYN
 *                        handling, and the per-state segment-processing
 *                        step that drives everything past the handshake
 *  - dmtcp_close.c       dmtcp_close()/_abort() and the TIME_WAIT timer
 *  - dmtcp.c             dmod_init()/_deinit() and dmtcp_connect()
 */

/**
 * @brief Magic value guarding struct dmtcp_conn - "TCB " in ASCII
 *
 * Checked at the top of every function that receives a dmtcp_conn_t,
 * dmtcp_conn_table_destroy() included, the same convention dm_sw_ring uses
 * for its own context struct.
 */
#define DMTCP_CONN_MAGIC 0x54434220u

/**
 * @brief Fixed retransmission timeout - no RTT estimation/backoff, per the
 *        "simple/fixed window" scope decision (see docs/dmtcp.md)
 */
#define DMTCP_RTO_MS 1000u

/**
 * @brief How many consecutive retransmissions of the same outstanding
 *        SYN/data/FIN are attempted before giving up on a connection
 *        (on_error(-ETIMEDOUT) fires and the TCB is freed)
 */
#define DMTCP_MAX_RETRANSMITS 5u

/**
 * @brief How long a connection stays in TIME_WAIT before its TCB is freed
 *
 * A deliberate, documented deviation from RFC 793's 2*MSL (~4 minutes) -
 * see docs/dmtcp.md's gaps section for why that isn't a good tradeoff on a
 * resource-constrained embedded target.
 */
#define DMTCP_TIME_WAIT_MS 4000u

/**
 * @brief Capacity of a connection's outbound byte-stream buffer
 *
 * A plain Dmod_Malloc'd linear buffer, not dm_sw_ring or dmlist - see
 * docs/dmtcp.md for why (conn->lock already serializes every access, and
 * dm_sw_ring's peek/discard API doesn't cleanly support "peek the
 * already-sent-but-unacked prefix, discard exactly the acked amount"
 * without an extra copy anyway).
 */
#define DMTCP_SEND_BUFFER_LEN 4096u

/**
 * @brief Fixed receive window dmtcp advertises to every peer
 *
 * No real receive-side backpressure exists (on_data must not block) - see
 * docs/dmtcp.md.
 */
#define DMTCP_DEFAULT_RECV_WINDOW 4096u

/**
 * @brief Largest payload dmtcp puts in a single outbound segment
 *
 * A fixed, conservative value - not derived from the peer's real
 * advertised MSS, since TCP options (including MSS) are never parsed (see
 * docs/dmtcp.md).
 */
#define DMTCP_MAX_SEGMENT_LEN 1400u

/**
 * @brief Default ARP-resolution timeout for a segment dmtcp sends on its
 *        own initiative (a RTO retransmission, a RST, ...)
 */
#define DMTCP_DEFAULT_ARP_TIMEOUT_MS 1000u

#define DMTCP_V4_PSEUDO_HEADER_LEN 12u
#define DMTCP_V6_PSEUDO_HEADER_LEN 40u

/**
 * @brief One TCP connection - the real definition behind dmtcp_conn_t
 *
 * `lock` guards every field below it (not `magic`/`lock` themselves), held
 * during all internal state mutation and released before calling any
 * dmtcp_conn_callbacks_t member - see docs/dmtcp.md's "Two lock contexts"
 * section for why this is the one lock taken from both the dmip rx-thread
 * callback and the retransmission timer's own (different) context.
 */
struct dmtcp_conn
{
    uint32_t      magic;
    dmtcp_state_t state;
    dmosi_mutex_t lock;

    /* Endpoint - local_addr/local_port are pinned once at TCB creation and
     * never re-resolved per segment (unlike dmudp_send()) - see
     * docs/dmtcp.md's "Source address stability" section. */
    dmip_addr_t     local_addr;
    uint16_t        local_port;
    dmip_addr_t     peer_addr;
    uint16_t        peer_port;
    dmnetif_iface_t iface;

    /* RFC 793 §3.2 send sequence variables */
    uint32_t snd_una;
    uint32_t snd_nxt;
    uint16_t snd_wnd;
    uint32_t iss;

    /* RFC 793 §3.2 receive sequence variables */
    uint32_t rcv_nxt;
    uint16_t rcv_wnd;
    uint32_t irs;

    /* Outbound byte-stream buffer: [0, send_buffer_sent) is already
     * transmitted at least once (the retransmit candidates,
     * send_buffer_sent == snd_nxt - snd_una), [send_buffer_sent,
     * send_buffer_len) is queued but not yet sent. */
    uint8_t* send_buffer;
    size_t   send_buffer_len;
    size_t   send_buffer_sent;

    dmosi_timer_t rto_timer;
    uint32_t      retransmit_count;

    dmosi_timer_t time_wait_timer;

    bool fin_sent;
    bool fin_acked;
    bool peer_fin_received;

    /* Only meaningful while state == dmtcp_state_syn_received - the
     * listener's handler, copied at SYN-arrival time (not referenced live)
     * since dmtcp_unlisten() could release the listener before this
     * connection's handshake finishes. Invoked once, when the final
     * handshake ACK arrives, then never consulted again. */
    dmtcp_accept_handler_t pending_accept_handler;

    dmtcp_conn_callbacks_t callbacks;
    void*                  user_data;
};

/* ---- dmtcp_wire.c: internal pseudo-header checksum helper ---- */

/**
 * @brief Compute the RFC 793 pseudo-header + segment checksum for IPv4
 *
 * Shared by dmtcp_v4_checksum_valid() (compares the result to 0) and the
 * send path (writes the result into the segment's checksum field before
 * transmit) - the same "one primitive, two roles depending on what's
 * already in the checksum field" trick dmudp_v4_checksum_valid()/
 * dmudp_send() use.
 */
uint16_t dmtcp_wire_v4_checksum(const dmip_addr_t* src_ip, const dmip_addr_t* dst_ip, const uint8_t* segment, size_t length);

/**
 * @brief IPv6 equivalent of dmtcp_wire_v4_checksum()
 */
uint16_t dmtcp_wire_v6_checksum(const dmip_addr_t* src_ip, const dmip_addr_t* dst_ip, const uint8_t* segment, size_t length);

/**
 * @brief Write the 12-byte IPv4 (RFC 793) / 40-byte IPv6 (RFC 8200 8.1)
 *        pseudo-header into `buf`
 *
 * Exposed so dmtcp_output.c's send path can build one
 * [pseudo-header][segment] buffer and checksum it directly (no second
 * allocation), the same "one buffer, no extra copy" shape dmudp_send()
 * uses - see dmtcp_output.c.
 */
void dmtcp_wire_write_v4_pseudo_header(uint8_t* buf, const dmip_addr_t* src_ip, const dmip_addr_t* dst_ip, uint16_t segment_len);
void dmtcp_wire_write_v6_pseudo_header(uint8_t* buf, const dmip_addr_t* src_ip, const dmip_addr_t* dst_ip, uint32_t segment_len);

/* ---- dmtcp_listen_table.c ---- */

int  dmtcp_listen_table_init(void);
void dmtcp_listen_table_deinit(void);

/**
 * @brief Look up the accept handler listening on `port`, if any
 *
 * Locks only for the lookup itself and returns a copy of the function
 * pointer - safe to call the returned handler without holding any lock.
 *
 * @return The registered handler, or NULL if `port` has no listener
 */
dmtcp_accept_handler_t dmtcp_listen_table_find(uint16_t port);

/* ---- dmtcp_conn_table.c ---- */

int  dmtcp_conn_table_init(void);
void dmtcp_conn_table_deinit(void);

/**
 * @brief Allocate and zero-initialize a TCB (mutex, timers, send buffer
 *        included) - not yet inserted into the connection table
 *
 * @return The new TCB, or NULL on allocation failure (nothing left
 *         partially allocated)
 */
struct dmtcp_conn* dmtcp_conn_table_create(void);

/**
 * @brief Free a TCB's own resources (mutex, timers, send buffer, the
 *        struct itself)
 *
 * The caller must have already removed `conn` from the connection table
 * (dmtcp_conn_table_remove()) - this function never touches the table.
 */
void dmtcp_conn_table_destroy(struct dmtcp_conn* conn);

/**
 * @brief Find the connection whose exact 4-tuple matches
 *
 * @return The matching TCB, or NULL if none is tracked
 */
struct dmtcp_conn* dmtcp_conn_table_find(const dmip_addr_t* local_addr, uint16_t local_port, const dmip_addr_t* peer_addr, uint16_t peer_port);

/**
 * @return 0 on success, -EEXIST if a connection with the same 4-tuple is
 *         already tracked, -ENOMEM
 */
int dmtcp_conn_table_insert(struct dmtcp_conn* conn);

/**
 * @brief Remove `conn` from the connection table (a no-op if it isn't in it)
 *
 * Does not free `conn` - see dmtcp_conn_table_destroy().
 */
void dmtcp_conn_table_remove(struct dmtcp_conn* conn);

/**
 * @brief Pick a local port in the ephemeral range not already used by any
 *        connection to the same peer 4-tuple, for dmtcp_connect()'s
 *        src_port == 0 case
 *
 * @return 0 on success (`*out_port` set), -EADDRNOTAVAIL if every port in
 *         the ephemeral range already has a connection to this peer
 */
int dmtcp_conn_table_alloc_ephemeral_port(const dmip_addr_t* peer_addr, uint16_t peer_port, uint16_t* out_port);

/**
 * @brief Whether some existing connection already uses (local_port,
 *        peer_addr, peer_port) - the same triple dmtcp_connect() must
 *        check for an explicit, caller-chosen src_port (-EADDRINUSE)
 */
bool dmtcp_conn_table_port_in_use_for_peer(uint16_t local_port, const dmip_addr_t* peer_addr, uint16_t peer_port);

/**
 * @brief Create conn's retransmission / TIME_WAIT timer
 *
 * Defined in dmtcp_output.c / dmtcp_close.c respectively (the same file
 * that defines the corresponding dmosi_timer_callback_t), and called by
 * dmtcp_conn_table_create() to wire up a fresh TCB's timers - deliberately
 * NOT done by dmtcp_conn_table_create() calling dmosi_timer_create()
 * itself with a callback defined elsewhere: see dmtcp_input_register()'s
 * doc comment in this header for why a cross-module callback registration
 * must be issued from the same translation unit that defines the callback
 * on this loader.
 *
 * @return The new timer (not started), or NULL on failure
 */
dmosi_timer_t dmtcp_output_create_rto_timer(struct dmtcp_conn* conn);
dmosi_timer_t dmtcp_close_create_time_wait_timer(struct dmtcp_conn* conn);

/**
 * @brief Reason a connection is being torn down - selects which terminal
 *        callback dmtcp_conn_table_terminate() invokes
 */
typedef enum
{
    dmtcp_terminal_closed,
    dmtcp_terminal_reset,
    dmtcp_terminal_error,
} dmtcp_terminal_reason_t;

/**
 * @brief Which (if any) of conn's own timers is currently executing the
 *        callback that led to this teardown
 *
 * dmosi_timer_destroy() joins the timer's own worker thread - calling it
 * on a timer from within that same timer's own callback is a self-join.
 * dmtcp_conn_table_terminate() stops (rather than destroys) exactly the
 * timer named here, deliberately leaking that one timer object as a
 * bounded, documented tradeoff - see docs/dmtcp.md. Both of dmtcp's timer
 * callbacks (dmtcp_rto_timer_callback(), dmtcp_time_wait_timer_callback())
 * run on their own separate worker threads, so at most one of the two is
 * ever the "current" one for a given teardown.
 */
typedef enum
{
    dmtcp_teardown_context_normal,           /* rx thread, dmtcp_close()/_abort() caller, ... */
    dmtcp_teardown_context_rto_timer,        /* running on conn->rto_timer's own callback */
    dmtcp_teardown_context_time_wait_timer,  /* running on conn->time_wait_timer's own callback */
} dmtcp_teardown_context_t;

/**
 * @brief Tear down `conn`: snapshot its callbacks, mark it closed, remove
 *        it from the table, invoke the terminal callback selected by
 *        `reason`, then free it
 *
 * Must be called with `conn->lock` NOT held - this function takes it
 * itself, exactly long enough to snapshot state, and releases it before
 * calling out to user code (the same "never call a callback while holding
 * a lock" rule as everywhere else in this module).
 *
 * @param error   Only meaningful for dmtcp_terminal_error (passed to on_error)
 * @param context See dmtcp_teardown_context_t
 */
void dmtcp_conn_table_terminate(struct dmtcp_conn* conn, dmtcp_terminal_reason_t reason, int error, dmtcp_teardown_context_t context);

/* ---- dmtcp_output.c ---- */

/**
 * @brief Build, checksum and send one TCP segment
 *
 * The low-level primitive every other send in this module goes through -
 * a bare 4-tuple + sequence fields, no TCB required, so it also serves the
 * RST-to-unmatched-segment path in dmtcp_input.c (which has no connection
 * to hang a TCB off of).
 *
 * @return 0 on success, -ENOSYS for a dmip_family_v6 `dst` (no
 *         dmip_v6_send() yet), otherwise whatever dmip_send() returns
 */
int dmtcp_send_segment(const dmip_addr_t* src, uint16_t src_port, const dmip_addr_t* dst, uint16_t dst_port,
                        uint32_t seq, uint32_t ack, uint8_t flags, uint16_t window,
                        const uint8_t* payload, size_t payload_len, uint32_t arp_timeout_ms);

/**
 * @brief Start the retransmission timer if it isn't already running
 *
 * Called whenever a SYN/data/FIN newly becomes outstanding. Assumes
 * `conn->lock` is held.
 */
void dmtcp_arm_rto(struct dmtcp_conn* conn);

/**
 * @brief Stop the retransmission timer and reset the retry counter
 *
 * Called once nothing is outstanding anymore (snd_una == snd_nxt and no
 * unacked FIN). Assumes `conn->lock` is held.
 */
void dmtcp_disarm_rto(struct dmtcp_conn* conn);

/**
 * @brief Send as much of the queued (not-yet-sent) portion of
 *        conn->send_buffer as the peer's advertised window allows
 *
 * Assumes `conn->lock` is held. A no-op if there is nothing queued or no
 * window room.
 */
void dmtcp_flush_send_buffer(struct dmtcp_conn* conn);

/* ---- dmtcp_input.c ---- */

/**
 * @brief dmip_protocol_handler_t registered for DMIP_PROTO_TCP - see
 *        dmtcp_input_register()
 */
void dmtcp_handle_ip_packet(dmip_family_t family, dmnetif_iface_t iface, const uint8_t* packet, size_t packet_len);

/**
 * @brief Register/unregister dmtcp_handle_ip_packet() with dmip, called
 *        from dmod_init()/_deinit() (see dmtcp.c)
 *
 * Kept in dmtcp_input.c, the same translation unit as
 * dmtcp_handle_ip_packet() itself - see that function's doc comment for
 * why calling dmip_register_protocol() directly from a *different* file
 * than the one defining the callback breaks on this loader.
 */
int  dmtcp_input_register(void);
void dmtcp_input_unregister(void);

#endif // DMTCP_INTERNAL_H
