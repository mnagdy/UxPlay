/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef MIRROR_PACKET_H
#define MIRROR_PACKET_H

#include <stdint.h>

/* Return a bounded, nonempty NAL size, or -1. Check the prefix before
 * reading it; an otherwise valid packet can end in a truncated prefix. */
static inline int mirror_packet_nal_size(const unsigned char *payload, int size, int offset) {
    if (!payload || offset < 0 || offset > size || size - offset < 4) return -1;
    const unsigned char *p = payload + offset;
    uint32_t length = ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
                      ((uint32_t)p[2] << 8) | (uint32_t)p[3];
    if (!length || length > (uint32_t)(size - offset - 4)) return -1;
    return (int)length;
}

/* Codec configuration records use a two-byte length immediately before
 * each parameter set. Return it only when the complete unit is present. */
static inline int mirror_packet_config_size(const unsigned char *payload, int size, int offset) {
    if (!payload || offset < 0 || offset > size || size - offset < 2) return -1;
    int length = ((int)payload[offset] << 8) | payload[offset + 1];
    return length > 0 && length <= size - offset - 2 ? length : -1;
}

#endif
