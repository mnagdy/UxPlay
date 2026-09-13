/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../lib/crypto.h"
#include "../lib/mirror_buffer.h"

static const unsigned char audio_key[16] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15};

/* Encrypt as a continuous independent CTR stream; receiver chunk sizes
 * must not change the resulting plaintext or read beyond each packet. */
static void encrypt_stream(uint64_t stream_id, const unsigned char *plain, unsigned char *cipher, int size) {
    unsigned char key[64], iv[64];
    char label[64];
    sha_ctx_t *hash = sha_init();
    snprintf(label, sizeof(label), "AirPlayStreamKey%" PRIu64, stream_id);
    sha_update(hash, (const unsigned char *)label, (int)strlen(label));
    sha_update(hash, audio_key, sizeof(audio_key));
    sha_final(hash, key, NULL);
    sha_reset(hash);
    snprintf(label, sizeof(label), "AirPlayStreamIV%" PRIu64, stream_id);
    sha_update(hash, (const unsigned char *)label, (int)strlen(label));
    sha_update(hash, audio_key, sizeof(audio_key));
    sha_final(hash, iv, NULL);
    sha_destroy(hash);
    aes_ctx_t *aes = aes_ctr_init(key, iv);
    aes_ctr_encrypt(aes, plain, cipher, size);
    aes_ctr_destroy(aes);
}

static void decrypt_piece(mirror_buffer_t *buffer, const unsigned char *cipher,
                          const unsigned char *plain, int size) {
    unsigned char *input = malloc(size), *output = malloc(size);
    assert(input && output);
    memcpy(input, cipher, size);
    mirror_buffer_decrypt(buffer, input, output, size);
    assert(!memcmp(output, plain, size));
    free(input);
    free(output);
}

int main(void) {
    unsigned char plain[257], cipher[257];
    uint64_t stream_id = 0x123456789abcdef0ULL;
    for (int i = 0; i < (int)sizeof(plain); i++) plain[i] = (unsigned char)(i * 37 + 11);
    encrypt_stream(stream_id, plain, cipher, sizeof(cipher));
    for (int first = 1; first <= 32; first++) {
        for (int step = 1; step <= 32; step++) {
            mirror_buffer_t *buffer = mirror_buffer_init(NULL, audio_key);
            assert(buffer);
            mirror_buffer_init_aes(buffer, &stream_id);
            decrypt_piece(buffer, cipher, plain, first);
            mirror_buffer_decrypt(buffer, NULL, NULL, 0);
            for (int pos = first; pos < (int)sizeof(cipher);) {
                int n = (int)sizeof(cipher) - pos;
                if (n > step) n = step;
                decrypt_piece(buffer, cipher + pos, plain + pos, n);
                pos += n;
            }
            /* Rekey after a partial block, on the same receiver object. */
            uint64_t replacement_id = stream_id + 1;
            unsigned char replacement[sizeof(cipher)];
            encrypt_stream(replacement_id, plain, replacement, sizeof(replacement));
            mirror_buffer_init_aes(buffer, &replacement_id);
            decrypt_piece(buffer, replacement, plain, 1);
            decrypt_piece(buffer, replacement + 1, plain + 1, sizeof(replacement) - 1);
            mirror_buffer_destroy(buffer);
        }
    }
    puts("mirror buffer regression passed (1024 chunk patterns and rekeys)");
    return 0;
}
