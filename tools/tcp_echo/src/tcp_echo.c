/**
 * @file tcp_echo.c
 * @brief tcp_echo - listen on a TCP port and echo every byte back to whoever connects
 *
 * A thin CLI over dmtcp's listen/accept API (dmtcp_listen(), see
 * include/dmtcp.h): dmtcp_listen()'s accept handler hands over an
 * already-ESTABLISHED dmtcp_conn_t, on which dmtcp_conn_set_callbacks()
 * hooks a data handler that echoes straight back with dmtcp_send() - same
 * inline-delivery, reply-from-the-callback shape as dmudp/tools/udp_echo,
 * except TCP is connection-oriented so there is an accept step in between.
 *
 * A NULL data buffer in the data handler is the peer's FIN (EOF), not a
 * terminal event - dmtcp.h documents that the connection can still be
 * written to until dmtcp_close() is called, so it is just logged here
 * rather than treated as a close.
 */
#include "dmod.h"
#include "dmtcp.h"
#include "dmosi.h"
#include <string.h>

#define TCP_ECHO_DEFAULT_DURATION_S 30u

static uint16_t g_listen_port;
static uint32_t g_accepted_count;
static uint32_t g_echoed_bytes;

/* Parses a plain decimal uint32_t (dmod's minimal module runtime has no
 * strtol()/atoi() - see dmod/src/module/string.c's replacement set). */
static bool parse_uint32(const char* s, uint32_t* out)
{
    if (s == NULL || *s == '\0')
        return false;

    uint64_t value = 0;
    for (const char* p = s; *p != '\0'; p++)
    {
        if (*p < '0' || *p > '9')
            return false;

        value = value * 10 + (uint64_t)(*p - '0');
        if (value > UINT32_MAX)
            return false;
    }

    *out = (uint32_t)value;
    return true;
}

/* Runs inline on whatever thread is pumping the rx interface, the same
 * delivery context dmtcp_datagram_handler_t-style callbacks always use
 * (see dmtcp.h) - safe to call dmtcp_send() straight back from here. */
static void on_data(dmtcp_conn_t conn, const uint8_t* data, size_t data_len, void* user_data)
{
    (void)user_data;

    if (data == NULL)
    {
        Dmod_Printf("tcp_echo: peer sent FIN\n");
        return;
    }

    int sent = dmtcp_send(conn, data, data_len);
    if (sent < 0)
    {
        Dmod_Printf("tcp_echo: send failed (error %d)\n", sent);
        return;
    }

    g_echoed_bytes += (uint32_t)sent;
    Dmod_Printf("tcp_echo: echoed %d of %u bytes\n", sent, (unsigned)data_len);
}

static void on_closed(dmtcp_conn_t conn, void* user_data)
{
    (void)conn;
    (void)user_data;
    Dmod_Printf("tcp_echo: connection closed\n");
}

static void on_reset(dmtcp_conn_t conn, void* user_data)
{
    (void)conn;
    (void)user_data;
    Dmod_Printf("tcp_echo: connection reset\n");
}

/* Called once per completed handshake, with an already-ESTABLISHED conn -
 * hooking on_data here is what turns it into an echo. */
static void on_accept(dmtcp_conn_t conn, const dmip_addr_t* peer, uint16_t peer_port, dmnetif_iface_t iface)
{
    (void)iface;

    g_accepted_count++;

    if (peer->family == dmip_family_v4)
    {
        Dmod_Printf("tcp_echo: accepted connection from %u.%u.%u.%u:%u\n",
                    peer->addr.v4[0], peer->addr.v4[1], peer->addr.v4[2], peer->addr.v4[3],
                    (unsigned)peer_port);
    }
    else
    {
        Dmod_Printf("tcp_echo: accepted connection from port %u\n", (unsigned)peer_port);
    }

    dmtcp_conn_callbacks_t callbacks = {
        .on_data   = on_data,
        .on_closed = on_closed,
        .on_reset  = on_reset,
    };
    dmtcp_conn_set_callbacks(conn, &callbacks, NULL);
}

static void print_usage(const char* prog)
{
    Dmod_Printf("Usage:\n");
    Dmod_Printf("  %s <port>          Listen on <port> and echo every byte back for %u s\n", prog, (unsigned)TCP_ECHO_DEFAULT_DURATION_S);
    Dmod_Printf("  %s --help | -h     Show this help\n", prog);
    Dmod_Printf("\n");
    Dmod_Printf("Options:\n");
    Dmod_Printf("  -t <seconds>       How long to listen before exiting (default: %u)\n", (unsigned)TCP_ECHO_DEFAULT_DURATION_S);
}

int main(int argc, char* argv[])
{
    const char* prog = (argc > 0 && argv[0] != NULL) ? argv[0] : "tcp_echo";

    if (argc >= 2 && (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0))
    {
        print_usage(prog);
        return 0;
    }

    if (argc < 2)
    {
        print_usage(prog);
        return 1;
    }

    uint32_t port_num;
    if (!parse_uint32(argv[1], &port_num) || port_num == 0 || port_num > 65535)
    {
        Dmod_Printf("%s: invalid port '%s'\n", prog, argv[1]);
        return 1;
    }

    uint32_t duration_s = TCP_ECHO_DEFAULT_DURATION_S;
    for (int i = 2; i < argc; i++)
    {
        if (strcmp(argv[i], "-t") == 0 && i + 1 < argc)
        {
            if (!parse_uint32(argv[++i], &duration_s) || duration_s == 0)
            {
                Dmod_Printf("%s: invalid duration '%s'\n", prog, argv[i]);
                return 1;
            }
        }
        else
        {
            print_usage(prog);
            return 1;
        }
    }

    g_listen_port = (uint16_t)port_num;
    g_accepted_count = 0;
    g_echoed_bytes = 0;

    int listen_ret = dmtcp_listen(g_listen_port, on_accept);
    if (listen_ret != 0)
    {
        Dmod_Printf("%s: failed to listen on TCP port %u (error %d)\n", prog, (unsigned)g_listen_port, listen_ret);
        return 1;
    }

    Dmod_Printf("tcp_echo: listening on TCP port %u for %u s\n", (unsigned)g_listen_port, (unsigned)duration_s);

    dmosi_thread_sleep(duration_s * 1000u);

    dmtcp_unlisten(g_listen_port);

    Dmod_Printf("tcp_echo: done, accepted %u connection(s), echoed %u byte(s)\n", (unsigned)g_accepted_count, (unsigned)g_echoed_bytes);
    return 0;
}
