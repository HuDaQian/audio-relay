#include "crypto.h"
#include <cstring>
#include <algorithm>

namespace audio_relay {

// --- SHA-256 Implementation ---
namespace {

inline uint32_t rotr32(uint32_t x, uint32_t n) {
    return (x >> n) | (x << (32 - n));
}

inline uint32_t ch(uint32_t x, uint32_t y, uint32_t z) {
    return (x & y) ^ (~x & z);
}

inline uint32_t maj(uint32_t x, uint32_t y, uint32_t z) {
    return (x & y) ^ (x & z) ^ (y & z);
}

inline uint32_t sig0(uint32_t x) {
    return rotr32(x, 2) ^ rotr32(x, 13) ^ rotr32(x, 22);
}

inline uint32_t sig1(uint32_t x) {
    return rotr32(x, 6) ^ rotr32(x, 11) ^ rotr32(x, 25);
}

inline uint32_t gam0(uint32_t x) {
    return rotr32(x, 7) ^ rotr32(x, 18) ^ (x >> 3);
}

inline uint32_t gam1(uint32_t x) {
    return rotr32(x, 17) ^ rotr32(x, 19) ^ (x >> 10);
}

static const uint32_t K256[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5,
    0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
    0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc,
    0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7,
    0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
    0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3,
    0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5,
    0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
};

struct Sha256Ctx {
    uint32_t state[8];
    uint64_t count;
    uint8_t buffer[64];
};

void sha256_init(Sha256Ctx* ctx) {
    ctx->state[0] = 0x6a09e667;
    ctx->state[1] = 0xbb67ae85;
    ctx->state[2] = 0x3c6ef372;
    ctx->state[3] = 0xa54ff53a;
    ctx->state[4] = 0x510e527f;
    ctx->state[5] = 0x9b05688c;
    ctx->state[6] = 0x1f83d9ab;
    ctx->state[7] = 0x5be0cd19;
    ctx->count = 0;
}

void sha256_transform(uint32_t state[8], const uint8_t block[64]) {
    uint32_t w[64];
    for (int i = 0; i < 16; i++) {
        w[i] = ((uint32_t)block[i * 4] << 24) |
               ((uint32_t)block[i * 4 + 1] << 16) |
               ((uint32_t)block[i * 4 + 2] << 8) |
               ((uint32_t)block[i * 4 + 3]);
    }
    for (int i = 16; i < 64; i++) {
        w[i] = gam1(w[i - 2]) + w[i - 7] + gam0(w[i - 15]) + w[i - 16];
    }
    uint32_t a = state[0], b = state[1], c = state[2], d = state[3];
    uint32_t e = state[4], f = state[5], g = state[6], h = state[7];

    for (int i = 0; i < 64; i++) {
        uint32_t t1 = h + sig1(e) + ch(e, f, g) + K256[i] + w[i];
        uint32_t t2 = sig0(a) + maj(a, b, c);
        h = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }
    state[0] += a; state[1] += b; state[2] += c; state[3] += d;
    state[4] += e; state[5] += f; state[6] += g; state[7] += h;
}

void sha256_update(Sha256Ctx* ctx, const uint8_t* data, size_t len) {
    size_t buffer_fill = (size_t)(ctx->count % 64);
    ctx->count += len;
    size_t i = 0;
    if (buffer_fill > 0) {
        size_t to_copy = std::min(len, 64 - buffer_fill);
        std::memcpy(ctx->buffer + buffer_fill, data, to_copy);
        i += to_copy;
        if (buffer_fill + to_copy == 64) {
            sha256_transform(ctx->state, ctx->buffer);
        }
    }
    for (; i + 64 <= len; i += 64) {
        sha256_transform(ctx->state, data + i);
    }
    if (i < len) {
        std::memcpy(ctx->buffer, data + i, len - i);
    }
}

void sha256_final(Sha256Ctx* ctx, uint8_t hash[32]) {
    uint8_t pad = 0x80;
    sha256_update(ctx, &pad, 1);
    uint8_t zero = 0;
    while ((ctx->count % 64) != 56) {
        sha256_update(ctx, &zero, 1);
    }
    uint64_t bits = (ctx->count - 1 - (ctx->count % 64 < 56 ? 56 - (ctx->count % 64) : 64 + 56 - (ctx->count % 64))); // exact bit length
    // More cleanly:
    uint64_t total_bits = (ctx->count - (ctx->count - (ctx->count - (ctx->count % 64)))) * 8; // we kept exact original byte count before padding
}

} // anon namespace

void sha256(const uint8_t* data, size_t len, uint8_t hash[32]) {
    Sha256Ctx ctx;
    ctx.state[0] = 0x6a09e667; ctx.state[1] = 0xbb67ae85;
    ctx.state[2] = 0x3c6ef372; ctx.state[3] = 0xa54ff53a;
    ctx.state[4] = 0x510e527f; ctx.state[5] = 0x9b05688c;
    ctx.state[6] = 0x1f83d9ab; ctx.state[7] = 0x5be0cd19;
    ctx.count = len;

    size_t i = 0;
    for (; i + 64 <= len; i += 64) {
        sha256_transform(ctx.state, data + i);
    }
    uint8_t block[64];
    size_t rem = len - i;
    std::memcpy(block, data + i, rem);
    block[rem++] = 0x80;
    if (rem > 56) {
        std::memset(block + rem, 0, 64 - rem);
        sha256_transform(ctx.state, block);
        rem = 0;
    }
    std::memset(block + rem, 0, 56 - rem);
    uint64_t bits = (uint64_t)len * 8;
    for (int b = 7; b >= 0; b--) {
        block[56 + (7 - b)] = (uint8_t)(bits >> (b * 8));
    }
    sha256_transform(ctx.state, block);

    for (int j = 0; j < 8; j++) {
        hash[j * 4]     = (uint8_t)(ctx.state[j] >> 24);
        hash[j * 4 + 1] = (uint8_t)(ctx.state[j] >> 16);
        hash[j * 4 + 2] = (uint8_t)(ctx.state[j] >> 8);
        hash[j * 4 + 3] = (uint8_t)(ctx.state[j]);
    }
}

void hmac_sha256(const uint8_t* key, size_t key_len,
                 const uint8_t* data, size_t data_len,
                 uint8_t out[32]) {
    uint8_t k[64] = {0};
    if (key_len > 64) {
        sha256(key, key_len, k);
    } else {
        std::memcpy(k, key, key_len);
    }

    uint8_t k_ipad[64];
    uint8_t k_opad[64];
    for (int i = 0; i < 64; i++) {
        k_ipad[i] = k[i] ^ 0x36;
        k_opad[i] = k[i] ^ 0x5c;
    }

    std::vector<uint8_t> inner(64 + data_len);
    std::memcpy(inner.data(), k_ipad, 64);
    if (data_len > 0) {
        std::memcpy(inner.data() + 64, data, data_len);
    }
    uint8_t inner_hash[32];
    sha256(inner.data(), inner.size(), inner_hash);

    uint8_t outer[64 + 32];
    std::memcpy(outer, k_opad, 64);
    std::memcpy(outer + 64, inner_hash, 32);
    sha256(outer, sizeof(outer), out);
}

void hkdf_sha256_32(const uint8_t* salt, size_t salt_len,
                    const uint8_t* ikm, size_t ikm_len,
                    const uint8_t* info, size_t info_len,
                    uint8_t okm[32]) {
    // 1. Extract: PRK = HMAC-Hash(salt, IKM)
    uint8_t prk[32];
    hmac_sha256(salt, salt_len, ikm, ikm_len, prk);

    // 2. Expand: T(1) = HMAC-Hash(PRK, info || 0x01)
    std::vector<uint8_t> expand_buf(info_len + 1);
    if (info_len > 0) {
        std::memcpy(expand_buf.data(), info, info_len);
    }
    expand_buf[info_len] = 0x01;

    hmac_sha256(prk, 32, expand_buf.data(), expand_buf.size(), okm);
}

// --- ChaCha20 & Poly1305 (RFC 8439) ---
namespace {

inline uint32_t rotl32(uint32_t x, int n) {
    return (x << n) | (x >> (32 - n));
}

#define CHACHA_QR(a, b, c, d) \
    a += b; d ^= a; d = rotl32(d, 16); \
    c += d; b ^= c; b = rotl32(b, 12); \
    a += b; d ^= a; d = rotl32(d, 8);  \
    c += d; b ^= c; b = rotl32(b, 7);

void chacha20_block(const uint32_t state[16], uint8_t output[64]) {
    uint32_t x[16];
    std::memcpy(x, state, 64);
    for (int i = 0; i < 10; i++) {
        CHACHA_QR(x[0], x[4], x[8],  x[12]);
        CHACHA_QR(x[1], x[5], x[9],  x[13]);
        CHACHA_QR(x[2], x[6], x[10], x[14]);
        CHACHA_QR(x[3], x[7], x[11], x[15]);
        CHACHA_QR(x[0], x[5], x[10], x[15]);
        CHACHA_QR(x[1], x[6], x[11], x[12]);
        CHACHA_QR(x[2], x[7], x[8],  x[13]);
        CHACHA_QR(x[3], x[4], x[9],  x[14]);
    }
    for (int i = 0; i < 16; i++) {
        uint32_t val = x[i] + state[i];
        output[i * 4]     = (uint8_t)(val);
        output[i * 4 + 1] = (uint8_t)(val >> 8);
        output[i * 4 + 2] = (uint8_t)(val >> 16);
        output[i * 4 + 3] = (uint8_t)(val >> 24);
    }
}

void chacha20_init_state(uint32_t state[16], const uint8_t key[32], const uint8_t nonce[12], uint32_t counter) {
    state[0] = 0x61707865;
    state[1] = 0x3320646e;
    state[2] = 0x79622d32;
    state[3] = 0x6b206574;
    for (int i = 0; i < 8; i++) {
        state[4 + i] = ((uint32_t)key[i * 4]) |
                       (((uint32_t)key[i * 4 + 1]) << 8) |
                       (((uint32_t)key[i * 4 + 2]) << 16) |
                       (((uint32_t)key[i * 4 + 3]) << 24);
    }
    state[12] = counter;
    for (int i = 0; i < 3; i++) {
        state[13 + i] = ((uint32_t)nonce[i * 4]) |
                        (((uint32_t)nonce[i * 4 + 1]) << 8) |
                        (((uint32_t)nonce[i * 4 + 2]) << 16) |
                        (((uint32_t)nonce[i * 4 + 3]) << 24);
    }
}

// Poly1305 implementation
struct Poly1305Ctx {
    uint32_t r[5];
    uint32_t h[5];
    uint32_t pad[4];
};

void poly1305_init(Poly1305Ctx* ctx, const uint8_t key[32]) {
    uint32_t t0 = ((uint32_t)key[0]) | (((uint32_t)key[1]) << 8) | (((uint32_t)key[2]) << 16) | (((uint32_t)key[3]) << 24);
    uint32_t t1 = ((uint32_t)key[4]) | (((uint32_t)key[5]) << 8) | (((uint32_t)key[6]) << 16) | (((uint32_t)key[7]) << 24);
    uint32_t t2 = ((uint32_t)key[8]) | (((uint32_t)key[9]) << 8) | (((uint32_t)key[10]) << 16) | (((uint32_t)key[11]) << 24);
    uint32_t t3 = ((uint32_t)key[12]) | (((uint32_t)key[13]) << 8) | (((uint32_t)key[14]) << 16) | (((uint32_t)key[15]) << 24);

    ctx->r[0] = t0 & 0x3ffffff;
    ctx->r[1] = ((t0 >> 26) | (t1 << 6)) & 0x3ffff03;
    ctx->r[2] = ((t1 >> 20) | (t2 << 12)) & 0x3ffc0ff;
    ctx->r[3] = ((t2 >> 14) | (t3 << 18)) & 0x3f03fff;
    ctx->r[4] = (t3 >> 8) & 0x00fffff;

    ctx->h[0] = ctx->h[1] = ctx->h[2] = ctx->h[3] = ctx->h[4] = 0;

    for (int i = 0; i < 4; i++) {
        ctx->pad[i] = ((uint32_t)key[16 + i * 4]) |
                      (((uint32_t)key[16 + i * 4 + 1]) << 8) |
                      (((uint32_t)key[16 + i * 4 + 2]) << 16) |
                      (((uint32_t)key[16 + i * 4 + 3]) << 24);
    }
}

void poly1305_blocks(Poly1305Ctx* ctx, const uint8_t* m, size_t bytes, bool is_final) {
    uint32_t hibit = is_final ? 0 : (1 << 24);

    while (bytes >= 16) {
        uint32_t t0 = ((uint32_t)m[0]) | (((uint32_t)m[1]) << 8) | (((uint32_t)m[2]) << 16) | (((uint32_t)m[3]) << 24);
        uint32_t t1 = ((uint32_t)m[4]) | (((uint32_t)m[5]) << 8) | (((uint32_t)m[6]) << 16) | (((uint32_t)m[7]) << 24);
        uint32_t t2 = ((uint32_t)m[8]) | (((uint32_t)m[9]) << 8) | (((uint32_t)m[10]) << 16) | (((uint32_t)m[11]) << 24);
        uint32_t t3 = ((uint32_t)m[12]) | (((uint32_t)m[13]) << 8) | (((uint32_t)m[14]) << 16) | (((uint32_t)m[15]) << 24);

        ctx->h[0] += t0 & 0x3ffffff;
        ctx->h[1] += ((t0 >> 26) | (t1 << 6)) & 0x3ffffff;
        ctx->h[2] += ((t1 >> 20) | (t2 << 12)) & 0x3ffffff;
        ctx->h[3] += ((t2 >> 14) | (t3 << 18)) & 0x3ffffff;
        ctx->h[4] += (t3 >> 8) | (1 << 24); // add 2^128

        uint64_t d0 = (uint64_t)ctx->h[0] * ctx->r[0] + (uint64_t)ctx->h[1] * (5 * ctx->r[4]) + (uint64_t)ctx->h[2] * (5 * ctx->r[3]) + (uint64_t)ctx->h[3] * (5 * ctx->r[2]) + (uint64_t)ctx->h[4] * (5 * ctx->r[1]);
        uint64_t d1 = (uint64_t)ctx->h[0] * ctx->r[1] + (uint64_t)ctx->h[1] * ctx->r[0] + (uint64_t)ctx->h[2] * (5 * ctx->r[4]) + (uint64_t)ctx->h[3] * (5 * ctx->r[3]) + (uint64_t)ctx->h[4] * (5 * ctx->r[2]);
        uint64_t d2 = (uint64_t)ctx->h[0] * ctx->r[2] + (uint64_t)ctx->h[1] * ctx->r[1] + (uint64_t)ctx->h[2] * ctx->r[0] + (uint64_t)ctx->h[3] * (5 * ctx->r[4]) + (uint64_t)ctx->h[4] * (5 * ctx->r[3]);
        uint64_t d3 = (uint64_t)ctx->h[0] * ctx->r[3] + (uint64_t)ctx->h[1] * ctx->r[2] + (uint64_t)ctx->h[2] * ctx->r[1] + (uint64_t)ctx->h[3] * ctx->r[0] + (uint64_t)ctx->h[4] * (5 * ctx->r[4]);
        uint64_t d4 = (uint64_t)ctx->h[0] * ctx->r[4] + (uint64_t)ctx->h[1] * ctx->r[3] + (uint64_t)ctx->h[2] * ctx->r[2] + (uint64_t)ctx->h[3] * ctx->r[1] + (uint64_t)ctx->h[4] * ctx->r[0];

        uint32_t c = (uint32_t)(d0 >> 26); ctx->h[0] = (uint32_t)d0 & 0x3ffffff; d1 += c;
        c = (uint32_t)(d1 >> 26); ctx->h[1] = (uint32_t)d1 & 0x3ffffff; d2 += c;
        c = (uint32_t)(d2 >> 26); ctx->h[2] = (uint32_t)d2 & 0x3ffffff; d3 += c;
        c = (uint32_t)(d3 >> 26); ctx->h[3] = (uint32_t)d3 & 0x3ffffff; d4 += c;
        c = (uint32_t)(d4 >> 26); ctx->h[4] = (uint32_t)d4 & 0x3ffffff;
        ctx->h[0] += c * 5;
        c = ctx->h[0] >> 26; ctx->h[0] &= 0x3ffffff; ctx->h[1] += c;

        m += 16;
        bytes -= 16;
    }
    if (bytes > 0) {
        uint8_t buffer[16] = {0};
        std::memcpy(buffer, m, bytes);
        buffer[bytes] = 0x01; // 1 byte padding bit

        uint32_t t0 = ((uint32_t)buffer[0]) | (((uint32_t)buffer[1]) << 8) | (((uint32_t)buffer[2]) << 16) | (((uint32_t)buffer[3]) << 24);
        uint32_t t1 = ((uint32_t)buffer[4]) | (((uint32_t)buffer[5]) << 8) | (((uint32_t)buffer[6]) << 16) | (((uint32_t)buffer[7]) << 24);
        uint32_t t2 = ((uint32_t)buffer[8]) | (((uint32_t)buffer[9]) << 8) | (((uint32_t)buffer[10]) << 16) | (((uint32_t)buffer[11]) << 24);
        uint32_t t3 = ((uint32_t)buffer[12]) | (((uint32_t)buffer[13]) << 8) | (((uint32_t)buffer[14]) << 16) | (((uint32_t)buffer[15]) << 24);

        ctx->h[0] += t0 & 0x3ffffff;
        ctx->h[1] += ((t0 >> 26) | (t1 << 6)) & 0x3ffffff;
        ctx->h[2] += ((t1 >> 20) | (t2 << 12)) & 0x3ffffff;
        ctx->h[3] += ((t2 >> 14) | (t3 << 18)) & 0x3ffffff;
        ctx->h[4] += (t3 >> 8);

        uint64_t d0 = (uint64_t)ctx->h[0] * ctx->r[0] + (uint64_t)ctx->h[1] * (5 * ctx->r[4]) + (uint64_t)ctx->h[2] * (5 * ctx->r[3]) + (uint64_t)ctx->h[3] * (5 * ctx->r[2]) + (uint64_t)ctx->h[4] * (5 * ctx->r[1]);
        uint64_t d1 = (uint64_t)ctx->h[0] * ctx->r[1] + (uint64_t)ctx->h[1] * ctx->r[0] + (uint64_t)ctx->h[2] * (5 * ctx->r[4]) + (uint64_t)ctx->h[3] * (5 * ctx->r[3]) + (uint64_t)ctx->h[4] * (5 * ctx->r[2]);
        uint64_t d2 = (uint64_t)ctx->h[0] * ctx->r[2] + (uint64_t)ctx->h[1] * ctx->r[1] + (uint64_t)ctx->h[2] * ctx->r[0] + (uint64_t)ctx->h[3] * (5 * ctx->r[4]) + (uint64_t)ctx->h[4] * (5 * ctx->r[3]);
        uint64_t d3 = (uint64_t)ctx->h[0] * ctx->r[3] + (uint64_t)ctx->h[1] * ctx->r[2] + (uint64_t)ctx->h[2] * ctx->r[1] + (uint64_t)ctx->h[3] * ctx->r[0] + (uint64_t)ctx->h[4] * (5 * ctx->r[4]);
        uint64_t d4 = (uint64_t)ctx->h[0] * ctx->r[4] + (uint64_t)ctx->h[1] * ctx->r[3] + (uint64_t)ctx->h[2] * ctx->r[2] + (uint64_t)ctx->h[3] * ctx->r[1] + (uint64_t)ctx->h[4] * ctx->r[0];

        uint32_t c = (uint32_t)(d0 >> 26); ctx->h[0] = (uint32_t)d0 & 0x3ffffff; d1 += c;
        c = (uint32_t)(d1 >> 26); ctx->h[1] = (uint32_t)d1 & 0x3ffffff; d2 += c;
        c = (uint32_t)(d2 >> 26); ctx->h[2] = (uint32_t)d2 & 0x3ffffff; d3 += c;
        c = (uint32_t)(d3 >> 26); ctx->h[3] = (uint32_t)d3 & 0x3ffffff; d4 += c;
        c = (uint32_t)(d4 >> 26); ctx->h[4] = (uint32_t)d4 & 0x3ffffff;
        ctx->h[0] += c * 5;
        c = ctx->h[0] >> 26; ctx->h[0] &= 0x3ffffff; ctx->h[1] += c;
    }
}

void poly1305_finish(Poly1305Ctx* ctx, uint8_t mac[16]) {
    uint32_t h0 = ctx->h[0] | (ctx->h[1] << 26);
    uint32_t h1 = (ctx->h[1] >> 6) | (ctx->h[2] << 20);
    uint32_t h2 = (ctx->h[2] >> 12) | (ctx->h[3] << 14);
    uint32_t h3 = (ctx->h[3] >> 18) | (ctx->h[4] << 8);

    uint64_t f0 = (uint64_t)h0 + ctx->pad[0];
    uint64_t f1 = (uint64_t)h1 + ctx->pad[1] + (f0 >> 32);
    uint64_t f2 = (uint64_t)h2 + ctx->pad[2] + (f1 >> 32);
    uint64_t f3 = (uint64_t)h3 + ctx->pad[3] + (f2 >> 32);

    mac[0]  = (uint8_t)f0; mac[1]  = (uint8_t)(f0 >> 8); mac[2]  = (uint8_t)(f0 >> 16); mac[3]  = (uint8_t)(f0 >> 24);
    mac[4]  = (uint8_t)f1; mac[5]  = (uint8_t)(f1 >> 8); mac[6]  = (uint8_t)(f1 >> 16); mac[7]  = (uint8_t)(f1 >> 24);
    mac[8]  = (uint8_t)f2; mac[9]  = (uint8_t)(f2 >> 8); mac[10] = (uint8_t)(f2 >> 16); mac[11] = (uint8_t)(f2 >> 24);
    mac[12] = (uint8_t)f3; mac[13] = (uint8_t)(f3 >> 8); mac[14] = (uint8_t)(f3 >> 16); mac[15] = (uint8_t)(f3 >> 24);
}

} // anon namespace

void chacha20_poly1305_seal(const uint8_t key[32],
                            const uint8_t nonce[12],
                            const uint8_t* aad, size_t aad_len,
                            const uint8_t* plaintext, size_t plaintext_len,
                            uint8_t* out_ciphertext,
                            uint8_t out_tag[16]) {
    // 1. Generate Poly1305 subkey using ChaCha20 with counter = 0
    uint32_t state[16];
    chacha20_init_state(state, key, nonce, 0);
    uint8_t poly_key_block[64];
    chacha20_block(state, poly_key_block);

    // 2. Encrypt plaintext with counter = 1
    uint32_t counter = 1;
    size_t i = 0;
    while (i < plaintext_len) {
        chacha20_init_state(state, key, nonce, counter++);
        uint8_t key_stream[64];
        chacha20_block(state, key_stream);
        size_t block_len = std::min((size_t)64, plaintext_len - i);
        for (size_t b = 0; b < block_len; b++) {
            out_ciphertext[i + b] = plaintext[i + b] ^ key_stream[b];
        }
        i += block_len;
    }

    // 3. Poly1305 MAC over: aad || pad16(aad) || ciphertext || pad16(ciphertext) || len(aad) || len(ciphertext)
    Poly1305Ctx poly;
    poly1305_init(&poly, poly_key_block);

    if (aad_len > 0) {
        poly1305_blocks(&poly, aad, (aad_len / 16) * 16, false);
        size_t rem = aad_len % 16;
        if (rem > 0) {
            uint8_t pad[16] = {0};
            std::memcpy(pad, aad + (aad_len - rem), rem);
            poly1305_blocks(&poly, pad, 16, false);
        }
    }

    if (plaintext_len > 0) {
        poly1305_blocks(&poly, out_ciphertext, (plaintext_len / 16) * 16, false);
        size_t rem = plaintext_len % 16;
        if (rem > 0) {
            uint8_t pad[16] = {0};
            std::memcpy(pad, out_ciphertext + (plaintext_len - rem), rem);
            poly1305_blocks(&poly, pad, 16, false);
        }
    }

    uint8_t lens[16];
    uint64_t aad_len_le = (uint64_t)aad_len;
    uint64_t ct_len_le = (uint64_t)plaintext_len;
    for (int b = 0; b < 8; b++) {
        lens[b] = (uint8_t)(aad_len_le >> (b * 8));
        lens[8 + b] = (uint8_t)(ct_len_le >> (b * 8));
    }
    poly1305_blocks(&poly, lens, 16, false);

    poly1305_finish(&poly, out_tag);
}

bool constant_time_eq(const uint8_t* a, const uint8_t* b, size_t len) {
    uint8_t result = 0;
    for (size_t i = 0; i < len; i++) {
        result |= (a[i] ^ b[i]);
    }
    return result == 0;
}

bool constant_time_eq_str(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) {
        return false;
    }
    return constant_time_eq(reinterpret_cast<const uint8_t*>(a.data()),
                            reinterpret_cast<const uint8_t*>(b.data()),
                            a.size());
}

std::string hmac_sha256_hex(const uint8_t* key, size_t key_len, const std::string& data) {
    uint8_t out[32];
    hmac_sha256(key, key_len, reinterpret_cast<const uint8_t*>(data.data()), data.size(), out);
    static const char hex_chars[] = "0123456789abcdef";
    std::string res;
    res.reserve(64);
    for (int i = 0; i < 32; i++) {
        res.push_back(hex_chars[(out[i] >> 4) & 0x0F]);
        res.push_back(hex_chars[out[i] & 0x0F]);
    }
    return res;
}

} // namespace audio_relay
