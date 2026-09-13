/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../lib/mirror_packet.h"

int main(void) {
    const unsigned char valid[] = {0, 0, 0, 2, 0x65, 0xaa, 0, 0, 0, 1, 0x61};
    assert(mirror_packet_nal_size(valid, sizeof(valid), 0) == 2);
    assert(mirror_packet_nal_size(valid, sizeof(valid), 6) == 1);
    assert(mirror_packet_nal_size(valid, sizeof(valid), -1) == -1);
    assert(mirror_packet_nal_size(valid, sizeof(valid), sizeof(valid) + 1) == -1);
    assert(mirror_packet_nal_size(NULL, 8, 0) == -1);
    for (int size = 1; size <= (int)sizeof(valid); size++) {
        unsigned char *bounded = malloc(size);
        assert(bounded);
        memcpy(bounded, valid, size);
        int pos = 0;
        while (pos < size) {
            int length = mirror_packet_nal_size(bounded, size, pos);
            if (length < 0) break;
            pos += length + 4;
        }
        if (size == 6 || size == 11) assert(pos == size);
        else assert(pos < size);
        free(bounded);
    }
    unsigned char empty[] = {0, 0, 0, 0};
    unsigned char oversized[] = {0xff, 0xff, 0xff, 0xff, 0x65};
    assert(mirror_packet_nal_size(empty, sizeof(empty), 0) == -1);
    assert(mirror_packet_nal_size(oversized, sizeof(oversized), 0) == -1);
    const unsigned char config[] = {0, 2, 0x67, 0x68};
    assert(mirror_packet_config_size(config, sizeof(config), 0) == 2);
    assert(mirror_packet_config_size(config, sizeof(config) - 1, 0) == -1);
    assert(mirror_packet_config_size(config, 1, 0) == -1);
    assert(mirror_packet_config_size(config, sizeof(config), 4) == -1);
    assert(mirror_packet_config_size(empty, sizeof(empty), 0) == -1);
    unsigned char *large = calloc(32770, 1);
    assert(large);
    large[0] = 0x80;
    assert(mirror_packet_config_size(large, 32770, 0) == 32768);
    free(large);
    puts("mirror packet bounds regression passed");
    return 0;
}
