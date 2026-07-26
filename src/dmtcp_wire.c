/**
 * @file dmtcp_wire.c
 * @brief TCP header build/parse and checksum - see dmtcp.h
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

static uint16_t read_u16_be(const uint8_t* p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | (uint16_t)p[1]);
}

static void write_u32_be(uint8_t* p, uint32_t value)
{
    p[0] = (uint8_t)(value >> 24);
    p[1] = (uint8_t)(value >> 16);
    p[2] = (uint8_t)(value >> 8);
    p[3] = (uint8_t)(value & 0xFFu);
}

static uint32_t read_u32_be(const uint8_t* p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

/* ============================================================================
 *                      Common header
 * ========================================================================== */

dmod_dmtcp_api_declaration(1.0, int, _build_header, ( uint8_t* buffer, size_t buffer_len, const dmtcp_header_t* header ))
{
    if (buffer == NULL || header == NULL || buffer_len < DMTCP_HEADER_LEN)
        return -EINVAL;

    write_u16_be(&buffer[0], header->src_port);
    write_u16_be(&buffer[2], header->dst_port);
    write_u32_be(&buffer[4], header->seq_num);
    write_u32_be(&buffer[8], header->ack_num);
    buffer[12] = 0x50u; /* Data Offset = 5 (no options), low 4 bits reserved/zero */
    buffer[13] = header->flags & 0x3Fu;
    write_u16_be(&buffer[14], header->window);
    write_u16_be(&buffer[16], 0); /* checksum - filled in by the caller once the payload is known */
    write_u16_be(&buffer[18], header->urgent_pointer);
    return 0;
}

dmod_dmtcp_api_declaration(1.0, int, _parse_header, ( const uint8_t* buffer, size_t length, dmtcp_header_t* header, size_t* header_len ))
{
    if (buffer == NULL || header == NULL || header_len == NULL || length < DMTCP_HEADER_LEN)
        return -EINVAL;

    uint8_t data_offset_words = (uint8_t)(buffer[12] >> 4);
    if (data_offset_words < 5u)
        return -EPROTO;

    size_t real_len = (size_t)data_offset_words * 4u;
    if (real_len > length)
        return -EPROTO;

    header->src_port = read_u16_be(&buffer[0]);
    header->dst_port = read_u16_be(&buffer[2]);
    header->seq_num  = read_u32_be(&buffer[4]);
    header->ack_num  = read_u32_be(&buffer[8]);
    header->flags    = buffer[13] & 0x3Fu;
    header->window   = read_u16_be(&buffer[14]);
    header->checksum = read_u16_be(&buffer[16]);
    header->urgent_pointer = read_u16_be(&buffer[18]);
    *header_len = real_len;
    return 0;
}

/* ============================================================================
 *                      Checksum
 * ========================================================================== */

void dmtcp_wire_write_v4_pseudo_header(uint8_t* buf, const dmip_addr_t* src_ip, const dmip_addr_t* dst_ip, uint16_t segment_len)
{
    memcpy(&buf[0], src_ip->addr.v4, DMIP_IPV4_ADDR_LEN);
    memcpy(&buf[4], dst_ip->addr.v4, DMIP_IPV4_ADDR_LEN);
    buf[8] = 0;
    buf[9] = DMIP_PROTO_TCP;
    write_u16_be(&buf[10], segment_len);
}

void dmtcp_wire_write_v6_pseudo_header(uint8_t* buf, const dmip_addr_t* src_ip, const dmip_addr_t* dst_ip, uint32_t segment_len)
{
    memcpy(&buf[0], src_ip->addr.v6, DMIP_IPV6_ADDR_LEN);
    memcpy(&buf[16], dst_ip->addr.v6, DMIP_IPV6_ADDR_LEN);
    write_u32_be(&buf[32], segment_len);
    buf[36] = 0;
    buf[37] = 0;
    buf[38] = 0;
    buf[39] = DMIP_PROTO_TCP;
}

uint16_t dmtcp_wire_v4_checksum(const dmip_addr_t* src_ip, const dmip_addr_t* dst_ip, const uint8_t* segment, size_t length)
{
    size_t total = DMTCP_V4_PSEUDO_HEADER_LEN + length;
    uint8_t* buf = Dmod_Malloc(total);
    if (buf == NULL)
        return 0xFFFFu; /* never a valid "matches" result on its own - caller only cares about == 0 for verification */

    dmtcp_wire_write_v4_pseudo_header(buf, src_ip, dst_ip, (uint16_t)length);
    memcpy(buf + DMTCP_V4_PSEUDO_HEADER_LEN, segment, length);

    uint16_t result = dmip_checksum(buf, total);
    Dmod_Free(buf);
    return result;
}

uint16_t dmtcp_wire_v6_checksum(const dmip_addr_t* src_ip, const dmip_addr_t* dst_ip, const uint8_t* segment, size_t length)
{
    size_t total = DMTCP_V6_PSEUDO_HEADER_LEN + length;
    uint8_t* buf = Dmod_Malloc(total);
    if (buf == NULL)
        return 0xFFFFu;

    dmtcp_wire_write_v6_pseudo_header(buf, src_ip, dst_ip, (uint32_t)length);
    memcpy(buf + DMTCP_V6_PSEUDO_HEADER_LEN, segment, length);

    uint16_t result = dmip_checksum(buf, total);
    Dmod_Free(buf);
    return result;
}

dmod_dmtcp_api_declaration(1.0, bool, _v4_checksum_valid, ( const dmip_addr_t* src_ip, const dmip_addr_t* dst_ip, const uint8_t* segment, size_t length ))
{
    if (src_ip == NULL || dst_ip == NULL || segment == NULL || length < DMTCP_HEADER_LEN)
        return false;
    if (src_ip->family != dmip_family_v4 || dst_ip->family != dmip_family_v4)
        return false;

    return dmtcp_wire_v4_checksum(src_ip, dst_ip, segment, length) == 0;
}

dmod_dmtcp_api_declaration(1.0, bool, _v6_checksum_valid, ( const dmip_addr_t* src_ip, const dmip_addr_t* dst_ip, const uint8_t* segment, size_t length ))
{
    if (src_ip == NULL || dst_ip == NULL || segment == NULL || length < DMTCP_HEADER_LEN)
        return false;
    if (src_ip->family != dmip_family_v6 || dst_ip->family != dmip_family_v6)
        return false;

    return dmtcp_wire_v6_checksum(src_ip, dst_ip, segment, length) == 0;
}
