/* Exercise production TCP output with an ARP miss on the RX-pump thread. */
#include "../src/dmtcp_output.c"
#include "../src/dmtcp_input.c"
#include "../src/dmtcp_close.c"
#include <stdio.h>
#include <stdlib.h>

#define CHECK(e) do { if (!(e)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #e); return 1; } } while (0)
static struct dmtcp_conn connection;
static uint8_t send_buffer[DMTCP_SEND_BUFFER_LEN];
static unsigned sends, waits, timer_starts, timer_stops;
static bool resolved;

static void* allocate(size_t size, const char* owner) { (void)owner; return malloc(size); }
static void release(void* ptr, bool merge) { (void)merge; free(ptr); }
static int lock_mutex(dmosi_mutex_t lock) { (void)lock; return 0; }
static int unlock_mutex(dmosi_mutex_t lock) { (void)lock; return 0; }
static int start_timer(dmosi_timer_t timer) { (void)timer; timer_starts++; return 0; }
static int stop_timer(dmosi_timer_t timer) { (void)timer; timer_stops++; return 0; }
static uint16_t checksum(const void* data, size_t size) { (void)data; (void)size; return 0; }
static uint16_t identification(void) { return 0; }
static uint32_t ticks(void) { return 100; }
static int send_ip(const dmip_header_t* header, const void* data, size_t size, uint32_t timeout)
{
    (void)header; (void)data; (void)size;
    sends++;
    if (!resolved && timeout > 0) waits++;
    return resolved ? 0 : -EHOSTUNREACH;
}

/* Table lifetime is independent of the ARP/RTO interaction under test. */
struct dmtcp_conn* dmtcp_conn_table_create(void) { return &connection; }
int dmtcp_conn_table_insert(struct dmtcp_conn* conn) { (void)conn; return 0; }
void dmtcp_conn_table_destroy(struct dmtcp_conn* conn) { (void)conn; }
void dmtcp_conn_table_terminate(struct dmtcp_conn* conn, dmtcp_terminal_reason_t reason, int error, dmtcp_teardown_context_t context)
{ (void)conn; (void)reason; (void)error; (void)context; }

static void reset(void)
{
    memset(&connection, 0, sizeof(connection));
    connection.magic = DMTCP_CONN_MAGIC;
    connection.state = dmtcp_state_established;
    connection.local_addr.family = connection.peer_addr.family = dmip_family_v4;
    connection.local_port = 21; connection.peer_port = 12345;
    connection.snd_una = connection.snd_nxt = 100;
    connection.rcv_nxt = 200;
    connection.snd_wnd = connection.rcv_wnd = DMTCP_DEFAULT_RECV_WINDOW;
    connection.send_buffer = send_buffer;
    sends = waits = timer_starts = timer_stops = 0;
    resolved = false;
    Dmod_MallocEx = allocate; Dmod_FreeEx = release;
    dmosi_mutex_lock = lock_mutex; dmosi_mutex_unlock = unlock_mutex;
    dmosi_timer_start = start_timer; dmosi_timer_stop = stop_timer;
    dmosi_get_tick_count = ticks;
    dmip_send = send_ip; dmip_checksum = checksum; dmip_v4_next_identification = identification;
}

static int lost_ack_is_recovered_by_peer_retry(void)
{
    reset();
    const uint8_t data[] = {1, 2, 3, 4};
    dmtcp_header_t header = {.seq_num = 200, .flags = DMTCP_FLAG_ACK};
    struct segment_result result = {0};
    apply_incoming_data_and_fin(&connection, &header, data, sizeof(data), &result);
    CHECK(sends == 1 && waits == 0 && result.has_data && connection.rcv_nxt == 204);
    /* Returning from RX lets the same pump cache the queued ARP reply. */
    resolved = true;
    memset(&result, 0, sizeof(result));
    apply_incoming_data_and_fin(&connection, &header, data, sizeof(data), &result);
    CHECK(sends == 2 && waits == 0 && !result.has_data && connection.rcv_nxt == 204);
    return 0;
}

static int queued_data_survives_arp_miss(void)
{
    reset();
    memcpy(send_buffer, "test", 4); connection.send_buffer_len = 4;
    dmtcp_flush_send_buffer(&connection);
    CHECK(sends == 1 && waits == 0 && timer_starts == 1);
    CHECK(connection.snd_nxt == 104 && connection.send_buffer_sent == 4);
    resolved = true;
    dmtcp_rto_timer_callback(&connection);
    CHECK(sends == 2 && waits == 0 && connection.send_buffer_sent == 4);
    struct segment_result result = {0};
    dmtcp_header_t header = {.ack_num = 104, .flags = DMTCP_FLAG_ACK, .window = 4096};
    apply_ack(&connection, &header, &result);
    CHECK(connection.send_buffer_len == 0 && connection.snd_una == 104 && timer_stops == 1);
    return 0;
}

static int passive_handshake_survives_arp_miss(void)
{
    reset();
    dmip_addr_t local = connection.local_addr, peer = connection.peer_addr;
    dmtcp_header_t header = {.seq_num = 500, .flags = DMTCP_FLAG_SYN};
    handle_new_syn(NULL, dmip_family_v4, NULL, &local, 21, &peer, 12345, &header);
    CHECK(sends == 1 && waits == 0 && connection.state == dmtcp_state_syn_received);
    resolved = true; dmtcp_rto_timer_callback(&connection);
    CHECK(sends == 2 && waits == 0);
    struct segment_result result = {0};
    header.flags = DMTCP_FLAG_ACK; header.ack_num = connection.snd_nxt; header.window = 4096;
    apply_ack(&connection, &header, &result);
    CHECK(connection.state == dmtcp_state_established && result.completed_passive_handshake);
    return 0;
}

static int fin_and_reset_never_wait_for_arp(void)
{
    reset();
    CHECK(dmtcp_close(&connection) == -EHOSTUNREACH);
    CHECK(waits == 0 && connection.fin_sent && connection.snd_nxt == 101);
    resolved = true; dmtcp_rto_timer_callback(&connection);
    CHECK(sends == 2 && waits == 0);
    resolved = false;
    dmtcp_header_t header = {.seq_num = 1, .flags = DMTCP_FLAG_SYN};
    send_rst_for_unmatched(dmip_family_v4, &connection.local_addr, 21, &connection.peer_addr, 12345, &header, 0);
    CHECK(sends == 3 && waits == 0);
    header.flags = DMTCP_FLAG_ACK; header.ack_num = 1;
    send_rst_for_unmatched(dmip_family_v4, &connection.local_addr, 21, &connection.peer_addr, 12345, &header, 0);
    CHECK(sends == 4 && waits == 0);
    dmtcp_abort(&connection);
    CHECK(sends == 5 && waits == 0);
    return 0;
}

int main(void)
{
    int errors = lost_ack_is_recovered_by_peer_retry() + queued_data_survives_arp_miss()
               + passive_handshake_survives_arp_miss() + fin_and_reset_never_wait_for_arp();
    printf("TCP ARP regression tests: %d failure(s)\n", errors);
    return errors;
}
