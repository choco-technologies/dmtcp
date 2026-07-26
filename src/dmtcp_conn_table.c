/**
 * @file dmtcp_conn_table.c
 * @brief The connection table (4-tuple -> TCB), TCB lifecycle, and the
 *        simple per-connection accessors
 *
 * Two mutexes are at play in this file, deliberately different in scope:
 * g_conn_mutex guards LIST MEMBERSHIP only (who's in the table), never a
 * TCB's own fields - that's conn->lock, taken separately by whichever file
 * is actually processing that connection (dmtcp_input.c, dmtcp_output.c,
 * dmtcp_close.c). See dmtcp_internal.h's struct dmtcp_conn doc comment.
 */
#include "dmod.h"
#include "dmtcp_internal.h"
#include "dmlist.h"
#include <string.h>
#include <errno.h>

static dmlist_context_t* g_conns = NULL;
static dmosi_mutex_t     g_conn_mutex = NULL;
static uint16_t          g_next_ephemeral_connect_port = DMTCP_PORT_EPHEMERAL_FIRST;

/* dmod modules have no libc memcmp() (see dmod/src/module/string.c's
 * minimal replacement set) - a small manual comparison stands in for it,
 * the same pattern tests/dmtcp_test.c uses for itself. */
static bool bytes_equal(const uint8_t* a, const uint8_t* b, size_t len)
{
    for (size_t i = 0; i < len; i++)
    {
        if (a[i] != b[i])
            return false;
    }
    return true;
}

static bool addr_equal(const dmip_addr_t* a, const dmip_addr_t* b)
{
    if (a->family != b->family)
        return false;
    if (a->family == dmip_family_v4)
        return bytes_equal(a->addr.v4, b->addr.v4, DMIP_IPV4_ADDR_LEN);
    if (a->family == dmip_family_v6)
        return bytes_equal(a->addr.v6, b->addr.v6, DMIP_IPV6_ADDR_LEN);
    return true; /* both dmip_family_none */
}

struct dmtcp_conn_key
{
    dmip_addr_t local_addr;
    uint16_t    local_port;
    dmip_addr_t peer_addr;
    uint16_t    peer_port;
};

static int compare_conn_key(const void* data, const void* user_data)
{
    const struct dmtcp_conn* entry = (const struct dmtcp_conn*)data;
    const struct dmtcp_conn_key* key = (const struct dmtcp_conn_key*)user_data;
    bool match = entry->local_port == key->local_port && entry->peer_port == key->peer_port
              && addr_equal(&entry->local_addr, &key->local_addr) && addr_equal(&entry->peer_addr, &key->peer_addr);
    return match ? 0 : -1;
}

struct dmtcp_port_peer_key
{
    uint16_t    local_port;
    dmip_addr_t peer_addr;
    uint16_t    peer_port;
};

static int compare_port_peer(const void* data, const void* user_data)
{
    const struct dmtcp_conn* entry = (const struct dmtcp_conn*)data;
    const struct dmtcp_port_peer_key* key = (const struct dmtcp_port_peer_key*)user_data;
    bool match = entry->local_port == key->local_port && entry->peer_port == key->peer_port
              && addr_equal(&entry->peer_addr, &key->peer_addr);
    return match ? 0 : -1;
}

static int compare_pointer(const void* data, const void* user_data)
{
    return (data == user_data) ? 0 : -1;
}

int dmtcp_conn_table_init(void)
{
    g_conns = dmlist_create(Dmod_GetCurrentAllocatorName());
    g_conn_mutex = dmosi_mutex_create(false);
    return (g_conns != NULL && g_conn_mutex != NULL) ? 0 : -1;
}

void dmtcp_conn_table_deinit(void)
{
    size_t count = dmlist_size(g_conns);
    for (size_t i = 0; i < count; i++)
    {
        dmtcp_conn_table_destroy((struct dmtcp_conn*)dmlist_pop_front(g_conns));
    }
    dmlist_destroy(g_conns);
    g_conns = NULL;

    dmosi_mutex_destroy(g_conn_mutex);
    g_conn_mutex = NULL;
}

struct dmtcp_conn* dmtcp_conn_table_create(void)
{
    struct dmtcp_conn* conn = Dmod_Malloc(sizeof(*conn));
    if (conn == NULL)
        return NULL;

    memset(conn, 0, sizeof(*conn));
    conn->lock = dmosi_mutex_create(false);
    conn->send_buffer = Dmod_Malloc(DMTCP_SEND_BUFFER_LEN);
    conn->rto_timer = dmtcp_output_create_rto_timer(conn);
    conn->time_wait_timer = dmtcp_close_create_time_wait_timer(conn);

    if (conn->lock == NULL || conn->send_buffer == NULL || conn->rto_timer == NULL || conn->time_wait_timer == NULL)
    {
        dmtcp_conn_table_destroy(conn); /* magic is still 0 here - see destroy()'s own guard */
        return NULL;
    }

    conn->rcv_wnd = DMTCP_DEFAULT_RECV_WINDOW;
    conn->magic = DMTCP_CONN_MAGIC;
    return conn;
}

void dmtcp_conn_table_destroy(struct dmtcp_conn* conn)
{
    if (conn == NULL)
        return;

    /* dmtcp_conn_table_create() calls this on a partially-constructed
     * conn (magic still 0) if a sub-allocation fails - every field below
     * tolerates being NULL, so no separate "was this ever set" tracking
     * is needed. */
    conn->magic = 0;

    if (conn->rto_timer != NULL)
    {
        dmosi_timer_stop(conn->rto_timer);
        dmosi_timer_destroy(conn->rto_timer);
    }
    if (conn->time_wait_timer != NULL)
    {
        dmosi_timer_stop(conn->time_wait_timer);
        dmosi_timer_destroy(conn->time_wait_timer);
    }
    Dmod_Free(conn->send_buffer);
    if (conn->lock != NULL)
    {
        dmosi_mutex_destroy(conn->lock);
    }
    Dmod_Free(conn);
}

struct dmtcp_conn* dmtcp_conn_table_find(const dmip_addr_t* local_addr, uint16_t local_port, const dmip_addr_t* peer_addr, uint16_t peer_port)
{
    struct dmtcp_conn_key key = { .local_addr = *local_addr, .local_port = local_port, .peer_addr = *peer_addr, .peer_port = peer_port };

    dmosi_mutex_lock(g_conn_mutex);
    struct dmtcp_conn* conn = (struct dmtcp_conn*)dmlist_find(g_conns, &key, compare_conn_key);
    dmosi_mutex_unlock(g_conn_mutex);
    return conn;
}

int dmtcp_conn_table_insert(struct dmtcp_conn* conn)
{
    struct dmtcp_conn_key key = { .local_addr = conn->local_addr, .local_port = conn->local_port, .peer_addr = conn->peer_addr, .peer_port = conn->peer_port };

    dmosi_mutex_lock(g_conn_mutex);
    int result;
    if (dmlist_find(g_conns, &key, compare_conn_key) != NULL)
    {
        result = -EEXIST;
    }
    else
    {
        result = dmlist_push_back(g_conns, conn) ? 0 : -ENOMEM;
    }
    dmosi_mutex_unlock(g_conn_mutex);
    return result;
}

void dmtcp_conn_table_remove(struct dmtcp_conn* conn)
{
    dmosi_mutex_lock(g_conn_mutex);
    dmlist_remove(g_conns, conn, compare_pointer);
    dmosi_mutex_unlock(g_conn_mutex);
}

bool dmtcp_conn_table_port_in_use_for_peer(uint16_t local_port, const dmip_addr_t* peer_addr, uint16_t peer_port)
{
    struct dmtcp_port_peer_key key = { .local_port = local_port, .peer_addr = *peer_addr, .peer_port = peer_port };

    dmosi_mutex_lock(g_conn_mutex);
    bool in_use = dmlist_find(g_conns, &key, compare_port_peer) != NULL;
    dmosi_mutex_unlock(g_conn_mutex);
    return in_use;
}

int dmtcp_conn_table_alloc_ephemeral_port(const dmip_addr_t* peer_addr, uint16_t peer_port, uint16_t* out_port)
{
    uint32_t range = (uint32_t)DMTCP_PORT_EPHEMERAL_LAST - (uint32_t)DMTCP_PORT_EPHEMERAL_FIRST + 1u;

    dmosi_mutex_lock(g_conn_mutex);
    int result = -EADDRNOTAVAIL;
    for (uint32_t i = 0; i < range; i++)
    {
        uint16_t candidate = g_next_ephemeral_connect_port;
        g_next_ephemeral_connect_port = (candidate == DMTCP_PORT_EPHEMERAL_LAST) ? DMTCP_PORT_EPHEMERAL_FIRST : (uint16_t)(candidate + 1u);

        struct dmtcp_port_peer_key key = { .local_port = candidate, .peer_addr = *peer_addr, .peer_port = peer_port };
        if (dmlist_find(g_conns, &key, compare_port_peer) != NULL)
            continue;

        *out_port = candidate;
        result = 0;
        break;
    }
    dmosi_mutex_unlock(g_conn_mutex);
    return result;
}

/**
 * @brief Free every TCB resource, except that the timer named by `context`
 *        (if any) is only stopped, never destroyed - see
 *        dmtcp_teardown_context_t for why
 */
static void destroy_with_context(struct dmtcp_conn* conn, dmtcp_teardown_context_t context)
{
    conn->magic = 0;

    dmosi_timer_stop(conn->rto_timer);
    if (context != dmtcp_teardown_context_rto_timer)
    {
        dmosi_timer_destroy(conn->rto_timer);
    }

    dmosi_timer_stop(conn->time_wait_timer);
    if (context != dmtcp_teardown_context_time_wait_timer)
    {
        dmosi_timer_destroy(conn->time_wait_timer);
    }

    Dmod_Free(conn->send_buffer);
    if (conn->lock != NULL)
    {
        dmosi_mutex_destroy(conn->lock);
    }
    Dmod_Free(conn);
}

void dmtcp_conn_table_terminate(struct dmtcp_conn* conn, dmtcp_terminal_reason_t reason, int error, dmtcp_teardown_context_t context)
{
    if (conn == NULL || conn->magic != DMTCP_CONN_MAGIC)
        return;

    dmosi_mutex_lock(conn->lock);
    dmtcp_conn_callbacks_t callbacks = conn->callbacks;
    void* user_data = conn->user_data;
    conn->state = dmtcp_state_closed;
    dmosi_mutex_unlock(conn->lock);

    dmtcp_conn_table_remove(conn);

    if (reason == dmtcp_terminal_closed && callbacks.on_closed != NULL)
    {
        callbacks.on_closed(conn, user_data);
    }
    else if (reason == dmtcp_terminal_reset && callbacks.on_reset != NULL)
    {
        callbacks.on_reset(conn, user_data);
    }
    else if (reason == dmtcp_terminal_error && callbacks.on_error != NULL)
    {
        callbacks.on_error(conn, error, user_data);
    }

    destroy_with_context(conn, context);
}

/* ============================================================================
 *                      Simple per-connection accessors
 * ========================================================================== */

dmod_dmtcp_api_declaration(1.0, int, _conn_set_callbacks, ( dmtcp_conn_t conn, const dmtcp_conn_callbacks_t* callbacks, void* user_data ))
{
    if (conn == NULL || conn->magic != DMTCP_CONN_MAGIC)
        return -EINVAL;

    dmosi_mutex_lock(conn->lock);
    conn->callbacks = (callbacks != NULL) ? *callbacks : (dmtcp_conn_callbacks_t){ 0 };
    conn->user_data = user_data;
    dmosi_mutex_unlock(conn->lock);
    return 0;
}

dmod_dmtcp_api_declaration(1.0, void*, _conn_get_user_data, ( dmtcp_conn_t conn ))
{
    if (conn == NULL || conn->magic != DMTCP_CONN_MAGIC)
        return NULL;

    dmosi_mutex_lock(conn->lock);
    void* user_data = conn->user_data;
    dmosi_mutex_unlock(conn->lock);
    return user_data;
}

dmod_dmtcp_api_declaration(1.0, dmtcp_state_t, _conn_get_state, ( dmtcp_conn_t conn ))
{
    if (conn == NULL || conn->magic != DMTCP_CONN_MAGIC)
        return dmtcp_state_closed;

    dmosi_mutex_lock(conn->lock);
    dmtcp_state_t state = conn->state;
    dmosi_mutex_unlock(conn->lock);
    return state;
}

dmod_dmtcp_api_declaration(1.0, int, _conn_get_local_endpoint, ( dmtcp_conn_t conn, dmip_addr_t* out_addr, uint16_t* out_port ))
{
    if (conn == NULL || conn->magic != DMTCP_CONN_MAGIC || out_addr == NULL || out_port == NULL)
        return -EINVAL;

    dmosi_mutex_lock(conn->lock);
    *out_addr = conn->local_addr;
    *out_port = conn->local_port;
    dmosi_mutex_unlock(conn->lock);
    return 0;
}

dmod_dmtcp_api_declaration(1.0, int, _conn_get_peer_endpoint, ( dmtcp_conn_t conn, dmip_addr_t* out_addr, uint16_t* out_port ))
{
    if (conn == NULL || conn->magic != DMTCP_CONN_MAGIC || out_addr == NULL || out_port == NULL)
        return -EINVAL;

    dmosi_mutex_lock(conn->lock);
    *out_addr = conn->peer_addr;
    *out_port = conn->peer_port;
    dmosi_mutex_unlock(conn->lock);
    return 0;
}
