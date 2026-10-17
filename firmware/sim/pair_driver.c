/* Host driver for main/pair_crypto.c: reads hex args, prints pB cA cB token.
 * usage: pair_driver <w-hex|code:NNNNNN> <A> <B> <pA-hex> <y-hex> */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pair_crypto.h"

static int rng(void *ctx, unsigned char *out, size_t len)
{
    (void)ctx;
    for (size_t i = 0; i < len; i++) out[i] = (unsigned char)arc4random();
    return 0;
}
static void unhex(const char *s, uint8_t *out, size_t n)
{
    for (size_t i = 0; i < n; i++) sscanf(s + 2 * i, "%2hhx", &out[i]);
}
static void hex(const uint8_t *b, size_t n)
{
    for (size_t i = 0; i < n; i++) printf("%02x", b[i]);
    printf("\n");
}
int main(int argc, char **argv)
{
    if (argc != 6) return 2;
    uint8_t w[32], pa[65], y[32];
    if (!strncmp(argv[1], "code:", 5)) {
        if (pair_code_scalar(argv[1] + 5, w)) { printf("bad code\n"); return 1; }
    } else unhex(argv[1], w, 32);
    unhex(argv[4], pa, 65);
    unhex(argv[5], y, 32);
    pair_result_t r;
    if (pair_spake2_b(w, (uint8_t *)argv[2], strlen(argv[2]), (uint8_t *)argv[3], strlen(argv[3]), pa,
                      strlen(argv[5]) ? y : NULL, rng, NULL, &r)) { printf("error\n"); return 1; }
    hex(w, 32);
    hex(r.pb, 65);
    hex(r.ca, 32);
    hex(r.cb, 32);
    hex(r.ke, 16);
    printf("%s\n", r.token);
    return 0;
}
