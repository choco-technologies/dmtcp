/**
 * @file dmtcp_dif.c
 * @brief Implements dmip's protocol handler DIF (dmip_protocol_receive()/
 *        _protocol_numbers(), claiming DMIP_PROTO_TCP - see dmip.h's
 *        "Protocol handler DIF" section)
 *
 * Kept in its own translation unit, separate from dmtcp_input.c (which
 * owns the actual demux/state-machine logic in dmtcp_handle_ip_packet()),
 * and including only dmip.h - not dmtcp.h/dmtcp_internal.h: setting
 * DMOD_ENABLE_REGISTRATION here to get a real (not extern) registration
 * for the two DIF functions below would, if this file also pulled in
 * dmtcp.h, re-emit dmtcp's own Built-in API registration structs a second
 * time - dmtcp_registrations.c already emits those once for the whole
 * module (see its own doc comment) and a second copy is a duplicate-
 * definition link error. Forwarding to dmtcp_handle_ip_packet() (a plain
 * function declared in dmtcp_internal.h, defined in dmtcp_input.c) avoids
 * needing dmtcp.h here at all.
 */
#define DMOD_ENABLE_REGISTRATION ON
#include "dmod.h"
#include "dmip.h"

/**
 * @brief Forward declaration mirroring dmtcp_internal.h's - kept local so
 *        this file doesn't need dmtcp_internal.h (see file doc comment)
 */
void dmtcp_handle_ip_packet(dmip_family_t family, dmnetif_iface_t iface, const uint8_t* packet, size_t packet_len);

/**
 * @brief Implementation of dmip's dmip_protocol_receive DIF - forwards
 *        straight to dmtcp_handle_ip_packet() (see dmtcp_input.c)
 */
dmod_dmip_dif_api_declaration(1.0, dmtcp, void, _protocol_receive, ( dmip_family_t family, dmnetif_iface_t iface, const uint8_t* packet, size_t packet_len ))
{
    dmtcp_handle_ip_packet(family, iface, packet, packet_len);
}

/**
 * @brief Implementation of dmip's dmip_protocol_numbers DIF - dmtcp
 *        claims DMIP_PROTO_TCP unconditionally
 */
dmod_dmip_dif_api_declaration(1.0, dmtcp, size_t, _protocol_numbers, ( uint16_t* out_protocols, size_t max_protocols ))
{
    if (max_protocols == 0)
        return 0;

    out_protocols[0] = DMIP_PROTO_TCP;
    return 1;
}
