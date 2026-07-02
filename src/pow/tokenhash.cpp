// Copyright (c) 2020, Ryo Currency Project
// Renamed: tokenhash → tokenhash for TKNC
// TKNC Cryptonight hash implementation
//
// Adapted for TKNC blockchain

#include "tokenhash_impl.hpp"

#define HASH_DATA_AREA 136

static const uint64_t keccakf_rndc[24] = {
    0x0000000000000001ULL, 0x0000000000008082ULL, 0x800000000000808aULL,
    0x8000000080008000ULL, 0x000000000000808bULL, 0x0000000080000001ULL,
    0x8000000080008081ULL, 0x8000000000008009ULL, 0x000000000000008aULL,
    0x0000000000000088ULL, 0x0000000080008009ULL, 0x000000008000000aULL,
    0x000000008000808bULL, 0x800000000000008bULL, 0x8000000000008089ULL,
    0x8000000000008003ULL, 0x8000000000008002ULL, 0x8000000000000080ULL,
    0x000000000000800aULL, 0x800000008000000aULL, 0x8000000080008081ULL,
    0x8000000000008080ULL, 0x0000000080000001ULL, 0x8000000080008008ULL
};

#ifndef ROTL64
#define ROTL64(x, y) (((x) << (y)) | ((x) >> (64 - (y))))
#endif

static void keccakf(uint64_t st[25]) {
    for (size_t round = 0; round < 24; round++) {
        uint64_t bc0, bc1, bc2, bc3, bc4, t0, t1;

        bc0 = st[0] ^ st[5] ^ st[10] ^ st[15] ^ st[20];
        bc1 = st[1] ^ st[6] ^ st[11] ^ st[16] ^ st[21];
        bc2 = st[2] ^ st[7] ^ st[12] ^ st[17] ^ st[22];
        bc3 = st[3] ^ st[8] ^ st[13] ^ st[18] ^ st[23];
        bc4 = st[4] ^ st[9] ^ st[14] ^ st[19] ^ st[24];

        t0 = bc0; t1 = bc1;
        bc0 ^= ROTL64(bc2, 1); bc1 ^= ROTL64(bc3, 1);
        bc2 ^= ROTL64(bc4, 1); bc3 ^= ROTL64(t0, 1); bc4 ^= ROTL64(t1, 1);

        t0 = st[1] ^ bc0; st[0] ^= bc4;
        st[1] = ROTL64(st[6] ^ bc0, 44);
        st[6] = ROTL64(st[9] ^ bc3, 20);
        st[9] = ROTL64(st[22] ^ bc1, 61);
        st[22] = ROTL64(st[14] ^ bc3, 39);
        st[14] = ROTL64(st[20] ^ bc4, 18);
        st[20] = ROTL64(st[2] ^ bc1, 62);
        st[2] = ROTL64(st[12] ^ bc1, 43);
        st[12] = ROTL64(st[13] ^ bc2, 25);
        st[13] = ROTL64(st[19] ^ bc3, 8);
        st[19] = ROTL64(st[23] ^ bc2, 56);
        st[23] = ROTL64(st[15] ^ bc4, 41);
        st[15] = ROTL64(st[4] ^ bc3, 27);
        st[4] = ROTL64(st[24] ^ bc3, 14);
        st[24] = ROTL64(st[21] ^ bc0, 2);
        st[21] = ROTL64(st[8] ^ bc2, 55);
        st[8] = ROTL64(st[16] ^ bc0, 45);
        st[16] = ROTL64(st[5] ^ bc4, 36);
        st[5] = ROTL64(st[3] ^ bc2, 28);
        st[3] = ROTL64(st[18] ^ bc2, 21);
        st[18] = ROTL64(st[17] ^ bc1, 15);
        st[17] = ROTL64(st[11] ^ bc0, 10);
        st[11] = ROTL64(st[7] ^ bc1, 6);
        st[7] = ROTL64(st[10] ^ bc4, 3);
        st[10] = ROTL64(t0, 1);

        bc0 = st[0]; bc1 = st[1];
        st[0] ^= (~st[1]) & st[2]; st[1] ^= (~st[2]) & st[3];
        st[2] ^= (~st[3]) & st[4]; st[3] ^= (~st[4]) & bc0;
        st[4] ^= (~bc0) & bc1;

        bc0 = st[5]; bc1 = st[6];
        st[5] ^= (~st[6]) & st[7]; st[6] ^= (~st[7]) & st[8];
        st[7] ^= (~st[8]) & st[9]; st[8] ^= (~st[9]) & bc0;
        st[9] ^= (~bc0) & bc1;

        bc0 = st[10]; bc1 = st[11];
        st[10] ^= (~st[11]) & st[12]; st[11] ^= (~st[12]) & st[13];
        st[12] ^= (~st[13]) & st[14]; st[13] ^= (~st[14]) & bc0;
        st[14] ^= (~bc0) & bc1;

        bc0 = st[15]; bc1 = st[16];
        st[15] ^= (~st[16]) & st[17]; st[16] ^= (~st[17]) & st[18];
        st[17] ^= (~st[18]) & st[19]; st[18] ^= (~st[19]) & bc0;
        st[19] ^= (~bc0) & bc1;

        bc0 = st[20]; bc1 = st[21];
        uint64_t bc2_t = st[22], bc3_t = st[23], bc4_t = st[24];
        st[20] ^= (~bc1) & bc2_t; st[21] ^= (~bc2_t) & bc3_t;
        st[22] ^= (~bc3_t) & bc4_t; st[23] ^= (~bc4_t) & bc0;
        st[24] ^= (~bc0) & bc1;

        st[0] ^= keccakf_rndc[round];
    }
}

static void cn_keccak(const uint8_t *in, size_t inlen, uint8_t *md, int mdlen) {
    uint64_t st[25];
    uint8_t temp[144];
    size_t rsiz = mdlen == 64 ? HASH_DATA_AREA : 200 - 2 * mdlen;
    size_t rsizw = rsiz / 8;

    memset(st, 0, sizeof(st));
    for (; inlen >= rsiz; inlen -= rsiz, in += rsiz) {
        for (size_t i = 0; i < rsizw; i++)
            st[i] ^= ((const uint64_t*)in)[i];
        keccakf(st);
    }

    memcpy(temp, in, inlen);
    temp[inlen++] = 0x01;
    memset(temp + inlen, 0, rsiz - inlen);
    temp[rsiz - 1] |= 0x80;

    for (size_t i = 0; i < rsizw; i++)
        st[i] ^= ((uint64_t*)temp)[i];
    keccakf(st);
    memcpy(md, st, mdlen);
}

struct alignas(16) aesdata {
    uint64_t v64x0;
    uint64_t v64x1;

    inline void load(const cn_sptr mem) {
        v64x0 = mem.as_uqword(0);
        v64x1 = mem.as_uqword(1);
    }
    inline void xor_load(const cn_sptr mem) {
        v64x0 ^= mem.as_uqword(0);
        v64x1 ^= mem.as_uqword(1);
    }
    inline void write(cn_sptr mem) {
        mem.as_uqword(0) = v64x0;
        mem.as_uqword(1) = v64x1;
    }
    inline aesdata& operator^=(const aesdata& r) noexcept {
        v64x0 ^= r.v64x0; v64x1 ^= r.v64x1; return *this;
    }
    inline void get_quad(uint32_t& x0, uint32_t& x1, uint32_t& x2, uint32_t& x3) {
        x0 = (uint32_t)v64x0; x1 = (uint32_t)(v64x0 >> 32);
        x2 = (uint32_t)v64x1; x3 = (uint32_t)(v64x1 >> 32);
    }
    inline void set_quad(uint32_t x0, uint32_t x1, uint32_t x2, uint32_t x3) {
        v64x0 = (uint64_t)x0 | ((uint64_t)x1 << 32);
        v64x1 = (uint64_t)x2 | ((uint64_t)x3 << 32);
    }
};

#define SAES_WPOLY 0x011b
#define saes_b2w(b0, b1, b2, b3) (((uint32_t)(b3) << 24) | ((uint32_t)(b2) << 16) | ((uint32_t)(b1) << 8) | (b0))
#define saes_f2(x) ((x << 1) ^ (((x >> 7) & 1) * SAES_WPOLY))
#define saes_f3(x) (saes_f2(x) ^ x)
#define saes_h0(x) (x)
#define saes_u0(p) saes_b2w(saes_f2(p), p, p, saes_f3(p))
#define saes_u1(p) saes_b2w(saes_f3(p), saes_f2(p), p, p)
#define saes_u2(p) saes_b2w(p, saes_f3(p), saes_f2(p), p)
#define saes_u3(p) saes_b2w(p, p, saes_f3(p), saes_f2(p))

#define saes_data(w) { \
    w(0x63), w(0x7c), w(0x77), w(0x7b), w(0xf2), w(0x6b), w(0x6f), w(0xc5), \
    w(0x30), w(0x01), w(0x67), w(0x2b), w(0xfe), w(0xd7), w(0xab), w(0x76), \
    w(0xca), w(0x82), w(0xc9), w(0x7d), w(0xfa), w(0x59), w(0x47), w(0xf0), \
    w(0xad), w(0xd4), w(0xa2), w(0xaf), w(0x9c), w(0xa4), w(0x72), w(0xc0), \
    w(0xb7), w(0xfd), w(0x93), w(0x26), w(0x36), w(0x3f), w(0xf7), w(0xcc), \
    w(0x34), w(0xa5), w(0xe5), w(0xf1), w(0x71), w(0xd8), w(0x31), w(0x15), \
    w(0x04), w(0xc7), w(0x23), w(0xc3), w(0x18), w(0x96), w(0x05), w(0x9a), \
    w(0x07), w(0x12), w(0x80), w(0xe2), w(0xeb), w(0x27), w(0xb2), w(0x75), \
    w(0x09), w(0x83), w(0x2c), w(0x1a), w(0x1b), w(0x6e), w(0x5a), w(0xa0), \
    w(0x52), w(0x3b), w(0xd6), w(0xb3), w(0x29), w(0xe3), w(0x2f), w(0x84), \
    w(0x53), w(0xd1), w(0x00), w(0xed), w(0x20), w(0xfc), w(0xb1), w(0x5b), \
    w(0x6a), w(0xcb), w(0xbe), w(0x39), w(0x4a), w(0x4c), w(0x58), w(0xcf), \
    w(0xd0), w(0xef), w(0xaa), w(0xfb), w(0x43), w(0x4d), w(0x33), w(0x85), \
    w(0x45), w(0xf9), w(0x02), w(0x7f), w(0x50), w(0x3c), w(0x9f), w(0xa8), \
    w(0x51), w(0xa3), w(0x40), w(0x8f), w(0x92), w(0x9d), w(0x38), w(0xf5), \
    w(0xbc), w(0xb6), w(0xda), w(0x21), w(0x10), w(0xff), w(0xf3), w(0xd2), \
    w(0xcd), w(0x0c), w(0x13), w(0xec), w(0x5f), w(0x97), w(0x44), w(0x17), \
    w(0xc4), w(0xa7), w(0x7e), w(0x3d), w(0x64), w(0x5d), w(0x19), w(0x73), \
    w(0x60), w(0x81), w(0x4f), w(0xdc), w(0x22), w(0x2a), w(0x90), w(0x88), \
    w(0x46), w(0xee), w(0xb8), w(0x14), w(0xde), w(0x5e), w(0x0b), w(0xdb), \
    w(0xe0), w(0x32), w(0x3a), w(0x0a), w(0x49), w(0x06), w(0x24), w(0x5c), \
    w(0xc2), w(0xd3), w(0xac), w(0x62), w(0x91), w(0x95), w(0xe4), w(0x79), \
    w(0xe7), w(0xc8), w(0x37), w(0x6d), w(0x8d), w(0xd5), w(0x4e), w(0xa9), \
    w(0x6c), w(0x56), w(0xf4), w(0xea), w(0x65), w(0x7a), w(0xae), w(0x08), \
    w(0xba), w(0x78), w(0x25), w(0x2e), w(0x1c), w(0xa6), w(0xb4), w(0xc6), \
    w(0xe8), w(0xdd), w(0x74), w(0x1f), w(0x4b), w(0xbd), w(0x8b), w(0x8a), \
    w(0x70), w(0x3e), w(0xb5), w(0x66), w(0x48), w(0x03), w(0xf6), w(0x0e), \
    w(0x61), w(0x35), w(0x57), w(0xb9), w(0x86), w(0xc1), w(0x1d), w(0x9e), \
    w(0xe1), w(0xf8), w(0x98), w(0x11), w(0x69), w(0xd9), w(0x8e), w(0x94), \
    w(0x9b), w(0x1e), w(0x87), w(0xe9), w(0xce), w(0x55), w(0x28), w(0xdf), \
    w(0x8c), w(0xa1), w(0x89), w(0x0d), w(0xbf), w(0xe6), w(0x42), w(0x68), \
    w(0x41), w(0x99), w(0x2d), w(0x0f), w(0xb0), w(0x54), w(0xbb), w(0x16) \
}

alignas(16) static const uint32_t saes_table[4][256] = {saes_data(saes_u0), saes_data(saes_u1), saes_data(saes_u2), saes_data(saes_u3)};
alignas(16) extern const uint8_t saes_sbox[256] = saes_data(saes_h0);

inline uint32_t sub_word(uint32_t key) {
    return (saes_sbox[key >> 24] << 24) | (saes_sbox[(key >> 16) & 0xff] << 16) |
           (saes_sbox[(key >> 8) & 0xff] << 8) | saes_sbox[key & 0xff];
}

#if defined(_MSC_VER)
inline uint32_t rotr(uint32_t value, uint32_t amount) {
    return _rotr(value, amount);
}
#else
inline uint32_t rotr(uint32_t value, uint32_t amount) {
    return (value >> amount) | (value << ((32 - amount) & 31));
}
#endif

inline void sl_xor(aesdata& x) {
    uint32_t x0, x1, x2, x3;
    x.get_quad(x0, x1, x2, x3);
    x1 ^= x0; x2 ^= x1; x3 ^= x2;
    x.set_quad(x0, x1, x2, x3);
}

template <uint8_t rcon>
inline void soft_aes_genkey_sub(aesdata& xout0, aesdata& xout2) {
    sl_xor(xout0);
    xout0 ^= rotr(sub_word((uint32_t)xout2.v64x1 >> 32), 8) ^ rcon;
    sl_xor(xout2);
    xout2 ^= sub_word((uint32_t)xout0.v64x1 >> 32);
}

inline void aes_genkey(const cn_sptr memory, aesdata& k0, aesdata& k1, aesdata& k2, aesdata& k3,
                       aesdata& k4, aesdata& k5, aesdata& k6, aesdata& k7, aesdata& k8, aesdata& k9) {
    aesdata xout0, xout2;
    xout0.load(memory);
    xout2.load(memory.offset(16));
    k0 = xout0; k1 = xout2;
    soft_aes_genkey_sub<0x01>(xout0, xout2);
    k2 = xout0; k3 = xout2;
    soft_aes_genkey_sub<0x02>(xout0, xout2);
    k4 = xout0; k5 = xout2;
    soft_aes_genkey_sub<0x04>(xout0, xout2);
    k6 = xout0; k7 = xout2;
    soft_aes_genkey_sub<0x08>(xout0, xout2);
    k8 = xout0; k9 = xout2;
}

inline void aes_round(aesdata& val, const aesdata& key) {
    uint32_t x0, x1, x2, x3;
    val.get_quad(x0, x1, x2, x3);
    val.set_quad(
        saes_table[0][x0 & 0xff] ^ saes_table[1][(x1 >> 8) & 0xff] ^ saes_table[2][(x2 >> 16) & 0xff] ^ saes_table[3][x3 >> 24],
        saes_table[0][x1 & 0xff] ^ saes_table[1][(x2 >> 8) & 0xff] ^ saes_table[2][(x3 >> 16) & 0xff] ^ saes_table[3][x0 >> 24],
        saes_table[0][x2 & 0xff] ^ saes_table[1][(x3 >> 8) & 0xff] ^ saes_table[2][(x0 >> 16) & 0xff] ^ saes_table[3][x1 >> 24],
        saes_table[0][x3 & 0xff] ^ saes_table[1][(x0 >> 8) & 0xff] ^ saes_table[2][(x1 >> 16) & 0xff] ^ saes_table[3][x2 >> 24]
    );
    val ^= key;
}

inline void aes_round8(const aesdata& key, aesdata& x0, aesdata& x1, aesdata& x2, aesdata& x3,
                        aesdata& x4, aesdata& x5, aesdata& x6, aesdata& x7) {
    aes_round(x0, key); aes_round(x1, key); aes_round(x2, key); aes_round(x3, key);
    aes_round(x4, key); aes_round(x5, key); aes_round(x6, key); aes_round(x7, key);
}

inline void xor_shift(aesdata& x0, aesdata& x1, aesdata& x2, aesdata& x3,
                      aesdata& x4, aesdata& x5, aesdata& x6, aesdata& x7) {
    aesdata tmp = x0;
    x0 ^= x1; x1 ^= x2; x2 ^= x3; x3 ^= x4;
    x4 ^= x5; x5 ^= x6; x6 ^= x7; x7 ^= tmp;
}

template <size_t MEMORY, size_t ITER, size_t VERSION>
void cn_slow_hash<MEMORY, ITER, VERSION>::implode_scratchpad_soft() {
    aesdata x0, x1, x2, x3, x4, x5, x6, x7;
    aesdata k0, k1, k2, k3, k4, k5, k6, k7, k8, k9;

    aes_genkey(spad.as_uqword() + 4, k0, k1, k2, k3, k4, k5, k6, k7, k8, k9);

    x0.load(spad.as_uqword() + 8); x1.load(spad.as_uqword() + 10);
    x2.load(spad.as_uqword() + 12); x3.load(spad.as_uqword() + 14);
    x4.load(spad.as_uqword() + 16); x5.load(spad.as_uqword() + 18);
    x6.load(spad.as_uqword() + 20); x7.load(spad.as_uqword() + 22);

    for (size_t i = 0; i < MEMORY / sizeof(uint64_t); i += 16) {
        x0.xor_load(lpad.as_uqword() + i + 0); x1.xor_load(lpad.as_uqword() + i + 2);
        x2.xor_load(lpad.as_uqword() + i + 4); x3.xor_load(lpad.as_uqword() + i + 6);
        x4.xor_load(lpad.as_uqword() + i + 8); x5.xor_load(lpad.as_uqword() + i + 10);
        x6.xor_load(lpad.as_uqword() + i + 12); x7.xor_load(lpad.as_uqword() + i + 14);
        aes_round8(k0, x0, x1, x2, x3, x4, x5, x6, x7);
        aes_round8(k1, x0, x1, x2, x3, x4, x5, x6, x7);
        aes_round8(k2, x0, x1, x2, x3, x4, x5, x6, x7);
        aes_round8(k3, x0, x1, x2, x3, x4, x5, x6, x7);
        aes_round8(k4, x0, x1, x2, x3, x4, x5, x6, x7);
        aes_round8(k5, x0, x1, x2, x3, x4, x5, x6, x7);
        aes_round8(k6, x0, x1, x2, x3, x4, x5, x6, x7);
        aes_round8(k7, x0, x1, x2, x3, x4, x5, x6, x7);
        aes_round8(k8, x0, x1, x2, x3, x4, x5, x6, x7);
        aes_round8(k9, x0, x1, x2, x3, x4, x5, x6, x7);
        if (VERSION > 0) xor_shift(x0, x1, x2, x3, x4, x5, x6, x7);
    }

    for (size_t i = 0; VERSION > 0 && i < MEMORY / sizeof(uint64_t); i += 16) {
        x0.xor_load(lpad.as_uqword() + i + 0); x1.xor_load(lpad.as_uqword() + i + 2);
        x2.xor_load(lpad.as_uqword() + i + 4); x3.xor_load(lpad.as_uqword() + i + 6);
        x4.xor_load(lpad.as_uqword() + i + 8); x5.xor_load(lpad.as_uqword() + i + 10);
        x6.xor_load(lpad.as_uqword() + i + 12); x7.xor_load(lpad.as_uqword() + i + 14);
        aes_round8(k0, x0, x1, x2, x3, x4, x5, x6, x7);
        aes_round8(k1, x0, x1, x2, x3, x4, x5, x6, x7);
        aes_round8(k2, x0, x1, x2, x3, x4, x5, x6, x7);
        aes_round8(k3, x0, x1, x2, x3, x4, x5, x6, x7);
        aes_round8(k4, x0, x1, x2, x3, x4, x5, x6, x7);
        aes_round8(k5, x0, x1, x2, x3, x4, x5, x6, x7);
        aes_round8(k6, x0, x1, x2, x3, x4, x5, x6, x7);
        aes_round8(k7, x0, x1, x2, x3, x4, x5, x6, x7);
        aes_round8(k8, x0, x1, x2, x3, x4, x5, x6, x7);
        aes_round8(k9, x0, x1, x2, x3, x4, x5, x6, x7);
        xor_shift(x0, x1, x2, x3, x4, x5, x6, x7);
    }

    for (size_t i = 0; VERSION > 0 && i < 16; i++) {
        aes_round8(k0, x0, x1, x2, x3, x4, x5, x6, x7);
        aes_round8(k1, x0, x1, x2, x3, x4, x5, x6, x7);
        aes_round8(k2, x0, x1, x2, x3, x4, x5, x6, x7);
        aes_round8(k3, x0, x1, x2, x3, x4, x5, x6, x7);
        aes_round8(k4, x0, x1, x2, x3, x4, x5, x6, x7);
        aes_round8(k5, x0, x1, x2, x3, x4, x5, x6, x7);
        aes_round8(k6, x0, x1, x2, x3, x4, x5, x6, x7);
        aes_round8(k7, x0, x1, x2, x3, x4, x5, x6, x7);
        aes_round8(k8, x0, x1, x2, x3, x4, x5, x6, x7);
        aes_round8(k9, x0, x1, x2, x3, x4, x5, x6, x7);
        xor_shift(x0, x1, x2, x3, x4, x5, x6, x7);
    }

    x0.write(spad.as_uqword() + 8); x1.write(spad.as_uqword() + 10);
    x2.write(spad.as_uqword() + 12); x3.write(spad.as_uqword() + 14);
    x4.write(spad.as_uqword() + 16); x5.write(spad.as_uqword() + 18);
    x6.write(spad.as_uqword() + 20); x7.write(spad.as_uqword() + 22);
}

template <size_t MEMORY, size_t ITER, size_t VERSION>
void cn_slow_hash<MEMORY, ITER, VERSION>::explode_scratchpad_soft() {
    aesdata x0, x1, x2, x3, x4, x5, x6, x7;
    aesdata k0, k1, k2, k3, k4, k5, k6, k7, k8, k9;

    aes_genkey(spad.as_uqword(), k0, k1, k2, k3, k4, k5, k6, k7, k8, k9);

    x0.load(spad.as_uqword() + 8); x1.load(spad.as_uqword() + 10);
    x2.load(spad.as_uqword() + 12); x3.load(spad.as_uqword() + 14);
    x4.load(spad.as_uqword() + 16); x5.load(spad.as_uqword() + 18);
    x6.load(spad.as_uqword() + 20); x7.load(spad.as_uqword() + 22);

    for (size_t i = 0; VERSION > 0 && i < 16; i++) {
        aes_round8(k0, x0, x1, x2, x3, x4, x5, x6, x7);
        aes_round8(k1, x0, x1, x2, x3, x4, x5, x6, x7);
        aes_round8(k2, x0, x1, x2, x3, x4, x5, x6, x7);
        aes_round8(k3, x0, x1, x2, x3, x4, x5, x6, x7);
        aes_round8(k4, x0, x1, x2, x3, x4, x5, x6, x7);
        aes_round8(k5, x0, x1, x2, x3, x4, x5, x6, x7);
        aes_round8(k6, x0, x1, x2, x3, x4, x5, x6, x7);
        aes_round8(k7, x0, x1, x2, x3, x4, x5, x6, x7);
        aes_round8(k8, x0, x1, x2, x3, x4, x5, x6, x7);
        aes_round8(k9, x0, x1, x2, x3, x4, x5, x6, x7);
        xor_shift(x0, x1, x2, x3, x4, x5, x6, x7);
    }

    for (size_t i = 0; i < MEMORY / sizeof(uint64_t); i += 16) {
        aes_round8(k0, x0, x1, x2, x3, x4, x5, x6, x7);
        aes_round8(k1, x0, x1, x2, x3, x4, x5, x6, x7);
        aes_round8(k2, x0, x1, x2, x3, x4, x5, x6, x7);
        aes_round8(k3, x0, x1, x2, x3, x4, x5, x6, x7);
        aes_round8(k4, x0, x1, x2, x3, x4, x5, x6, x7);
        aes_round8(k5, x0, x1, x2, x3, x4, x5, x6, x7);
        aes_round8(k6, x0, x1, x2, x3, x4, x5, x6, x7);
        aes_round8(k7, x0, x1, x2, x3, x4, x5, x6, x7);
        aes_round8(k8, x0, x1, x2, x3, x4, x5, x6, x7);
        aes_round8(k9, x0, x1, x2, x3, x4, x5, x6, x7);
        x0.write(lpad.as_uqword() + i + 0); x1.write(lpad.as_uqword() + i + 2);
        x2.write(lpad.as_uqword() + i + 4); x3.write(lpad.as_uqword() + i + 6);
        x4.write(lpad.as_uqword() + i + 8); x5.write(lpad.as_uqword() + i + 10);
        x6.write(lpad.as_uqword() + i + 12); x7.write(lpad.as_uqword() + i + 14);
    }
}

template <size_t MEMORY, size_t ITER, size_t VERSION>
void cn_slow_hash<MEMORY, ITER, VERSION>::software_hash_3(const void* in, size_t len, void* pout) {
    memset(lpad.as_void(), 0, MEMORY);
    memset(spad.as_void(), 0, 4096);

    cn_keccak((const uint8_t*)in, len, spad.as_byte(), 200);

    memcpy(lpad.as_byte(), spad.as_byte(), 40);
    memcpy(&lpad.as_byte()[40], spad.as_byte(), 40);

    explode_scratchpad_soft();
    implode_scratchpad_soft();

    memcpy(&lpad.as_byte()[8], spad.as_byte() + 8, 168);
    cn_keccak(lpad.as_byte(), 168, (uint8_t*)pout, 32);
}
