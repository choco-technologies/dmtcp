/**
 * @file dmtcp_test.c
 * @brief Test steps for dmtcp
 *
 * Same infrastructure as dmudp_test.c/dmicmp_test.c: a "/dev/null"-backed
 * dmnetif fixture interface, and feed_frame() driving a hand-built,
 * correctly-checksummed IPv4/IPv6 packet straight into dmnetbridge's real
 * packet_received DIF via Dmod_GetNextDifModule()/Dmod_GetDifFunction() -
 * the same discovery dmnetbridge_handle_netif_rx() itself uses - so a
 * segment flows through the real dmnetbridge -> dmip -> dmtcp dispatch
 * chain, not a mock.
 *
 * Only the PASSIVE-open (dmtcp_listen()) path can be driven to a fully
 * ESTABLISHED dmtcp_conn_t here: handle_new_syn()'s automatic SYN-ACK reply
 * ignores its own send result (fire-and-forget, same as dmudp/dmicmp's
 * automatic replies), so a connection completes its handshake regardless
 * of whether that reply could actually reach a real driver. dmtcp_connect()
 * is different by design (see dmtcp.h): it checks and propagates its
 * first SYN's send result, and this test environment's "/dev/null" netif
 * has no real driver behind it, so that first send always ends in -EIO
 * (the same fundamental limit dmudp_test.c/dmicmp_test.c document for
 * their own full-path send tests) - meaning dmtcp_connect() can only be
 * exercised here for argument validation and the synchronous error paths,
 * never all the way to a successful, usable connection.
 *
 * The server's initial sequence number (ISS) is exactly
 * dmosi_get_tick_count() at SYN-arrival time (see docs/dmtcp.md) with no
 * added randomization - deliberately, so a test can sample the same
 * counter itself immediately before feeding a SYN and reuse that value to
 * predict the exact ISS dmtcp will choose, without any test-only API.
 */
#define ENABLE_DIF_REGISTRATIONS ON
#include "dmod_test.h"
#include "dmtcp.h"
#include "dmroute.h"
#include "dmarp.h"
#include "dmnetbridge.h"
#include "dmosi.h"
#include <string.h>
#include <errno.h>

/* Mirror src/dmtcp_internal.h's DMTCP_RTO_MS/DMTCP_MAX_RETRANSMITS - not
 * pulled in directly to avoid this test target needing its own
 * dmod_link_modules(dmosi/dmlist) just for two constants (dmtcp_internal.h
 * also declares the private struct dmtcp_conn, which needs dmlist.h's
 * plumbing indirectly) - see retransmit_limit_exceeded_fires_on_error(). */
#define TEST_EXPECTED_RTO_MS 1000u
#define TEST_EXPECTED_MAX_RETRANSMITS 5u

static dmip_addr_t make_v4(uint8_t a, uint8_t b, uint8_t c, uint8_t d)
{
    dmip_addr_t ip = { 0 };
    ip.family = dmip_family_v4;
    ip.addr.v4[0] = a;
    ip.addr.v4[1] = b;
    ip.addr.v4[2] = c;
    ip.addr.v4[3] = d;
    return ip;
}

static dmip_addr_t make_v6(uint8_t last_byte)
{
    dmip_addr_t ip = { 0 };
    ip.family = dmip_family_v6;
    ip.addr.v6[0] = 0x20;
    ip.addr.v6[1] = 0x01;
    ip.addr.v6[DMIP_IPV6_ADDR_LEN - 1] = last_byte;
    return ip;
}

static void write_u16_be(uint8_t* p, uint16_t value)
{
    p[0] = (uint8_t)(value >> 8);
    p[1] = (uint8_t)(value & 0xFFu);
}

/* dmod modules have no libc memcmp() (see dmod/src/module/string.c's
 * minimal replacement set) - a small manual comparison stands in for it. */
static bool bytes_equal(const uint8_t* a, const uint8_t* b, size_t len)
{
    for (size_t i = 0; i < len; i++)
    {
        if (a[i] != b[i])
            return false;
    }
    return true;
}

#define TEST_ETH_HEADER_LEN 14u
#define TEST_ETHERTYPE_IPV4 0x0800u
#define TEST_ETHERTYPE_IPV6 0x86DDu
#define TEST_MAX_PAYLOAD_LEN 64u
#define TEST_V4_PSEUDO_HEADER_LEN 12u
#define TEST_V6_PSEUDO_HEADER_LEN 40u
#define TEST_WINDOW 65535u

/**
 * @brief Wrap a complete IP packet in a minimal Ethernet frame and
 *        broadcast it to every packet_received DIF implementor (dmip's
 *        own, in practice) - the same discovery
 *        dmnetbridge_handle_netif_rx() itself uses
 */
static void feed_frame(dmnetif_iface_t iface, uint16_t ethertype, const uint8_t* packet, size_t packet_len)
{
    size_t frame_len = TEST_ETH_HEADER_LEN + packet_len;
    uint8_t* frame = Dmod_Malloc(frame_len);
    memset(frame, 0, TEST_ETH_HEADER_LEN);
    write_u16_be(&frame[12], ethertype);
    memcpy(frame + TEST_ETH_HEADER_LEN, packet, packet_len);

    Dmod_Context_t* implementor = NULL;
    while ((implementor = Dmod_GetNextDifModule(dmod_dmnetbridge_packet_received_sig, implementor)) != NULL)
    {
        dmod_dmnetbridge_packet_received_t fn =
            (dmod_dmnetbridge_packet_received_t)Dmod_GetDifFunction(implementor, dmod_dmnetbridge_packet_received_sig);
        if (fn != NULL)
        {
            fn(iface, frame, frame_len);
        }
    }

    Dmod_Free(frame);
}

/**
 * @brief Build a complete, correctly-checksummed TCP-over-IPv4 segment
 *        into `out` and return its length
 */
static size_t build_v4_segment(uint8_t* out, dmip_addr_t src, dmip_addr_t dst, uint16_t src_port, uint16_t dst_port,
                                uint32_t seq, uint32_t ack, uint8_t flags, const uint8_t* payload, size_t payload_len)
{
    size_t seg_len = DMTCP_HEADER_LEN + payload_len;
    dmtcp_header_t header = { .src_port = src_port, .dst_port = dst_port, .seq_num = seq, .ack_num = ack, .flags = flags, .window = TEST_WINDOW };
    dmtcp_build_header(out, seg_len, &header);
    if (payload_len > 0)
    {
        memcpy(out + DMTCP_HEADER_LEN, payload, payload_len);
    }

    uint8_t pseudo_and_segment[TEST_V4_PSEUDO_HEADER_LEN + DMTCP_HEADER_LEN + TEST_MAX_PAYLOAD_LEN];
    memcpy(&pseudo_and_segment[0], src.addr.v4, DMIP_IPV4_ADDR_LEN);
    memcpy(&pseudo_and_segment[4], dst.addr.v4, DMIP_IPV4_ADDR_LEN);
    pseudo_and_segment[8] = 0;
    pseudo_and_segment[9] = DMIP_PROTO_TCP;
    write_u16_be(&pseudo_and_segment[10], (uint16_t)seg_len);
    memcpy(&pseudo_and_segment[TEST_V4_PSEUDO_HEADER_LEN], out, seg_len);

    write_u16_be(&out[16], dmip_checksum(pseudo_and_segment, TEST_V4_PSEUDO_HEADER_LEN + seg_len));
    return seg_len;
}

/**
 * @brief Build a complete, correctly-checksummed TCP-over-IPv6 segment
 *        into `out` and return its length
 */
static size_t build_v6_segment(uint8_t* out, dmip_addr_t src, dmip_addr_t dst, uint16_t src_port, uint16_t dst_port,
                                uint32_t seq, uint32_t ack, uint8_t flags, const uint8_t* payload, size_t payload_len)
{
    size_t seg_len = DMTCP_HEADER_LEN + payload_len;
    dmtcp_header_t header = { .src_port = src_port, .dst_port = dst_port, .seq_num = seq, .ack_num = ack, .flags = flags, .window = TEST_WINDOW };
    dmtcp_build_header(out, seg_len, &header);
    if (payload_len > 0)
    {
        memcpy(out + DMTCP_HEADER_LEN, payload, payload_len);
    }

    uint8_t pseudo_and_segment[TEST_V6_PSEUDO_HEADER_LEN + DMTCP_HEADER_LEN + TEST_MAX_PAYLOAD_LEN];
    memcpy(&pseudo_and_segment[0], src.addr.v6, DMIP_IPV6_ADDR_LEN);
    memcpy(&pseudo_and_segment[16], dst.addr.v6, DMIP_IPV6_ADDR_LEN);
    pseudo_and_segment[32] = 0; pseudo_and_segment[33] = 0; pseudo_and_segment[34] = 0;
    pseudo_and_segment[35] = (uint8_t)seg_len;
    pseudo_and_segment[36] = 0; pseudo_and_segment[37] = 0; pseudo_and_segment[38] = 0;
    pseudo_and_segment[39] = DMIP_PROTO_TCP;
    memcpy(&pseudo_and_segment[TEST_V6_PSEUDO_HEADER_LEN], out, seg_len);

    write_u16_be(&out[16], dmip_checksum(pseudo_and_segment, TEST_V6_PSEUDO_HEADER_LEN + seg_len));
    return seg_len;
}

/**
 * @brief Build a complete IPv4 or IPv6 packet wrapping `segment` and feed
 *        it in via feed_frame()
 */
static void feed_tcp_packet(dmnetif_iface_t iface, dmip_family_t family, dmip_addr_t src, dmip_addr_t dst, const uint8_t* segment, size_t segment_len)
{
    if (family == dmip_family_v4)
    {
        dmip_v4_header_t header = { 0 };
        header.total_length = (uint16_t)(DMIP_V4_HEADER_LEN + segment_len);
        header.ttl = DMIP_DEFAULT_TTL;
        header.protocol = DMIP_PROTO_TCP;
        header.src = src;
        header.dst = dst;

        uint8_t packet[DMIP_V4_HEADER_LEN + DMTCP_HEADER_LEN + TEST_MAX_PAYLOAD_LEN];
        dmip_v4_build_header(packet, sizeof(packet), &header);
        memcpy(packet + DMIP_V4_HEADER_LEN, segment, segment_len);

        feed_frame(iface, TEST_ETHERTYPE_IPV4, packet, DMIP_V4_HEADER_LEN + segment_len);
    }
    else
    {
        dmip_v6_header_t header = { 0 };
        header.payload_length = (uint16_t)segment_len;
        header.next_header = DMIP_PROTO_TCP;
        header.hop_limit = DMIP_DEFAULT_HOP_LIMIT;
        header.src = src;
        header.dst = dst;

        uint8_t packet[DMIP_V6_HEADER_LEN + DMTCP_HEADER_LEN + TEST_MAX_PAYLOAD_LEN];
        dmip_v6_build_header(packet, sizeof(packet), &header);
        memcpy(packet + DMIP_V6_HEADER_LEN, segment, segment_len);

        feed_frame(iface, TEST_ETHERTYPE_IPV6, packet, DMIP_V6_HEADER_LEN + segment_len);
    }
}

#define TEST_DEVICE_PATH "/dev/null"

static dmnetif_iface_t g_iface = NULL;

void dmod_test_setup(void)
{
    g_iface = dmnetif_register("test0", TEST_DEVICE_PATH);
}

void dmod_test_teardown(void)
{
    dmnetif_unregister(g_iface);
    g_iface = NULL;
}

/* ---- Header build/parse ---- */

DMOD_TEST_STEP(build_header_rejects_bad_arguments)
{
    uint8_t buffer[DMTCP_HEADER_LEN];
    dmtcp_header_t header = { 0 };

    DMOD_TEST_EXPECT_EQ(dmtcp_build_header(NULL, sizeof(buffer), &header), -EINVAL);
    DMOD_TEST_EXPECT_EQ(dmtcp_build_header(buffer, sizeof(buffer), NULL), -EINVAL);
    DMOD_TEST_EXPECT_EQ(dmtcp_build_header(buffer, DMTCP_HEADER_LEN - 1, &header), -EINVAL);
}

DMOD_TEST_STEP(parse_header_rejects_bad_arguments)
{
    uint8_t buffer[DMTCP_HEADER_LEN] = { 0 };
    dmtcp_header_t header = { 0 };
    size_t header_len = 0;

    DMOD_TEST_EXPECT_EQ(dmtcp_parse_header(NULL, sizeof(buffer), &header, &header_len), -EINVAL);
    DMOD_TEST_EXPECT_EQ(dmtcp_parse_header(buffer, sizeof(buffer), NULL, &header_len), -EINVAL);
    DMOD_TEST_EXPECT_EQ(dmtcp_parse_header(buffer, sizeof(buffer), &header, NULL), -EINVAL);
    DMOD_TEST_EXPECT_EQ(dmtcp_parse_header(buffer, DMTCP_HEADER_LEN - 1, &header, &header_len), -EINVAL);
}

DMOD_TEST_STEP(build_and_parse_header_round_trip)
{
    dmtcp_header_t header = { .src_port = 5353, .dst_port = 80, .seq_num = 111, .ack_num = 222, .flags = DMTCP_FLAG_SYN | DMTCP_FLAG_ACK, .window = 4096 };
    uint8_t buffer[DMTCP_HEADER_LEN];

    DMOD_TEST_EXPECT_EQ(dmtcp_build_header(buffer, sizeof(buffer), &header), 0);

    dmtcp_header_t parsed = { 0 };
    size_t header_len = 0;
    DMOD_TEST_EXPECT_EQ(dmtcp_parse_header(buffer, sizeof(buffer), &parsed, &header_len), 0);
    DMOD_TEST_EXPECT_EQ(header_len, (size_t)DMTCP_HEADER_LEN);
    DMOD_TEST_EXPECT_EQ(parsed.src_port, (uint16_t)5353);
    DMOD_TEST_EXPECT_EQ(parsed.dst_port, (uint16_t)80);
    DMOD_TEST_EXPECT_EQ(parsed.seq_num, (uint32_t)111);
    DMOD_TEST_EXPECT_EQ(parsed.ack_num, (uint32_t)222);
    DMOD_TEST_EXPECT_EQ(parsed.flags, (uint8_t)(DMTCP_FLAG_SYN | DMTCP_FLAG_ACK));
    DMOD_TEST_EXPECT_EQ(parsed.window, (uint16_t)4096);
}

DMOD_TEST_STEP(parse_header_skips_unknown_options)
{
    uint8_t buffer[DMTCP_HEADER_LEN + 4] = { 0 };
    dmtcp_header_t header = { .src_port = 1, .dst_port = 2, .flags = DMTCP_FLAG_SYN, .window = 100 };
    DMOD_TEST_EXPECT_EQ(dmtcp_build_header(buffer, DMTCP_HEADER_LEN, &header), 0);
    buffer[12] = 0x60u; /* Data Offset = 6 (one 4-byte option word) */

    dmtcp_header_t parsed = { 0 };
    size_t header_len = 0;
    DMOD_TEST_EXPECT_EQ(dmtcp_parse_header(buffer, sizeof(buffer), &parsed, &header_len), 0);
    DMOD_TEST_EXPECT_EQ(header_len, (size_t)(DMTCP_HEADER_LEN + 4));

    buffer[12] = 0x40u; /* Data Offset = 4 - invalid, smaller than the minimum */
    DMOD_TEST_EXPECT_EQ(dmtcp_parse_header(buffer, sizeof(buffer), &parsed, &header_len), -EPROTO);
}

/* ---- Checksum ---- */

DMOD_TEST_STEP(v4_checksum_valid_round_trip)
{
    dmip_addr_t src = make_v4(10, 1, 0, 1);
    dmip_addr_t dst = make_v4(10, 1, 0, 2);
    uint8_t payload[4] = { 'p', 'i', 'n', 'g' };
    uint8_t segment[DMTCP_HEADER_LEN + sizeof(payload)];
    size_t segment_len = build_v4_segment(segment, src, dst, 1234, 80, 1, 0, DMTCP_FLAG_ACK, payload, sizeof(payload));

    DMOD_TEST_EXPECT_TRUE(dmtcp_v4_checksum_valid(&src, &dst, segment, segment_len));

    segment[DMTCP_HEADER_LEN] ^= 0xFF;
    DMOD_TEST_EXPECT_FALSE(dmtcp_v4_checksum_valid(&src, &dst, segment, segment_len));
}

DMOD_TEST_STEP(v6_checksum_valid_round_trip)
{
    dmip_addr_t src = make_v6(1);
    dmip_addr_t dst = make_v6(2);
    uint8_t payload[4] = { 'p', 'o', 'n', 'g' };
    uint8_t segment[DMTCP_HEADER_LEN + sizeof(payload)];
    size_t segment_len = build_v6_segment(segment, src, dst, 4321, 80, 1, 0, DMTCP_FLAG_ACK, payload, sizeof(payload));

    DMOD_TEST_EXPECT_TRUE(dmtcp_v6_checksum_valid(&src, &dst, segment, segment_len));

    segment[DMTCP_HEADER_LEN] ^= 0xFF;
    DMOD_TEST_EXPECT_FALSE(dmtcp_v6_checksum_valid(&src, &dst, segment, segment_len));
}

DMOD_TEST_STEP(checksum_valid_rejects_bad_arguments)
{
    dmip_addr_t v4_addr = make_v4(1, 1, 1, 1);
    dmip_addr_t v6_addr = make_v6(1);
    uint8_t segment[DMTCP_HEADER_LEN] = { 0 };

    DMOD_TEST_EXPECT_FALSE(dmtcp_v4_checksum_valid(NULL, &v4_addr, segment, sizeof(segment)));
    DMOD_TEST_EXPECT_FALSE(dmtcp_v4_checksum_valid(&v4_addr, &v4_addr, segment, DMTCP_HEADER_LEN - 1));
    DMOD_TEST_EXPECT_FALSE(dmtcp_v4_checksum_valid(&v6_addr, &v4_addr, segment, sizeof(segment)));

    DMOD_TEST_EXPECT_FALSE(dmtcp_v6_checksum_valid(NULL, &v6_addr, segment, sizeof(segment)));
    DMOD_TEST_EXPECT_FALSE(dmtcp_v6_checksum_valid(&v4_addr, &v6_addr, segment, sizeof(segment)));
}

DMOD_TEST_STEP(wire_checksum_zero_is_not_special_cased)
{
    /* Unlike UDP's RFC768 "0 means none" allowance, TCP has no such
     * exception for either family - a wire checksum of 0 on a segment
     * whose real checksum isn't 0 must be rejected like any other
     * mismatch (see dmtcp.h). */
    dmip_addr_t src = make_v4(10, 9, 0, 1);
    dmip_addr_t dst = make_v4(10, 9, 0, 2);
    uint8_t segment[DMTCP_HEADER_LEN];
    size_t segment_len = build_v4_segment(segment, src, dst, 1, 80, 1, 0, DMTCP_FLAG_ACK, NULL, 0);
    write_u16_be(&segment[16], 0);

    DMOD_TEST_EXPECT_FALSE(dmtcp_v4_checksum_valid(&src, &dst, segment, segment_len));
}

/* ---- Listen / listen_any / unlisten ---- */

static void unused_accept_handler(dmtcp_conn_t conn, const dmip_addr_t* peer, uint16_t peer_port, dmnetif_iface_t iface)
{
    (void)conn; (void)peer; (void)peer_port; (void)iface;
}

#define TEST_FIXED_PORT_BASE 45000u

DMOD_TEST_STEP(listen_rejects_null_handler)
{
    DMOD_TEST_EXPECT_EQ(dmtcp_listen(TEST_FIXED_PORT_BASE, NULL), -EINVAL);
}

DMOD_TEST_STEP(listen_rejects_port_zero)
{
    DMOD_TEST_EXPECT_EQ(dmtcp_listen(0, unused_accept_handler), -EINVAL);
}

DMOD_TEST_STEP(listen_twice_same_port_returns_eexist)
{
    DMOD_TEST_EXPECT_EQ(dmtcp_listen(TEST_FIXED_PORT_BASE + 1, unused_accept_handler), 0);
    DMOD_TEST_EXPECT_EQ(dmtcp_listen(TEST_FIXED_PORT_BASE + 1, unused_accept_handler), -EEXIST);

    dmtcp_unlisten(TEST_FIXED_PORT_BASE + 1);
}

DMOD_TEST_STEP(unlisten_unlistened_port_is_safe)
{
    dmtcp_unlisten(TEST_FIXED_PORT_BASE + 999); /* must not crash */
}

DMOD_TEST_STEP(listen_after_unlisten_succeeds)
{
    DMOD_TEST_EXPECT_EQ(dmtcp_listen(TEST_FIXED_PORT_BASE + 2, unused_accept_handler), 0);
    dmtcp_unlisten(TEST_FIXED_PORT_BASE + 2);
    DMOD_TEST_EXPECT_EQ(dmtcp_listen(TEST_FIXED_PORT_BASE + 2, unused_accept_handler), 0);

    dmtcp_unlisten(TEST_FIXED_PORT_BASE + 2);
}

DMOD_TEST_STEP(listen_any_rejects_null_arguments)
{
    uint16_t port = 0;
    DMOD_TEST_EXPECT_EQ(dmtcp_listen_any(NULL, &port), -EINVAL);
    DMOD_TEST_EXPECT_EQ(dmtcp_listen_any(unused_accept_handler, NULL), -EINVAL);
}

DMOD_TEST_STEP(listen_any_returns_port_in_ephemeral_range)
{
    uint16_t port = 0;
    DMOD_TEST_EXPECT_EQ(dmtcp_listen_any(unused_accept_handler, &port), 0);
    DMOD_TEST_EXPECT_TRUE(port >= DMTCP_PORT_EPHEMERAL_FIRST && port <= DMTCP_PORT_EPHEMERAL_LAST);
    DMOD_TEST_EXPECT_EQ(dmtcp_listen(port, unused_accept_handler), -EEXIST);

    dmtcp_unlisten(port);
}

DMOD_TEST_STEP(listen_any_skips_already_listened_port)
{
    uint16_t port_a = 0;
    uint16_t port_b = 0;
    DMOD_TEST_EXPECT_EQ(dmtcp_listen_any(unused_accept_handler, &port_a), 0);
    DMOD_TEST_EXPECT_EQ(dmtcp_listen_any(unused_accept_handler, &port_b), 0);
    DMOD_TEST_EXPECT_TRUE(port_a != port_b);

    dmtcp_unlisten(port_a);
    dmtcp_unlisten(port_b);
}

/* ---- connect() argument validation ---- */

DMOD_TEST_STEP(connect_rejects_null_arguments)
{
    dmip_addr_t dst = make_v4(10, 10, 0, 1);
    dmtcp_conn_t conn = NULL;

    DMOD_TEST_EXPECT_EQ(dmtcp_connect(NULL, 80, 0, NULL, NULL, &conn), -EINVAL);
    DMOD_TEST_EXPECT_EQ(dmtcp_connect(&dst, 0, 0, NULL, NULL, &conn), -EINVAL);
    DMOD_TEST_EXPECT_EQ(dmtcp_connect(&dst, 80, 0, NULL, NULL, NULL), -EINVAL);
}

DMOD_TEST_STEP(connect_v6_returns_enosys_immediately)
{
    dmip_addr_t dst = make_v6(5);
    dmtcp_conn_t conn = NULL;

    DMOD_TEST_EXPECT_EQ(dmtcp_connect(&dst, 80, 0, NULL, NULL, &conn), -ENOSYS);
}

DMOD_TEST_STEP(connect_v4_no_route_returns_enetunreach)
{
    dmip_addr_t dst = make_v4(203, 0, 113, 9); /* TEST-NET-3 - no route added anywhere in this file */
    dmtcp_conn_t conn = NULL;

    DMOD_TEST_EXPECT_EQ(dmtcp_connect(&dst, 80, 0, NULL, NULL, &conn), -ENETUNREACH);
}

/* ---- Passive handshake helper ---- */

static bool          g_accept_called;
static dmtcp_conn_t   g_accepted_conn;
static dmip_addr_t    g_accept_peer;
static uint16_t       g_accept_peer_port;

static void recording_accept_handler(dmtcp_conn_t conn, const dmip_addr_t* peer, uint16_t peer_port, dmnetif_iface_t iface)
{
    (void)iface;
    g_accept_called = true;
    g_accepted_conn = conn;
    g_accept_peer = *peer;
    g_accept_peer_port = peer_port;
}

static void reset_accept_recording(void)
{
    g_accept_called = false;
    g_accepted_conn = NULL;
    memset(&g_accept_peer, 0, sizeof(g_accept_peer));
    g_accept_peer_port = 0;
}

/**
 * @brief Drive a full passive-open handshake to completion and return the
 *        resulting connection
 *
 * The server's ISS is predicted by sampling dmosi_get_tick_count()
 * immediately before feeding the SYN - see this file's top comment.
 */
static dmtcp_conn_t establish_passive_connection(uint16_t listen_port, dmip_addr_t client_addr, uint16_t client_port,
                                                  dmip_addr_t server_addr, uint32_t client_iss, uint32_t* out_server_iss)
{
    reset_accept_recording();
    DMOD_TEST_EXPECT_EQ(dmtcp_listen(listen_port, recording_accept_handler), 0);

    uint8_t syn[DMTCP_HEADER_LEN];
    size_t syn_len = build_v4_segment(syn, client_addr, server_addr, client_port, listen_port, client_iss, 0, DMTCP_FLAG_SYN, NULL, 0);

    uint32_t sampled_iss = dmosi_get_tick_count();
    feed_tcp_packet(g_iface, dmip_family_v4, client_addr, server_addr, syn, syn_len);

    /* The server's ISS is exactly dmosi_get_tick_count() at the instant dmtcp
     * itself samples it inside the (synchronous) feed above - sampled_iss
     * matches unless the tick advanced between our read and dmtcp's own,
     * which can happen right at a tick boundary. Try both candidates: an
     * ACK with the wrong ack_num is simply ignored (delta out of range for
     * the single outstanding SYN), so trying the wrong one first is harmless. */
    uint32_t candidates[2] = { sampled_iss, sampled_iss + 1u };
    uint32_t server_iss = sampled_iss;
    for (size_t i = 0; i < 2 && !g_accept_called; i++)
    {
        uint8_t ack[DMTCP_HEADER_LEN];
        size_t ack_len = build_v4_segment(ack, client_addr, server_addr, client_port, listen_port, client_iss + 1, candidates[i] + 1, DMTCP_FLAG_ACK, NULL, 0);
        feed_tcp_packet(g_iface, dmip_family_v4, client_addr, server_addr, ack, ack_len);
        if (g_accept_called)
        {
            server_iss = candidates[i];
        }
    }

    if (out_server_iss != NULL)
    {
        *out_server_iss = server_iss;
    }
    return g_accepted_conn;
}

/* ---- End-to-end passive handshake ---- */

DMOD_TEST_STEP(passive_handshake_reaches_established_and_fires_accept_handler)
{
    dmip_addr_t client = make_v4(10, 20, 0, 1);
    dmip_addr_t server = make_v4(10, 20, 0, 2);
    dmtcp_conn_t conn = establish_passive_connection(TEST_FIXED_PORT_BASE + 10, client, 6000, server, 1000, NULL);

    DMOD_TEST_EXPECT_TRUE(g_accept_called);
    DMOD_TEST_EXPECT_NOT_NULL(conn);
    DMOD_TEST_EXPECT_TRUE(bytes_equal(g_accept_peer.addr.v4, client.addr.v4, DMIP_IPV4_ADDR_LEN));
    DMOD_TEST_EXPECT_EQ(g_accept_peer_port, (uint16_t)6000);
    DMOD_TEST_EXPECT_EQ(dmtcp_conn_get_state(conn), dmtcp_state_established);

    dmip_addr_t local_addr = { 0 };
    uint16_t local_port = 0;
    DMOD_TEST_EXPECT_EQ(dmtcp_conn_get_local_endpoint(conn, &local_addr, &local_port), 0);
    DMOD_TEST_EXPECT_EQ(local_port, (uint16_t)(TEST_FIXED_PORT_BASE + 10));

    dmtcp_abort(conn);
    dmtcp_unlisten(TEST_FIXED_PORT_BASE + 10);
}

DMOD_TEST_STEP(syn_to_unlistened_port_does_not_crash)
{
    dmip_addr_t client = make_v4(10, 20, 1, 1);
    dmip_addr_t server = make_v4(10, 20, 1, 2);
    uint8_t syn[DMTCP_HEADER_LEN];
    size_t syn_len = build_v4_segment(syn, client, server, 6001, TEST_FIXED_PORT_BASE + 11, 2000, 0, DMTCP_FLAG_SYN, NULL, 0);

    feed_tcp_packet(g_iface, dmip_family_v4, client, server, syn, syn_len); /* no listener on +11 */
}

/* ---- Data transfer ---- */

static bool    g_data_called;
static bool    g_eof_called;
static uint8_t g_data_payload[TEST_MAX_PAYLOAD_LEN];
static size_t  g_data_payload_len;

static void recording_data_handler(dmtcp_conn_t conn, const uint8_t* data, size_t data_len, void* user_data)
{
    (void)conn; (void)user_data;
    if (data == NULL)
    {
        g_eof_called = true;
        return;
    }
    g_data_called = true;
    g_data_payload_len = data_len;
    if (data_len > 0)
    {
        memcpy(g_data_payload, data, data_len);
    }
}

static void reset_data_recording(void)
{
    g_data_called = false;
    g_eof_called = false;
    g_data_payload_len = 0;
}

DMOD_TEST_STEP(established_conn_delivers_in_order_data_via_on_data)
{
    dmip_addr_t client = make_v4(10, 21, 0, 1);
    dmip_addr_t server = make_v4(10, 21, 0, 2);
    uint32_t server_iss = 0;
    dmtcp_conn_t conn = establish_passive_connection(TEST_FIXED_PORT_BASE + 20, client, 6002, server, 3000, &server_iss);
    DMOD_TEST_EXPECT_NOT_NULL(conn);

    dmtcp_conn_callbacks_t callbacks = { .on_data = recording_data_handler };
    DMOD_TEST_EXPECT_EQ(dmtcp_conn_set_callbacks(conn, &callbacks, NULL), 0);

    reset_data_recording();
    uint8_t payload[4] = { 'd', 'a', 't', 'a' };
    uint8_t segment[DMTCP_HEADER_LEN + sizeof(payload)];
    size_t segment_len = build_v4_segment(segment, client, server, 6002, TEST_FIXED_PORT_BASE + 20, 3001, server_iss + 1, DMTCP_FLAG_ACK, payload, sizeof(payload));
    feed_tcp_packet(g_iface, dmip_family_v4, client, server, segment, segment_len);

    DMOD_TEST_EXPECT_TRUE(g_data_called);
    DMOD_TEST_EXPECT_EQ(g_data_payload_len, sizeof(payload));
    DMOD_TEST_EXPECT_TRUE(bytes_equal(g_data_payload, payload, sizeof(payload)));

    dmtcp_abort(conn);
    dmtcp_unlisten(TEST_FIXED_PORT_BASE + 20);
}

DMOD_TEST_STEP(out_of_order_segment_is_dropped)
{
    dmip_addr_t client = make_v4(10, 21, 1, 1);
    dmip_addr_t server = make_v4(10, 21, 1, 2);
    uint32_t server_iss = 0;
    dmtcp_conn_t conn = establish_passive_connection(TEST_FIXED_PORT_BASE + 21, client, 6003, server, 3100, &server_iss);
    DMOD_TEST_EXPECT_NOT_NULL(conn);

    dmtcp_conn_callbacks_t callbacks = { .on_data = recording_data_handler };
    DMOD_TEST_EXPECT_EQ(dmtcp_conn_set_callbacks(conn, &callbacks, NULL), 0);

    reset_data_recording();
    uint8_t payload[4] = { 'l', 'a', 't', 'e' };
    uint8_t segment[DMTCP_HEADER_LEN + sizeof(payload)];
    /* seq is one byte ahead of the expected rcv_nxt (3101) - out of order */
    size_t segment_len = build_v4_segment(segment, client, server, 6003, TEST_FIXED_PORT_BASE + 21, 3102, server_iss + 1, DMTCP_FLAG_ACK, payload, sizeof(payload));
    feed_tcp_packet(g_iface, dmip_family_v4, client, server, segment, segment_len);

    DMOD_TEST_EXPECT_FALSE(g_data_called);

    dmtcp_abort(conn);
    dmtcp_unlisten(TEST_FIXED_PORT_BASE + 21);
}

DMOD_TEST_STEP(dmtcp_send_partial_write_when_buffer_full)
{
    dmip_addr_t client = make_v4(10, 21, 2, 1);
    dmip_addr_t server = make_v4(10, 21, 2, 2);
    dmtcp_conn_t conn = establish_passive_connection(TEST_FIXED_PORT_BASE + 22, client, 6004, server, 3200, NULL);
    DMOD_TEST_EXPECT_NOT_NULL(conn);

    /* No window growth ever arrives from the peer in this test, so
     * everything queued beyond the peer's advertised window (TEST_WINDOW,
     * far larger than DMTCP_SEND_BUFFER_LEN) simply accumulates in
     * conn->send_buffer - filling it proves the partial-write contract. */
    static uint8_t big_payload[8192];
    int first = dmtcp_send(conn, big_payload, sizeof(big_payload));
    DMOD_TEST_EXPECT_TRUE(first > 0 && (size_t)first < sizeof(big_payload));

    int second = dmtcp_send(conn, big_payload, sizeof(big_payload));
    DMOD_TEST_EXPECT_EQ(second, 0);

    dmtcp_abort(conn);
    dmtcp_unlisten(TEST_FIXED_PORT_BASE + 22);
}

/* ---- Graceful close ---- */

static bool g_closed_called;
static bool g_reset_called;
static bool g_error_called;
static int  g_error_code;

static void recording_closed_handler(dmtcp_conn_t conn, void* user_data)
{
    (void)conn; (void)user_data;
    g_closed_called = true;
}

static void recording_reset_handler(dmtcp_conn_t conn, void* user_data)
{
    (void)conn; (void)user_data;
    g_reset_called = true;
}

static void recording_error_handler(dmtcp_conn_t conn, int error, void* user_data)
{
    (void)conn; (void)user_data;
    g_error_called = true;
    g_error_code = error;
}

static void reset_terminal_recording(void)
{
    g_closed_called = false;
    g_reset_called = false;
    g_error_called = false;
    g_error_code = 0;
}

DMOD_TEST_STEP(dmtcp_close_reaches_time_wait_and_delivers_eof)
{
    dmip_addr_t client = make_v4(10, 22, 0, 1);
    dmip_addr_t server = make_v4(10, 22, 0, 2);
    uint32_t server_iss = 0;
    dmtcp_conn_t conn = establish_passive_connection(TEST_FIXED_PORT_BASE + 30, client, 6005, server, 4000, &server_iss);
    DMOD_TEST_EXPECT_NOT_NULL(conn);

    reset_terminal_recording();
    reset_data_recording();
    dmtcp_conn_callbacks_t callbacks = { .on_data = recording_data_handler, .on_closed = recording_closed_handler };
    DMOD_TEST_EXPECT_EQ(dmtcp_conn_set_callbacks(conn, &callbacks, NULL), 0);

    /* No data was ever sent by the server before this - our own FIN's
     * sequence number is therefore exactly server_iss + 1 (only the SYN
     * consumed a sequence number so far), and snd_nxt becomes
     * server_iss + 2 once it's sent.
     *
     * dmtcp_close()'s return value is only a diagnostic passthrough of the
     * FIN's first send attempt (see dmtcp.h) - no route is seeded for this
     * test's network, so it's expected to be a send error here, not 0; the
     * local state transition happens regardless, which is what's actually
     * under test. */
    dmtcp_close(conn);
    DMOD_TEST_EXPECT_EQ(dmtcp_conn_get_state(conn), dmtcp_state_fin_wait_1);

    uint8_t ack_of_fin[DMTCP_HEADER_LEN];
    size_t ack_len = build_v4_segment(ack_of_fin, client, server, 6005, TEST_FIXED_PORT_BASE + 30, 4001, server_iss + 2, DMTCP_FLAG_ACK, NULL, 0);
    feed_tcp_packet(g_iface, dmip_family_v4, client, server, ack_of_fin, ack_len);
    DMOD_TEST_EXPECT_EQ(dmtcp_conn_get_state(conn), dmtcp_state_fin_wait_2);

    uint8_t peer_fin[DMTCP_HEADER_LEN];
    size_t fin_len = build_v4_segment(peer_fin, client, server, 6005, TEST_FIXED_PORT_BASE + 30, 4001, server_iss + 2, (uint8_t)(DMTCP_FLAG_FIN | DMTCP_FLAG_ACK), NULL, 0);
    feed_tcp_packet(g_iface, dmip_family_v4, client, server, peer_fin, fin_len);

    DMOD_TEST_EXPECT_TRUE(g_eof_called);
    DMOD_TEST_EXPECT_EQ(dmtcp_conn_get_state(conn), dmtcp_state_time_wait);
    DMOD_TEST_EXPECT_FALSE(g_closed_called); /* TIME_WAIT hasn't expired yet */

    dmtcp_unlisten(TEST_FIXED_PORT_BASE + 30);
}

/* ---- RST handling ---- */

DMOD_TEST_STEP(dmtcp_abort_sends_rst_and_fires_on_reset)
{
    dmip_addr_t client = make_v4(10, 23, 0, 1);
    dmip_addr_t server = make_v4(10, 23, 0, 2);
    dmtcp_conn_t conn = establish_passive_connection(TEST_FIXED_PORT_BASE + 31, client, 6006, server, 5000, NULL);
    DMOD_TEST_EXPECT_NOT_NULL(conn);

    reset_terminal_recording();
    dmtcp_conn_callbacks_t callbacks = { .on_reset = recording_reset_handler };
    DMOD_TEST_EXPECT_EQ(dmtcp_conn_set_callbacks(conn, &callbacks, NULL), 0);

    DMOD_TEST_EXPECT_EQ(dmtcp_abort(conn), 0);
    DMOD_TEST_EXPECT_TRUE(g_reset_called);

    dmtcp_unlisten(TEST_FIXED_PORT_BASE + 31);
}

DMOD_TEST_STEP(peer_rst_with_matching_seq_tears_down_and_fires_on_reset)
{
    dmip_addr_t client = make_v4(10, 23, 1, 1);
    dmip_addr_t server = make_v4(10, 23, 1, 2);
    uint32_t server_iss = 0;
    dmtcp_conn_t conn = establish_passive_connection(TEST_FIXED_PORT_BASE + 32, client, 6007, server, 5100, &server_iss);
    DMOD_TEST_EXPECT_NOT_NULL(conn);

    reset_terminal_recording();
    dmtcp_conn_callbacks_t callbacks = { .on_reset = recording_reset_handler };
    DMOD_TEST_EXPECT_EQ(dmtcp_conn_set_callbacks(conn, &callbacks, NULL), 0);

    uint8_t rst[DMTCP_HEADER_LEN];
    size_t rst_len = build_v4_segment(rst, client, server, 6007, TEST_FIXED_PORT_BASE + 32, 5101, 0, DMTCP_FLAG_RST, NULL, 0);
    feed_tcp_packet(g_iface, dmip_family_v4, client, server, rst, rst_len);

    DMOD_TEST_EXPECT_TRUE(g_reset_called);

    dmtcp_unlisten(TEST_FIXED_PORT_BASE + 32);
}

DMOD_TEST_STEP(peer_rst_with_wrong_seq_is_ignored)
{
    dmip_addr_t client = make_v4(10, 23, 2, 1);
    dmip_addr_t server = make_v4(10, 23, 2, 2);
    uint32_t server_iss = 0;
    dmtcp_conn_t conn = establish_passive_connection(TEST_FIXED_PORT_BASE + 33, client, 6008, server, 5200, &server_iss);
    DMOD_TEST_EXPECT_NOT_NULL(conn);

    reset_terminal_recording();
    dmtcp_conn_callbacks_t callbacks = { .on_reset = recording_reset_handler };
    DMOD_TEST_EXPECT_EQ(dmtcp_conn_set_callbacks(conn, &callbacks, NULL), 0);

    uint8_t rst[DMTCP_HEADER_LEN];
    size_t rst_len = build_v4_segment(rst, client, server, 6008, TEST_FIXED_PORT_BASE + 33, 9999, 0, DMTCP_FLAG_RST, NULL, 0); /* wrong seq */
    feed_tcp_packet(g_iface, dmip_family_v4, client, server, rst, rst_len);

    DMOD_TEST_EXPECT_FALSE(g_reset_called);
    DMOD_TEST_EXPECT_EQ(dmtcp_conn_get_state(conn), dmtcp_state_established);

    dmtcp_abort(conn);
    dmtcp_unlisten(TEST_FIXED_PORT_BASE + 33);
}

/* ---- Malformed/checksum (smoke tests - no accept must fire) ---- */

DMOD_TEST_STEP(malformed_short_segment_dropped)
{
    reset_accept_recording();
    DMOD_TEST_EXPECT_EQ(dmtcp_listen(TEST_FIXED_PORT_BASE + 40, recording_accept_handler), 0);

    dmip_addr_t client = make_v4(10, 24, 0, 1);
    dmip_addr_t server = make_v4(10, 24, 0, 2);
    uint8_t short_segment[DMTCP_HEADER_LEN - 1] = { 0 };

    feed_tcp_packet(g_iface, dmip_family_v4, client, server, short_segment, sizeof(short_segment)); /* must not crash */

    DMOD_TEST_EXPECT_FALSE(g_accept_called);

    dmtcp_unlisten(TEST_FIXED_PORT_BASE + 40);
}

DMOD_TEST_STEP(corrupted_checksum_syn_dropped_silently)
{
    reset_accept_recording();
    DMOD_TEST_EXPECT_EQ(dmtcp_listen(TEST_FIXED_PORT_BASE + 41, recording_accept_handler), 0);

    dmip_addr_t client = make_v4(10, 24, 1, 1);
    dmip_addr_t server = make_v4(10, 24, 1, 2);
    uint8_t syn[DMTCP_HEADER_LEN];
    size_t syn_len = build_v4_segment(syn, client, server, 6009, TEST_FIXED_PORT_BASE + 41, 6000, 0, DMTCP_FLAG_SYN, NULL, 0);
    syn[16] ^= 0xFF; /* corrupt the checksum */

    feed_tcp_packet(g_iface, dmip_family_v4, client, server, syn, syn_len);

    DMOD_TEST_EXPECT_FALSE(g_accept_called);

    dmtcp_unlisten(TEST_FIXED_PORT_BASE + 41);
}

/* ---- Retransmission retry limit (slow - exercises the timer-context
 * teardown path, see dmtcp_teardown_context_t in src/dmtcp_internal.h) ---- */

DMOD_TEST_STEP(retransmit_limit_exceeded_fires_on_error)
{
    dmip_addr_t client = make_v4(10, 25, 0, 1);
    dmip_addr_t server = make_v4(10, 25, 0, 2);
    dmtcp_conn_t conn = establish_passive_connection(TEST_FIXED_PORT_BASE + 50, client, 6010, server, 7000, NULL);
    DMOD_TEST_EXPECT_NOT_NULL(conn);

    reset_terminal_recording();
    dmtcp_conn_callbacks_t callbacks = { .on_error = recording_error_handler };
    DMOD_TEST_EXPECT_EQ(dmtcp_conn_set_callbacks(conn, &callbacks, NULL), 0);

    uint8_t payload[4] = { 'x', 'x', 'x', 'x' };
    DMOD_TEST_EXPECT_EQ(dmtcp_send(conn, payload, sizeof(payload)), (int)sizeof(payload));

    /* Never ACK it - after TEST_EXPECTED_MAX_RETRANSMITS+1 silent RTO
     * fires, the connection must give up and report -ETIMEDOUT, without
     * crashing or deadlocking (the scenario dmtcp_teardown_context_rto_timer
     * exists to make safe). */
    dmosi_thread_sleep((TEST_EXPECTED_MAX_RETRANSMITS + 2) * TEST_EXPECTED_RTO_MS);

    DMOD_TEST_EXPECT_TRUE(g_error_called);
    DMOD_TEST_EXPECT_EQ(g_error_code, -ETIMEDOUT);

    dmtcp_unlisten(TEST_FIXED_PORT_BASE + 50);
}
