/**
 * @file dmtcp_listen_table.c
 * @brief Passive-open listener table - dmtcp_listen()/_listen_any()/_unlisten()
 *
 * Same shape as dmudp's port-binding table (dmlist of { port, handler }
 * entries guarded by one dmosi_mutex_t) - just registering a
 * dmtcp_accept_handler_t instead of a dmudp_datagram_handler_t, and the
 * entries here are consulted only for a fresh SYN (see dmtcp_input.c),
 * never for an already-established connection - those live in the
 * connection table (dmtcp_conn_table.c) instead.
 */
#include "dmod.h"
#include "dmtcp_internal.h"
#include "dmlist.h"
#include <errno.h>

struct dmtcp_listener
{
    uint16_t               port;
    dmtcp_accept_handler_t handler;
};

static dmlist_context_t* g_listeners = NULL;
static dmosi_mutex_t     g_listen_mutex = NULL;
static uint16_t          g_next_ephemeral_listen_port = DMTCP_PORT_EPHEMERAL_FIRST;

static int compare_listen_port(const void* data, const void* user_data)
{
    const struct dmtcp_listener* entry = (const struct dmtcp_listener*)data;
    uint16_t port = *(const uint16_t*)user_data;
    return (entry->port == port) ? 0 : -1;
}

static int compare_pointer(const void* data, const void* user_data)
{
    return (data == user_data) ? 0 : -1;
}

int dmtcp_listen_table_init(void)
{
    g_listeners = dmlist_create(Dmod_GetCurrentAllocatorName());
    g_listen_mutex = dmosi_mutex_create(false);
    return (g_listeners != NULL && g_listen_mutex != NULL) ? 0 : -1;
}

void dmtcp_listen_table_deinit(void)
{
    size_t count = dmlist_size(g_listeners);
    for (size_t i = 0; i < count; i++)
    {
        Dmod_Free(dmlist_pop_front(g_listeners));
    }
    dmlist_destroy(g_listeners);
    g_listeners = NULL;

    dmosi_mutex_destroy(g_listen_mutex);
    g_listen_mutex = NULL;
}

dmtcp_accept_handler_t dmtcp_listen_table_find(uint16_t port)
{
    dmosi_mutex_lock(g_listen_mutex);
    struct dmtcp_listener* entry = (struct dmtcp_listener*)dmlist_find(g_listeners, &port, compare_listen_port);
    dmtcp_accept_handler_t handler = (entry != NULL) ? entry->handler : NULL;
    dmosi_mutex_unlock(g_listen_mutex);
    return handler;
}

dmod_dmtcp_api_declaration(1.0, int, _listen, ( uint16_t port, dmtcp_accept_handler_t handler ))
{
    if (handler == NULL || port == 0)
        return -EINVAL;

    dmosi_mutex_lock(g_listen_mutex);

    int result;
    if (dmlist_find(g_listeners, &port, compare_listen_port) != NULL)
    {
        result = -EEXIST;
    }
    else
    {
        struct dmtcp_listener* entry = Dmod_Malloc(sizeof(*entry));
        if (entry == NULL)
        {
            result = -ENOMEM;
        }
        else
        {
            entry->port = port;
            entry->handler = handler;
            if (dmlist_push_back(g_listeners, entry))
            {
                result = 0;
            }
            else
            {
                Dmod_Free(entry);
                result = -ENOMEM;
            }
        }
    }

    dmosi_mutex_unlock(g_listen_mutex);
    return result;
}

dmod_dmtcp_api_declaration(1.0, int, _listen_any, ( dmtcp_accept_handler_t handler, uint16_t* out_port ))
{
    if (handler == NULL || out_port == NULL)
        return -EINVAL;

    dmosi_mutex_lock(g_listen_mutex);

    int result = -EADDRNOTAVAIL;
    uint32_t range = (uint32_t)DMTCP_PORT_EPHEMERAL_LAST - (uint32_t)DMTCP_PORT_EPHEMERAL_FIRST + 1u;
    for (uint32_t i = 0; i < range; i++)
    {
        uint16_t candidate = g_next_ephemeral_listen_port;
        g_next_ephemeral_listen_port = (candidate == DMTCP_PORT_EPHEMERAL_LAST) ? DMTCP_PORT_EPHEMERAL_FIRST : (uint16_t)(candidate + 1u);

        if (dmlist_find(g_listeners, &candidate, compare_listen_port) != NULL)
            continue;

        struct dmtcp_listener* entry = Dmod_Malloc(sizeof(*entry));
        if (entry == NULL)
        {
            result = -ENOMEM;
            break;
        }

        entry->port = candidate;
        entry->handler = handler;
        if (!dmlist_push_back(g_listeners, entry))
        {
            Dmod_Free(entry);
            result = -ENOMEM;
            break;
        }

        *out_port = candidate;
        result = 0;
        break;
    }

    dmosi_mutex_unlock(g_listen_mutex);
    return result;
}

dmod_dmtcp_api_declaration(1.0, void, _unlisten, ( uint16_t port ))
{
    dmosi_mutex_lock(g_listen_mutex);

    struct dmtcp_listener* entry = (struct dmtcp_listener*)dmlist_find(g_listeners, &port, compare_listen_port);
    if (entry != NULL)
    {
        dmlist_remove(g_listeners, entry, compare_pointer);
        Dmod_Free(entry);
    }

    dmosi_mutex_unlock(g_listen_mutex);
}
