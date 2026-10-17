#pragma once
/* Pairing-code PAKE, board side (SPAKE2-P256-SHA256-HKDF-HMAC, RFC 9382 role B).
 * Reference implementation and protocol notes: host/clipboard_agent/pake.py. */
#include <stddef.h>
#include <stdint.h>

typedef int (*pair_rng_fn)(void *ctx, unsigned char *out, size_t len);

typedef struct {
    uint8_t pb[65];     /* B's public share, sent to A */
    uint8_t ca[32];     /* expected confirmation from A */
    uint8_t cb[32];     /* our confirmation, sent only after A's checks out */
    uint8_t ke[16];
    char token[65];     /* derived per-peer credential (hex), same form as USB pairing */
} pair_result_t;

/* w = SHA-512("deskclip-v4/pair-code\0" || code) mod n; code is 6 ASCII digits. */
int pair_code_scalar(const char *code, uint8_t w[32]);
/* Core SPAKE2 for side B. y may be NULL (random from rng). Returns 0 or -1
 * (invalid pA, identity result, crypto failure). */
int pair_spake2_b(const uint8_t w[32], const uint8_t *a, size_t a_len, const uint8_t *b, size_t b_len,
                  const uint8_t pa[65], const uint8_t *y, pair_rng_fn rng, void *rng_ctx, pair_result_t *out);
/* Constant-time compare of 32 bytes. */
int pair_equal32(const uint8_t *x, const uint8_t *y);
