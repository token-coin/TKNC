// Copyright (c) 2018-present The TKN Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef TKN_SPAN_H
#define TKN_SPAN_H

#include <cassert>
#include <cstddef>
#include <span>
#include <type_traits>
#include <utility>

// A span refers to a contiguous sequence of objects. Pitfalls: (1) lifetime—caller must ensure pointed-to objects outlive the span (vector push_back may invalidate); (2) constructing from temporaries is UB if span outlives the temporary; (3) auto-converts from range-like objects (vectors/arrays) for const element spans, but mutable spans reject temporaries (like non-const lvalue refs).

/** Pop the last element off a span, and return a reference to that element. */
template <typename T>
T& SpanPopBack(std::span<T>& span)
{
    size_t size = span.size();
    T& back = span.back();
    span = span.first(size - 1);
    return back;
}

template <typename V>
auto MakeByteSpan(const V& v) noexcept
{
    return std::as_bytes(std::span{v});
}
template <typename V>
auto MakeWritableByteSpan(V&& v) noexcept
{
    return std::as_writable_bytes(std::span{std::forward<V>(v)});
}

// Helper functions to safely cast basic byte pointers to unsigned char pointers.
inline unsigned char* UCharCast(char* c) { return reinterpret_cast<unsigned char*>(c); }
inline unsigned char* UCharCast(unsigned char* c) { return c; }
inline unsigned char* UCharCast(signed char* c) { return reinterpret_cast<unsigned char*>(c); }
inline unsigned char* UCharCast(std::byte* c) { return reinterpret_cast<unsigned char*>(c); }
inline const unsigned char* UCharCast(const char* c) { return reinterpret_cast<const unsigned char*>(c); }
inline const unsigned char* UCharCast(const unsigned char* c) { return c; }
inline const unsigned char* UCharCast(const signed char* c) { return reinterpret_cast<const unsigned char*>(c); }
inline const unsigned char* UCharCast(const std::byte* c) { return reinterpret_cast<const unsigned char*>(c); }
// Helper concept for the basic byte types.
template <typename B>
concept BasicByte = requires { UCharCast(std::span<B>{}.data()); };

// Helper function to safely convert a span to a span<[const] unsigned char>.
template <typename T, size_t N> constexpr auto UCharSpanCast(std::span<T, N> s) { return std::span<std::remove_pointer_t<decltype(UCharCast(s.data()))>, N>{UCharCast(s.data()), s.size()}; }

/** Like the std::span constructor, but for (const) unsigned char member types only. Only works for (un)signed char containers. */
template <typename V> constexpr auto MakeUCharSpan(const V& v) -> decltype(UCharSpanCast(std::span{v})) { return UCharSpanCast(std::span{v}); }
template <typename V> constexpr auto MakeWritableUCharSpan(V&& v) -> decltype(UCharSpanCast(std::span{std::forward<V>(v)})) { return UCharSpanCast(std::span{std::forward<V>(v)}); }

#endif // TKN_SPAN_H
