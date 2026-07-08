// Copyright (c) 2020, Ryo Currency Project
// Renamed: tokenhash → tokenhash for TKNC
// TKNC Cryptonight hash implementation

#pragma once

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <cassert>
#include <stddef.h>

#ifdef _WIN32
#include <intrin.h>
#include <malloc.h>
#define HAS_WIN_INTRIN_API
#endif

#if defined(_M_X64) || defined(__x86_64__)
#define HAS_INTEL_HW
#endif

template <size_t MEMORY, size_t ITER, size_t VERSION>
class cn_slow_hash;

using cn_pow_hash_v3 = cn_slow_hash<2 * 1024 * 1024, 0xC000, 2>;

#ifndef HAS_INTEL_HW
inline bool hw_check_aes() { return false; }
inline bool check_avx2() { return false; }
#else
inline bool hw_check_aes() {
    int32_t cpu_info[4];
#if defined(HAS_WIN_INTRIN_API)
    __cpuid(cpu_info, 1);
#else
    __cpuid(1, cpu_info[0], cpu_info[1], cpu_info[2], cpu_info[3]);
#endif
    return (cpu_info[2] & (1 << 25)) != 0;
}
inline bool check_avx2() {
    int32_t cpu_info[4];
#if defined(HAS_WIN_INTRIN_API)
    __cpuidex(cpu_info, 7, 0);
#else
    __cpuid_count(7, 0, cpu_info[0], cpu_info[1], cpu_info[2], cpu_info[3]);
#endif
    return (cpu_info[1] & (1 << 5)) != 0;
}
#endif

class cn_sptr {
public:
    cn_sptr() : base_ptr(nullptr) {}
    cn_sptr(uint64_t* ptr) { base_ptr = ptr; }
    cn_sptr(uint32_t* ptr) { base_ptr = ptr; }
    cn_sptr(uint8_t* ptr) { base_ptr = ptr; }

    inline void set(void* ptr) { base_ptr = ptr; }
    inline cn_sptr offset(size_t i) { return reinterpret_cast<uint8_t*>(base_ptr) + i; }
    inline const cn_sptr offset(size_t i) const { return reinterpret_cast<const uint8_t*>(base_ptr) + i; }

    inline void* as_void() { return base_ptr; }
    inline uint8_t& as_byte(size_t i) { return *(reinterpret_cast<uint8_t*>(base_ptr) + i); }
    inline uint8_t* as_byte() { return reinterpret_cast<uint8_t*>(base_ptr); }
    inline uint64_t& as_uqword(size_t i) { return *(reinterpret_cast<uint64_t*>(base_ptr) + i); }
    inline const uint64_t& as_uqword(size_t i) const { return *(reinterpret_cast<const uint64_t*>(base_ptr) + i); }
    inline uint64_t* as_uqword() { return reinterpret_cast<uint64_t*>(base_ptr); }
    inline const uint64_t* as_uqword() const { return reinterpret_cast<const uint64_t*>(base_ptr); }

private:
    void* base_ptr;
};

template <size_t MEMORY, size_t ITER, size_t VERSION>
class cn_slow_hash {
public:
    cn_slow_hash() : borrowed_pad(false) {
        lpad.set(_aligned_malloc(MEMORY, 4096));
        spad.set(_aligned_malloc(4096, 4096));
    }

    ~cn_slow_hash() { free_mem(); }

    cn_slow_hash(const cn_slow_hash&) = delete;
    cn_slow_hash& operator=(const cn_slow_hash&) = delete;

    void hash(const void* in, size_t len, void* out) {
        if (VERSION <= 1) {
            if (hw_check_aes())
                hardware_hash(in, len, out);
            else
                software_hash(in, len, out);
        } else {
            if (hw_check_aes())
                hardware_hash_3(in, len, out);
            else
                software_hash_3(in, len, out);
        }
    }

    void software_hash(const void* in, size_t len, void* out);
    void software_hash_3(const void* in, size_t len, void* pout);

#ifndef HAS_INTEL_HW
    inline void hardware_hash(const void*, size_t, void*) { assert(false); }
    inline void hardware_hash_3(const void*, size_t, void*) { assert(false); }
#else
    void hardware_hash(const void* in, size_t len, void* out);
    void hardware_hash_3(const void* in, size_t len, void* pout);
#endif

private:
    static constexpr size_t MASK = ((MEMORY - 1) >> 6) << 6;

    inline bool check_override() {
        const char* env = getenv("RYO_USE_SOFTWARE_AES");
        if (!env) return false;
        return strcmp(env, "0") != 0 && strcmp(env, "no") != 0;
    }

    inline void free_mem() {
        if (!borrowed_pad) {
            if (lpad.as_void()) _aligned_free(lpad.as_void());
            if (spad.as_void()) _aligned_free(spad.as_void());
        }
        lpad.set(nullptr);
        spad.set(nullptr);
    }

    inline cn_sptr scratchpad_ptr(uint32_t idx) { return lpad.as_byte() + (idx & MASK); }

    void explode_scratchpad_soft();
    void implode_scratchpad_soft();

    cn_sptr lpad;
    cn_sptr spad;
    bool borrowed_pad;
};
