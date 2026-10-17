/* SPAKE2 board side with mbedtls. Scalar multiplications use mbedtls_ecp_mul
 * (blinded, constant flow); mbedtls_ecp_muladd is only used to add two points
 * with scalar 1, never with a secret scalar. */
#define MBEDTLS_ALLOW_PRIVATE_ACCESS /* negate a point: Y = P - Y */
#include "pair_crypto.h"

#include <string.h>

#include "mbedtls/bignum.h"
#include "mbedtls/ecp.h"
#include "mbedtls/md.h"

static const uint8_t M_POINT[65] = {
    0x04, 0x88, 0x6e, 0x2f, 0x97, 0xac, 0xe4, 0x6e, 0x55, 0xba, 0x9d, 0xd7, 0x24, 0x25, 0x79, 0xf2, 0x99,
    0x3b, 0x64, 0xe1, 0x6e, 0xf3, 0xdc, 0xab, 0x95, 0xaf, 0xd4, 0x97, 0x33, 0x3d, 0x8f, 0xa1, 0x2f,
    0x5f, 0xf3, 0x55, 0x16, 0x3e, 0x43, 0xce, 0x22, 0x4e, 0x0b, 0x0e, 0x65, 0xff, 0x02, 0xac, 0x8e,
    0x5c, 0x7b, 0xe0, 0x94, 0x19, 0xc7, 0x85, 0xe0, 0xca, 0x54, 0x7d, 0x55, 0xa1, 0x2e, 0x2d, 0x20};
static const uint8_t N_POINT[65] = {
    0x04, 0xd8, 0xbb, 0xd6, 0xc6, 0x39, 0xc6, 0x29, 0x37, 0xb0, 0x4d, 0x99, 0x7f, 0x38, 0xc3, 0x77, 0x07,
    0x19, 0xc6, 0x29, 0xd7, 0x01, 0x4d, 0x49, 0xa2, 0x4b, 0x4f, 0x98, 0xba, 0xa1, 0x29, 0x2b, 0x49,
    0x07, 0xd6, 0x0a, 0xa6, 0xbf, 0xad, 0xe4, 0x50, 0x08, 0xa6, 0x36, 0x33, 0x7f, 0x51, 0x68, 0xc6,
    0x4d, 0x9b, 0xd3, 0x60, 0x34, 0x80, 0x8c, 0xd5, 0x64, 0x49, 0x0b, 0x1e, 0x65, 0x6e, 0xdb, 0xe7};

static int hmac256(const uint8_t *key, size_t key_len, const uint8_t *data, size_t len, uint8_t out[32])
{
    return mbedtls_md_hmac(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), key, key_len, data, len, out);
}

/* HKDF-SHA256 with an empty salt; length <= 32 is all we need. */
static int hkdf32(const uint8_t *ikm, size_t ikm_len, const char *info, uint8_t *out, size_t length)
{
    uint8_t zero[32] = {0}, prk[32], block[32 + 64];
    size_t info_len = strlen(info);
    if (length > 32 || info_len > 63) return -1;
    if (hmac256(zero, sizeof(zero), ikm, ikm_len, prk)) return -1;
    memcpy(block, info, info_len);
    block[info_len] = 1;
    uint8_t t[32];
    int rc = hmac256(prk, 32, block, info_len + 1, t);
    memcpy(out, t, length);
    memset(prk, 0, sizeof(prk));
    memset(t, 0, sizeof(t));
    return rc;
}

int pair_code_scalar(const char *code, uint8_t w[32])
{
    static const char label[] = "deskclip-v4/pair-code";
    uint8_t data[sizeof(label) + 6], digest[64];
    if (strlen(code) != 6) return -1;
    for (int i = 0; i < 6; i++)
        if (code[i] < '0' || code[i] > '9') return -1;
    memcpy(data, label, sizeof(label)); /* includes the NUL separator */
    memcpy(data + sizeof(label), code, 6);
    if (mbedtls_md(mbedtls_md_info_from_type(MBEDTLS_MD_SHA512), data, sizeof(data), digest)) return -1;
    mbedtls_ecp_group grp;
    mbedtls_mpi h, r;
    mbedtls_ecp_group_init(&grp);
    mbedtls_mpi_init(&h);
    mbedtls_mpi_init(&r);
    int rc = mbedtls_ecp_group_load(&grp, MBEDTLS_ECP_DP_SECP256R1);
    if (!rc) rc = mbedtls_mpi_read_binary(&h, digest, sizeof(digest));
    if (!rc) rc = mbedtls_mpi_mod_mpi(&r, &h, &grp.N);
    if (!rc) rc = mbedtls_mpi_write_binary(&r, w, 32);
    mbedtls_mpi_free(&h);
    mbedtls_mpi_free(&r);
    mbedtls_ecp_group_free(&grp);
    memset(digest, 0, sizeof(digest));
    return rc ? -1 : 0;
}

static size_t put(uint8_t *tt, size_t at, const uint8_t *data, size_t len)
{
    uint64_t n = len;
    for (int i = 0; i < 8; i++) tt[at + i] = (uint8_t)(n >> (8 * i));
    if (len) memcpy(tt + at + 8, data, len);
    return at + 8 + len;
}

int pair_equal32(const uint8_t *x, const uint8_t *y)
{
    uint8_t d = 0;
    for (int i = 0; i < 32; i++) d |= x[i] ^ y[i];
    return d == 0;
}

int pair_spake2_b(const uint8_t w_bytes[32], const uint8_t *a, size_t a_len, const uint8_t *b, size_t b_len,
                  const uint8_t pa_bytes[65], const uint8_t *y_bytes, pair_rng_fn rng, void *rng_ctx,
                  pair_result_t *out)
{
    if (a_len > 64 || b_len > 64) return -1;
    mbedtls_ecp_group grp;
    mbedtls_ecp_point M, N, pa, yg, pb, t, u, k;
    mbedtls_mpi w, y, one;
    mbedtls_ecp_group_init(&grp);
    mbedtls_ecp_point_init(&M);
    mbedtls_ecp_point_init(&N);
    mbedtls_ecp_point_init(&pa);
    mbedtls_ecp_point_init(&yg);
    mbedtls_ecp_point_init(&pb);
    mbedtls_ecp_point_init(&u);
    mbedtls_ecp_point_init(&t);
    mbedtls_ecp_point_init(&k);
    mbedtls_mpi_init(&w);
    mbedtls_mpi_init(&y);
    mbedtls_mpi_init(&one);
    uint8_t kb[65], tt[6 * 8 + 64 + 64 + 65 * 3 + 32], digest[32], kc[32];
    size_t olen = 0, n = 0;
    int rc = mbedtls_ecp_group_load(&grp, MBEDTLS_ECP_DP_SECP256R1);
    if (!rc) rc = mbedtls_ecp_point_read_binary(&grp, &M, M_POINT, sizeof(M_POINT));
    if (!rc) rc = mbedtls_ecp_point_read_binary(&grp, &N, N_POINT, sizeof(N_POINT));
    /* Reject anything that is not an uncompressed point on P-256. */
    if (!rc && pa_bytes[0] != 0x04) rc = -1;
    if (!rc) rc = mbedtls_ecp_point_read_binary(&grp, &pa, pa_bytes, 65);
    if (!rc) rc = mbedtls_ecp_check_pubkey(&grp, &pa);
    if (!rc) rc = mbedtls_mpi_read_binary(&w, w_bytes, 32);
    if (!rc) rc = mbedtls_mpi_lset(&one, 1);
    if (!rc) rc = y_bytes ? mbedtls_mpi_read_binary(&y, y_bytes, 32) : mbedtls_ecp_gen_privkey(&grp, &y, rng, rng_ctx);
    if (!rc && (mbedtls_mpi_cmp_int(&y, 1) < 0 || mbedtls_mpi_cmp_mpi(&y, &grp.N) >= 0)) rc = -1;
    /* pB = y*G + w*N */
    if (!rc) rc = mbedtls_ecp_mul(&grp, &yg, &y, &grp.G, rng, rng_ctx);
    if (!rc) rc = mbedtls_ecp_mul(&grp, &t, &w, &N, rng, rng_ctx);
    if (!rc) rc = mbedtls_ecp_muladd(&grp, &pb, &one, &yg, &one, &t);
    if (!rc) rc = mbedtls_ecp_point_write_binary(&grp, &pb, MBEDTLS_ECP_PF_UNCOMPRESSED, &olen, out->pb, 65);
    /* K = y*(pA - w*M) */
    if (!rc) rc = mbedtls_ecp_mul(&grp, &t, &w, &M, rng, rng_ctx);
    if (!rc) rc = mbedtls_mpi_sub_mpi(&t.Y, &grp.P, &t.Y); /* negate */
    if (!rc) rc = mbedtls_ecp_muladd(&grp, &u, &one, &pa, &one, &t);
    if (!rc && mbedtls_ecp_is_zero(&u)) rc = -1;
    if (!rc) rc = mbedtls_ecp_mul(&grp, &k, &y, &u, rng, rng_ctx);
    if (!rc && mbedtls_ecp_is_zero(&k)) rc = -1;
    if (!rc) rc = mbedtls_ecp_point_write_binary(&grp, &k, MBEDTLS_ECP_PF_UNCOMPRESSED, &olen, kb, sizeof(kb));
    if (!rc) {
        n = put(tt, n, a, a_len);
        n = put(tt, n, b, b_len);
        n = put(tt, n, pa_bytes, 65);
        n = put(tt, n, out->pb, 65);
        n = put(tt, n, kb, 65);
        n = put(tt, n, w_bytes, 32);
        rc = mbedtls_md(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), tt, n, digest);
    }
    /* Ke || Ka = Hash(TT); KcA || KcB = HKDF(Ka, "ConfirmationKeys") */
    if (!rc) rc = hkdf32(digest + 16, 16, "ConfirmationKeys", kc, 32);
    if (!rc) rc = hmac256(kc, 16, tt, n, out->ca);
    if (!rc) rc = hmac256(kc + 16, 16, tt, n, out->cb);
    if (!rc) {
        uint8_t cred[32];
        memcpy(out->ke, digest, 16);
        rc = hkdf32(digest, 16, "deskclip-v4/credential", cred, 32);
        for (int i = 0; i < 32; i++) {
            static const char hex[] = "0123456789abcdef";
            out->token[2 * i] = hex[cred[i] >> 4];
            out->token[2 * i + 1] = hex[cred[i] & 15];
        }
        out->token[64] = 0;
        memset(cred, 0, sizeof(cred));
    }
    memset(kb, 0, sizeof(kb));
    memset(tt, 0, sizeof(tt));
    memset(digest, 0, sizeof(digest));
    memset(kc, 0, sizeof(kc));
    mbedtls_mpi_free(&w);
    mbedtls_mpi_free(&y);
    mbedtls_mpi_free(&one);
    mbedtls_ecp_point_free(&M);
    mbedtls_ecp_point_free(&N);
    mbedtls_ecp_point_free(&pa);
    mbedtls_ecp_point_free(&yg);
    mbedtls_ecp_point_free(&pb);
    mbedtls_ecp_point_free(&u);
    mbedtls_ecp_point_free(&t);
    mbedtls_ecp_point_free(&k);
    mbedtls_ecp_group_free(&grp);
    if (rc) memset(out, 0, sizeof(*out));
    return rc ? -1 : 0;
}
