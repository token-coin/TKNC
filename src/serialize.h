// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-present The TKN Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef TKN_SERIALIZE_H
#define TKN_SERIALIZE_H

#include <attributes.h>
#include <compat/assumptions.h> // IWYU pragma: keep
#include <compat/endian.h>
#include <prevector.h>
#include <span.h>
#include <util/overflow.h>

#include <algorithm>
#include <concepts>
#include <cstdint>
#include <cstring>
#include <ios>
#include <limits>
#include <map>
#include <memory>
#include <set>
#include <span>
#include <string>
#include <utility>
#include <vector>

// Maximum size of a serialized object in bytes or number of elements (for eg vectors) when size is encoded as CompactSize.
static constexpr uint64_t MAX_SIZE = 0x02000000;

// Maximum amount of memory (in bytes) to allocate at once when deserializing vectors.
static const unsigned int MAX_VECTOR_ALLOCATE = 5000000;

// Dummy data type to identify deserializing constructors. By convention, `template<typename Stream> T::T(deserialize_type, Stream& s)` is a deserializing constructor that builds T by deserializing from s. If T contains const fields, this is likely the only way to construct it.
struct deserialize_type {};
constexpr deserialize_type deserialize {};

// Lowest-level serialization and conversion.
template<typename Stream> inline void ser_writedata8(Stream &s, uint8_t obj)
{
 s.write(std::as_bytes(std::span{&obj, 1}));
}
template<typename Stream> inline void ser_writedata16(Stream &s, uint16_t obj)
{
 obj = htole16_internal(obj);
 s.write(std::as_bytes(std::span{&obj, 1}));
}
template<typename Stream> inline void ser_writedata32(Stream &s, uint32_t obj)
{
 obj = htole32_internal(obj);
 s.write(std::as_bytes(std::span{&obj, 1}));
}
template<typename Stream> inline void ser_writedata32be(Stream &s, uint32_t obj)
{
 obj = htobe32_internal(obj);
 s.write(std::as_bytes(std::span{&obj, 1}));
}
template<typename Stream> inline void ser_writedata64(Stream &s, uint64_t obj)
{
 obj = htole64_internal(obj);
 s.write(std::as_bytes(std::span{&obj, 1}));
}
template<typename Stream> inline uint8_t ser_readdata8(Stream &s)
{
 uint8_t obj;
 s.read(std::as_writable_bytes(std::span{&obj, 1}));
 return obj;
}
template<typename Stream> inline uint16_t ser_readdata16(Stream &s)
{
 uint16_t obj;
 s.read(std::as_writable_bytes(std::span{&obj, 1}));
 return le16toh_internal(obj);
}
template<typename Stream> inline uint32_t ser_readdata32(Stream &s)
{
 uint32_t obj;
 s.read(std::as_writable_bytes(std::span{&obj, 1}));
 return le32toh_internal(obj);
}
template<typename Stream> inline uint32_t ser_readdata32be(Stream &s)
{
 uint32_t obj;
 s.read(std::as_writable_bytes(std::span{&obj, 1}));
 return be32toh_internal(obj);
}
template<typename Stream> inline uint64_t ser_readdata64(Stream &s)
{
 uint64_t obj;
 s.read(std::as_writable_bytes(std::span{&obj, 1}));
 return le64toh_internal(obj);
}


class SizeComputer;

// Convert any argument to a reference to X, maintaining constness. Used in serialization code to invoke a base class's serialization routines. static_cast cannot easily be used here since Obj is const Child& during serialization and Child& during deserialization; AsBase converts to const Base& and Base& appropriately. Example: `SERIALIZE_METHODS(Child, obj) { READWRITE(AsBase<Base>(obj), obj.m_data); }`.
template <class Out, class In>
Out& AsBase(In& x)
{
 static_assert(std::is_base_of_v<Out, In>);
 return x;
}
template <class Out, class In>
const Out& AsBase(const In& x)
{
 static_assert(std::is_base_of_v<Out, In>);
 return x;
}

#define READWRITE(...) (ser_action.SerReadWriteMany(s, __VA_ARGS__))
#define SER_READ(obj, code) ser_action.SerRead(s, obj, [&](Stream& s, std::remove_const_t<Type>& obj) { code; })
#define SER_WRITE(obj, code) ser_action.SerWrite(s, obj, [&](Stream& s, const Type& obj) { code; })

// Implement Ser/Unser for a formatter (see Using). Both delegate to single static SerializationOps, polymorphic in (de)serialized type (const for ser, non-const for unser). Example: `struct FooFormatter { FORMATTER_METHODS(Class, obj) { READWRITE(obj.val1, VARINT(obj.val2)); } }` defines FooFormatter that serializes Class via val1 (default) + val2 (VARINT), usable as `READWRITE(Using<FooFormatter>(obj.bla))`.
#define FORMATTER_METHODS(cls, obj) \
 template<typename Stream> \
 static void Ser(Stream& s, const cls& obj) { SerializationOps(obj, s, ActionSerialize{}); } \
 template<typename Stream> \
 static void Unser(Stream& s, cls& obj) { SerializationOps(obj, s, ActionUnserialize{}); } \
 template<typename Stream, typename Type, typename Operation> \
 static void SerializationOps(Type& obj, Stream& s, Operation ser_action)

// Formatter methods retrieve stream-attached parameters via SER_PARAMS(type) macro. Permits type-safe runtime-context-dependent serialization. parameter(obj) callable anywhere in call stack; passed down recursively until overridden. Parameters implicitly converted (parent code can use param derived from/convertible to child formatter's param type). Compilation fails if no convertible param is provided. Example: `READWRITE(BarParameter{...}(Using<FooFormatter>(obj.foo)))` then `auto& param = SER_PARAMS(BarParameter); if (param.fancy) READWRITE(VARINT(obj.value)); else READWRITE(obj.value);`.
#define SER_PARAMS(type) (s.template GetParams<type>())

#define BASE_SERIALIZE_METHODS(cls) \
 template <typename Stream> \
 void Serialize(Stream& s) const \
 { \
 static_assert(std::is_same_v<const cls&, decltype(*this)>, "Serialize type mismatch"); \
 Ser(s, *this); \
 } \
 template <typename Stream> \
 void Unserialize(Stream& s) \
 { \
 static_assert(std::is_same_v<cls&, decltype(*this)>, "Unserialize type mismatch"); \
 Unser(s, *this); \
 }

// Implement Serialize/Unserialize by delegating to a single templated static method taking the to-be-(de)serialized object. Constness becomes a template parameter, allowing a single impl that sees object as const for ser and non-const for unser, without casts.
#define SERIALIZE_METHODS(cls, obj) \
 BASE_SERIALIZE_METHODS(cls) \
 FORMATTER_METHODS(cls, obj)

// Templates for serializing to anything that looks like a stream, i.e. anything that supports .read(std::span<std::byte>) and .write(std::span<const std::byte>).

// Typically int8_t and char are distinct types, but some systems may define int8_t in terms of char. Forbid char serialization in typical case, but allow if it's the only way to describe an int8_t.
template<class T>
concept CharNotInt8 = std::same_as<T, char> && !std::same_as<T, int8_t>;

// clang-format off
template <typename Stream, CharNotInt8 V> void Serialize(Stream&, V) = delete; // char serialization forbidden. Use uint8_t or int8_t
template <typename Stream> void Serialize(Stream& s, std::byte a) { ser_writedata8(s, uint8_t(a)); }
template <typename Stream> void Serialize(Stream& s, int8_t a) { ser_writedata8(s, uint8_t(a)); }
template <typename Stream> void Serialize(Stream& s, uint8_t a) { ser_writedata8(s, a); }
template <typename Stream> void Serialize(Stream& s, int16_t a) { ser_writedata16(s, uint16_t(a)); }
template <typename Stream> void Serialize(Stream& s, uint16_t a) { ser_writedata16(s, a); }
template <typename Stream> void Serialize(Stream& s, int32_t a) { ser_writedata32(s, uint32_t(a)); }
template <typename Stream> void Serialize(Stream& s, uint32_t a) { ser_writedata32(s, a); }
template <typename Stream> void Serialize(Stream& s, int64_t a) { ser_writedata64(s, uint64_t(a)); }
template <typename Stream> void Serialize(Stream& s, uint64_t a) { ser_writedata64(s, a); }

template <typename Stream, BasicByte B, size_t N> void Serialize(Stream& s, const B (&a)[N]) { s.write(MakeByteSpan(a)); }
template <typename Stream, BasicByte B, size_t N> void Serialize(Stream& s, const std::array<B, N>& a) { s.write(MakeByteSpan(a)); }
template <typename Stream, BasicByte B, size_t N> void Serialize(Stream& s, std::span<B, N> span) { s.write(std::as_bytes(span)); }
template <typename Stream, BasicByte B> void Serialize(Stream& s, std::span<B> span) { s.write(std::as_bytes(span)); }

template <typename Stream, CharNotInt8 V> void Unserialize(Stream&, V) = delete; // char serialization forbidden. Use uint8_t or int8_t
template <typename Stream> void Unserialize(Stream& s, std::byte& a) { a = std::byte(ser_readdata8(s)); }
template <typename Stream> void Unserialize(Stream& s, int8_t& a) { a = int8_t(ser_readdata8(s)); }
template <typename Stream> void Unserialize(Stream& s, uint8_t& a) { a = ser_readdata8(s); }
template <typename Stream> void Unserialize(Stream& s, int16_t& a) { a = int16_t(ser_readdata16(s)); }
template <typename Stream> void Unserialize(Stream& s, uint16_t& a) { a = ser_readdata16(s); }
template <typename Stream> void Unserialize(Stream& s, int32_t& a) { a = int32_t(ser_readdata32(s)); }
template <typename Stream> void Unserialize(Stream& s, uint32_t& a) { a = ser_readdata32(s); }
template <typename Stream> void Unserialize(Stream& s, int64_t& a) { a = int64_t(ser_readdata64(s)); }
template <typename Stream> void Unserialize(Stream& s, uint64_t& a) { a = ser_readdata64(s); }

template <typename Stream, BasicByte B, size_t N> void Unserialize(Stream& s, B (&a)[N]) { s.read(MakeWritableByteSpan(a)); }
template <typename Stream, BasicByte B, size_t N> void Unserialize(Stream& s, std::array<B, N>& a) { s.read(MakeWritableByteSpan(a)); }
template <typename Stream, BasicByte B, size_t N> void Unserialize(Stream& s, std::span<B, N> span) { s.read(std::as_writable_bytes(span)); }
template <typename Stream, BasicByte B> void Unserialize(Stream& s, std::span<B> span) { s.read(std::as_writable_bytes(span)); }

template <typename Stream> void Serialize(Stream& s, bool a) { uint8_t f = a; ser_writedata8(s, f); }
template <typename Stream> void Unserialize(Stream& s, bool& a) { uint8_t f = ser_readdata8(s); a = f; }
// clang-format on


// (Chinese comment removed)
constexpr inline unsigned int GetSizeOfCompactSize(uint64_t nSize)
{
 if (nSize < 253) return sizeof(unsigned char);
 else if (nSize <= std::numeric_limits<uint16_t>::max()) return sizeof(unsigned char) + sizeof(uint16_t);
 else if (nSize <= std::numeric_limits<unsigned int>::max()) return sizeof(unsigned char) + sizeof(unsigned int);
 else return sizeof(unsigned char) + sizeof(uint64_t);
}

inline void WriteCompactSize(SizeComputer& os, uint64_t nSize);

template<typename Stream>
void WriteCompactSize(Stream& os, uint64_t nSize)
{
 if (nSize < 253)
 {
 ser_writedata8(os, nSize);
 }
 else if (nSize <= std::numeric_limits<uint16_t>::max())
 {
 ser_writedata8(os, 253);
 ser_writedata16(os, nSize);
 }
 else if (nSize <= std::numeric_limits<unsigned int>::max())
 {
 ser_writedata8(os, 254);
 ser_writedata32(os, nSize);
 }
 else
 {
 ser_writedata8(os, 255);
 ser_writedata64(os, nSize);
 }
 return;
}

// Decode a CompactSize-encoded variable-length integer. Primarily used to encode size of vector-like serializations, so range check is performed by default; set range_check=false for generic number encoding.
template<typename Stream>
uint64_t ReadCompactSize(Stream& is, bool range_check = true)
{
 uint8_t chSize = ser_readdata8(is);
 uint64_t nSizeRet = 0;
 if (chSize < 253)
 {
 nSizeRet = chSize;
 }
 else if (chSize == 253)
 {
 nSizeRet = ser_readdata16(is);
 if (nSizeRet < 253)
 throw std::ios_base::failure("non-canonical ReadCompactSize()");
 }
 else if (chSize == 254)
 {
 nSizeRet = ser_readdata32(is);
 if (nSizeRet < 0x10000u)
 throw std::ios_base::failure("non-canonical ReadCompactSize()");
 }
 else
 {
 nSizeRet = ser_readdata64(is);
 if (nSizeRet < 0x100000000ULL)
 throw std::ios_base::failure("non-canonical ReadCompactSize()");
 }
 if (range_check && nSizeRet > MAX_SIZE) {
 throw std::ios_base::failure("ReadCompactSize(): size too large");
 }
 return nSizeRet;
}

// (Chinese comment removed)

// Mode for encoding VarInts. No signed encoding support yet. DEFAULT mode won't compile with signed values; legacy NONNEGATIVE_SIGNED mode accepts signed but misencodes negatives. Future: DEFAULT could be extended for negatives in backwards-compatible way; additional modes (e.g. zigzag) could be added.
enum class VarIntMode { DEFAULT, NONNEGATIVE_SIGNED };

template <VarIntMode Mode, typename I>
struct CheckVarIntMode {
 constexpr CheckVarIntMode()
 {
 static_assert(Mode != VarIntMode::DEFAULT || std::is_unsigned_v<I>, "Unsigned type required with mode DEFAULT.");
 static_assert(Mode != VarIntMode::NONNEGATIVE_SIGNED || std::is_signed_v<I>, "Signed type required with mode NONNEGATIVE_SIGNED.");
 }
};

template<VarIntMode Mode, typename I>
inline unsigned int GetSizeOfVarInt(I n)
{
 CheckVarIntMode<Mode, I>();
 int nRet = 0;
 while(true) {
 nRet++;
 if (n <= 0x7F)
 break;
 n = (n >> 7) - 1;
 }
 return nRet;
}

template<typename I>
inline void WriteVarInt(SizeComputer& os, I n);

template<typename Stream, VarIntMode Mode, typename I>
void WriteVarInt(Stream& os, I n)
{
 CheckVarIntMode<Mode, I>();
 unsigned char tmp[CeilDiv(sizeof(n) * 8, 7u)];
 int len=0;
 while(true) {
 tmp[len] = (n & 0x7F) | (len ? 0x80 : 0x00);
 if (n <= 0x7F)
 break;
 n = (n >> 7) - 1;
 len++;
 }
 do {
 ser_writedata8(os, tmp[len]);
 } while(len--);
}

template<typename Stream, VarIntMode Mode, typename I>
I ReadVarInt(Stream& is)
{
 CheckVarIntMode<Mode, I>();
 I n = 0;
 while(true) {
 unsigned char chData = ser_readdata8(is);
 if (n > (std::numeric_limits<I>::max() >> 7)) {
 throw std::ios_base::failure("ReadVarInt(): size too large");
 }
 n = (n << 7) | (chData & 0x7F);
 if (chData & 0x80) {
 if (n == std::numeric_limits<I>::max()) {
 throw std::ios_base::failure("ReadVarInt(): size too large");
 }
 n++;
 } else {
 return n;
 }
 }
}

/** Simple wrapper class to serialize objects using a formatter; used by Using(). */
template<typename Formatter, typename T>
class Wrapper
{
 static_assert(std::is_lvalue_reference_v<T>, "Wrapper needs an lvalue reference type T");
protected:
 T m_object;
public:
 explicit Wrapper(T obj) : m_object(obj) {}
 template<typename Stream> void Serialize(Stream &s) const { Formatter().Ser(s, m_object); }
 template<typename Stream> void Unserialize(Stream &s) { Formatter().Unser(s, m_object); }
};

// Cause (de)serialization of an object using a specified formatter class. Formatter needs public Ser(stream, const object&) and Unser(stream, object&). Use as Using<Formatter>(object) inside READWRITE or with <</>> operators. Constructs Wrapper<Formatter, T> where T is const during ser and non-const during unser, maintaining const correctness.
template<typename Formatter, typename T>
static inline Wrapper<Formatter, T&> Using(T&& t) { return Wrapper<Formatter, T&>(t); }

#define VARINT_MODE(obj, mode) Using<VarIntFormatter<mode>>(obj)
#define VARINT(obj) Using<VarIntFormatter<VarIntMode::DEFAULT>>(obj)
#define COMPACTSIZE(obj) Using<CompactSizeFormatter<true>>(obj)
#define LIMITED_STRING(obj,n) Using<LimitedStringFormatter<n>>(obj)

/** Serialization wrapper class for integers in VarInt format. */
template<VarIntMode Mode>
struct VarIntFormatter
{
 template<typename Stream, typename I> void Ser(Stream &s, I v)
 {
 WriteVarInt<Stream,Mode, std::remove_cv_t<I>>(s, v);
 }

 template<typename Stream, typename I> void Unser(Stream& s, I& v)
 {
 v = ReadVarInt<Stream,Mode, std::remove_cv_t<I>>(s);
 }
};

// Serialization wrapper for custom integers and enums. Permits specifying serialized size (1-8 bytes) and endianness. Big endian mode only for compatibility with existing formats (not recommended for new data structures).
template<int Bytes, bool BigEndian = false>
struct CustomUintFormatter
{
 static_assert(Bytes > 0 && Bytes <= 8, "CustomUintFormatter Bytes out of range");
 static constexpr uint64_t MAX = 0xffffffffffffffff >> (8 * (8 - Bytes));

 template <typename Stream, typename I> void Ser(Stream& s, I v)
 {
 if (v < 0 || v > MAX) throw std::ios_base::failure("CustomUintFormatter value out of range");
 if (BigEndian) {
 uint64_t raw = htobe64_internal(v);
 s.write(std::as_bytes(std::span{&raw, 1}).last(Bytes));
 } else {
 uint64_t raw = htole64_internal(v);
 s.write(std::as_bytes(std::span{&raw, 1}).first(Bytes));
 }
 }

 template <typename Stream, typename I> void Unser(Stream& s, I& v)
 {
 using U = typename std::conditional_t<std::is_enum_v<I>, std::underlying_type<I>, std::common_type<I>>::type;
 static_assert(std::numeric_limits<U>::max() >= MAX && std::numeric_limits<U>::min() <= 0, "Assigned type too small");
 uint64_t raw = 0;
 if (BigEndian) {
 s.read(std::as_writable_bytes(std::span{&raw, 1}).last(Bytes));
 v = static_cast<I>(be64toh_internal(raw));
 } else {
 s.read(std::as_writable_bytes(std::span{&raw, 1}).first(Bytes));
 v = static_cast<I>(le64toh_internal(raw));
 }
 }
};

template<int Bytes> using BigEndianFormatter = CustomUintFormatter<Bytes, true>;

/** Formatter for integers in CompactSize format. */
template<bool RangeCheck>
struct CompactSizeFormatter
{
 template<typename Stream, typename I>
 void Unser(Stream& s, I& v)
 {
 uint64_t n = ReadCompactSize<Stream>(s, RangeCheck);
 if (n < std::numeric_limits<I>::min() || n > std::numeric_limits<I>::max()) {
 throw std::ios_base::failure("CompactSize exceeds limit of type");
 }
 v = n;
 }

 template<typename Stream, typename I>
 void Ser(Stream& s, I v)
 {
 static_assert(std::is_unsigned_v<I>, "CompactSize only supported for unsigned integers");
 static_assert(std::numeric_limits<I>::max() <= std::numeric_limits<uint64_t>::max(), "CompactSize only supports 64-bit integers and below");

 WriteCompactSize<Stream>(s, v);
 }
};

template <typename U, bool LOSSY = false>
struct ChronoFormatter {
 template <typename Stream, typename Tp>
 void Unser(Stream& s, Tp& tp)
 {
 U u;
 s >> u;
 // Lossy deserialization does not make sense, so force Wnarrowing
 tp = Tp{typename Tp::duration{typename Tp::duration::rep{u}}};
 }
 template <typename Stream, typename Tp>
 void Ser(Stream& s, Tp tp)
 {
 if constexpr (LOSSY) {
 s << U(tp.time_since_epoch().count());
 } else {
 s << U{tp.time_since_epoch().count()};
 }
 }
};
template <typename U>
using LossyChronoFormatter = ChronoFormatter<U, true>;

class CompactSizeWriter
{
protected:
 uint64_t n;
public:
 explicit CompactSizeWriter(uint64_t n_in) : n(n_in) { }

 template<typename Stream>
 void Serialize(Stream &s) const {
 WriteCompactSize<Stream>(s, n);
 }
};

template<size_t Limit>
struct LimitedStringFormatter
{
 template<typename Stream>
 void Unser(Stream& s, std::string& v)
 {
 size_t size = ReadCompactSize(s);
 if (size > Limit) {
 throw std::ios_base::failure("String length limit exceeded");
 }
 v.resize(size);
 if (size != 0) s.read(MakeWritableByteSpan(v));
 }

 template<typename Stream>
 void Ser(Stream& s, const std::string& v)
 {
 s << v;
 }
};

// (Chinese comment removed)
template<class Formatter>
struct VectorFormatter
{
 template<typename Stream, typename V>
 void Ser(Stream& s, const V& v)
 {
 Formatter formatter;
 WriteCompactSize(s, v.size());
 for (const typename V::value_type& elem : v) {
 formatter.Ser(s, elem);
 }
 }

 template<typename Stream, typename V>
 void Unser(Stream& s, V& v)
 {
 Formatter formatter;
 v.clear();
 size_t size = ReadCompactSize(s);
 size_t allocated = 0;
 while (allocated < size) {
 // For DoS prevention, allocate in 5MiB batches rather than blindly allocating as much as stream claims. Attacker must provide X MiB of data to make us allocate X+5 MiB.
 static_assert(sizeof(typename V::value_type) <= MAX_VECTOR_ALLOCATE, "Vector element size too large");
 allocated = std::min(size, allocated + MAX_VECTOR_ALLOCATE / sizeof(typename V::value_type));
 v.reserve(allocated);
 while (v.size() < allocated) {
 v.emplace_back();
 formatter.Unser(s, v.back());
 }
 }
 };
};

// Forward declarations

// string
template<typename Stream, typename C> void Serialize(Stream& os, const std::basic_string<C>& str);
template<typename Stream, typename C> void Unserialize(Stream& is, std::basic_string<C>& str);

// prevector
template<typename Stream, unsigned int N, typename T> inline void Serialize(Stream& os, const prevector<N, T>& v);
template<typename Stream, unsigned int N, typename T> inline void Unserialize(Stream& is, prevector<N, T>& v);

// vector
template<typename Stream, typename T, typename A> inline void Serialize(Stream& os, const std::vector<T, A>& v);
template<typename Stream, typename T, typename A> inline void Unserialize(Stream& is, std::vector<T, A>& v);

// pair
template<typename Stream, typename K, typename T> void Serialize(Stream& os, const std::pair<K, T>& item);
template<typename Stream, typename K, typename T> void Unserialize(Stream& is, std::pair<K, T>& item);

// map
template<typename Stream, typename K, typename T, typename Pred, typename A> void Serialize(Stream& os, const std::map<K, T, Pred, A>& m);
template<typename Stream, typename K, typename T, typename Pred, typename A> void Unserialize(Stream& is, std::map<K, T, Pred, A>& m);

// set
template<typename Stream, typename K, typename Pred, typename A> void Serialize(Stream& os, const std::set<K, Pred, A>& m);
template<typename Stream, typename K, typename Pred, typename A> void Unserialize(Stream& is, std::set<K, Pred, A>& m);

// shared_ptr
template<typename Stream, typename T> void Serialize(Stream& os, const std::shared_ptr<const T>& p);
template<typename Stream, typename T> void Unserialize(Stream& os, std::shared_ptr<const T>& p);

// unique_ptr
template<typename Stream, typename T> void Serialize(Stream& os, const std::unique_ptr<const T>& p);
template<typename Stream, typename T> void Unserialize(Stream& os, std::unique_ptr<const T>& p);


// If none of the specialized versions above matched, default to calling member function.
template <class T, class Stream>
concept Serializable = requires(T a, Stream s) { a.Serialize(s); };
template <typename Stream, typename T>
 requires Serializable<T, Stream>
void Serialize(Stream& os, const T& a)
{
 a.Serialize(os);
}

template <class T, class Stream>
concept Unserializable = requires(T a, Stream s) { a.Unserialize(s); };
template <typename Stream, typename T>
 requires Unserializable<T, Stream>
void Unserialize(Stream& is, T&& a)
{
 a.Unserialize(is);
}

// Default formatter. Serializes objects as themselves. Vector/prevector serialization code passes this to VectorFormatter to enable reusing that logic; shouldn't be needed elsewhere.
struct DefaultFormatter
{
 template<typename Stream, typename T>
 static void Ser(Stream& s, const T& t) { Serialize(s, t); }

 template<typename Stream, typename T>
 static void Unser(Stream& s, T& t) { Unserialize(s, t); }
};





// string
template<typename Stream, typename C>
void Serialize(Stream& os, const std::basic_string<C>& str)
{
 WriteCompactSize(os, str.size());
 if (!str.empty())
 os.write(MakeByteSpan(str));
}

template<typename Stream, typename C>
void Unserialize(Stream& is, std::basic_string<C>& str)
{
 unsigned int nSize = ReadCompactSize(is);
 str.resize(nSize);
 if (nSize != 0)
 is.read(MakeWritableByteSpan(str));
}



// prevector
template <typename Stream, unsigned int N, typename T>
void Serialize(Stream& os, const prevector<N, T>& v)
{
 if constexpr (BasicByte<T>) { // Use optimized version for unformatted basic bytes
 WriteCompactSize(os, v.size());
 if (!v.empty()) os.write(MakeByteSpan(v));
 } else {
 Serialize(os, Using<VectorFormatter<DefaultFormatter>>(v));
 }
}


template <typename Stream, unsigned int N, typename T>
void Unserialize(Stream& is, prevector<N, T>& v)
{
 if constexpr (BasicByte<T>) { // Use optimized version for unformatted basic bytes
 // Limit size per read so bogus size value won't cause out of memory
 v.clear();
 unsigned int nSize = ReadCompactSize(is);
 unsigned int i = 0;
 while (i < nSize) {
 unsigned int blk = std::min(nSize - i, (unsigned int)(1 + 4999999 / sizeof(T)));
 v.resize_uninitialized(i + blk);
 is.read(std::as_writable_bytes(std::span{&v[i], blk}));
 i += blk;
 }
 } else {
 Unserialize(is, Using<VectorFormatter<DefaultFormatter>>(v));
 }
}


// vector
template <typename Stream, typename T, typename A>
void Serialize(Stream& os, const std::vector<T, A>& v)
{
 if constexpr (BasicByte<T>) { // Use optimized version for unformatted basic bytes
 WriteCompactSize(os, v.size());
 if (!v.empty()) os.write(MakeByteSpan(v));
 } else if constexpr (std::is_same_v<T, bool>) {
 // Special case for std::vector<bool>: dereferencing const_iterator doesn't yield const bool& due to std::vector's bool special casing.
 WriteCompactSize(os, v.size());
 for (bool elem : v) {
 ::Serialize(os, elem);
 }
 } else {
 Serialize(os, Using<VectorFormatter<DefaultFormatter>>(v));
 }
}


template <typename Stream, typename T, typename A>
void Unserialize(Stream& is, std::vector<T, A>& v)
{
 if constexpr (BasicByte<T>) { // Use optimized version for unformatted basic bytes
 // Limit size per read so bogus size value won't cause out of memory
 v.clear();
 unsigned int nSize = ReadCompactSize(is);
 unsigned int i = 0;
 while (i < nSize) {
 unsigned int blk = std::min(nSize - i, (unsigned int)(1 + 4999999 / sizeof(T)));
 v.resize(i + blk);
 is.read(std::as_writable_bytes(std::span{&v[i], blk}));
 i += blk;
 }
 } else {
 Unserialize(is, Using<VectorFormatter<DefaultFormatter>>(v));
 }
}


// pair
template<typename Stream, typename K, typename T>
void Serialize(Stream& os, const std::pair<K, T>& item)
{
 Serialize(os, item.first);
 Serialize(os, item.second);
}

template<typename Stream, typename K, typename T>
void Unserialize(Stream& is, std::pair<K, T>& item)
{
 Unserialize(is, item.first);
 Unserialize(is, item.second);
}



// map
template<typename Stream, typename K, typename T, typename Pred, typename A>
void Serialize(Stream& os, const std::map<K, T, Pred, A>& m)
{
 WriteCompactSize(os, m.size());
 for (const auto& entry : m)
 Serialize(os, entry);
}

template<typename Stream, typename K, typename T, typename Pred, typename A>
void Unserialize(Stream& is, std::map<K, T, Pred, A>& m)
{
 m.clear();
 unsigned int nSize = ReadCompactSize(is);
 typename std::map<K, T, Pred, A>::iterator mi = m.begin();
 for (unsigned int i = 0; i < nSize; i++)
 {
 std::pair<K, T> item;
 Unserialize(is, item);
 mi = m.insert(mi, item);
 }
}



// set
template<typename Stream, typename K, typename Pred, typename A>
void Serialize(Stream& os, const std::set<K, Pred, A>& m)
{
 WriteCompactSize(os, m.size());
 for (typename std::set<K, Pred, A>::const_iterator it = m.begin(); it != m.end(); ++it)
 Serialize(os, (*it));
}

template<typename Stream, typename K, typename Pred, typename A>
void Unserialize(Stream& is, std::set<K, Pred, A>& m)
{
 m.clear();
 unsigned int nSize = ReadCompactSize(is);
 typename std::set<K, Pred, A>::iterator it = m.begin();
 for (unsigned int i = 0; i < nSize; i++)
 {
 K key;
 Unserialize(is, key);
 it = m.insert(it, key);
 }
}



// unique_ptr
template<typename Stream, typename T> void
Serialize(Stream& os, const std::unique_ptr<const T>& p)
{
 Serialize(os, *p);
}

template<typename Stream, typename T>
void Unserialize(Stream& is, std::unique_ptr<const T>& p)
{
 p.reset(new T(deserialize, is));
}



// shared_ptr
template<typename Stream, typename T> void
Serialize(Stream& os, const std::shared_ptr<const T>& p)
{
 Serialize(os, *p);
}

template<typename Stream, typename T>
void Unserialize(Stream& is, std::shared_ptr<const T>& p)
{
 p = std::make_shared<const T>(deserialize, is);
}

// Support for (un)serializing many things at once

template <typename Stream, typename... Args>
void SerializeMany(Stream& s, const Args&... args)
{
 (::Serialize(s, args), ...);
}

template <typename Stream, typename... Args>
inline void UnserializeMany(Stream& s, Args&&... args)
{
 (::Unserialize(s, args), ...);
}

// Support for all macros providing or using the ser_action parameter of the SerializationOps method.
struct ActionSerialize {
 static constexpr bool ForRead() { return false; }

 template<typename Stream, typename... Args>
 static void SerReadWriteMany(Stream& s, const Args&... args)
 {
 ::SerializeMany(s, args...);
 }

 template<typename Stream, typename Type, typename Fn>
 static void SerRead(Stream& s, Type&&, Fn&&)
 {
 }

 template<typename Stream, typename Type, typename Fn>
 static void SerWrite(Stream& s, Type&& obj, Fn&& fn)
 {
 fn(s, std::forward<Type>(obj));
 }
};
struct ActionUnserialize {
 static constexpr bool ForRead() { return true; }

 template<typename Stream, typename... Args>
 static void SerReadWriteMany(Stream& s, Args&&... args)
 {
 ::UnserializeMany(s, args...);
 }

 template<typename Stream, typename Type, typename Fn>
 static void SerRead(Stream& s, Type&& obj, Fn&& fn)
 {
 fn(s, std::forward<Type>(obj));
 }

 template<typename Stream, typename Type, typename Fn>
 static void SerWrite(Stream& s, Type&&, Fn&&)
 {
 }
};

// ::GetSerializeSize implementations: computing serialized size of objects is done through a special SizeComputer stream that only records bytes written. If your Serialize/SerializationOp method has non-trivial serialization overhead, implement a specialized SizeComputer version using s.seek() to record bytes that would be written.
class SizeComputer
{
protected:
 uint64_t m_size{0};

public:
 SizeComputer() = default;

 void write(std::span<const std::byte> src)
 {
 m_size += src.size();
 }

 /** Pretend this many bytes are written, without specifying them. */
 void seek(uint64_t num)
 {
 m_size += num;
 }

 template <typename T>
 SizeComputer& operator<<(const T& obj)
 {
 ::Serialize(*this, obj);
 return *this;
 }

 uint64_t size() const
 {
 return m_size;
 }
};

template<typename I>
inline void WriteVarInt(SizeComputer &s, I n)
{
 s.seek(GetSizeOfVarInt<I>(n));
}

inline void WriteCompactSize(SizeComputer &s, uint64_t nSize)
{
 s.seek(GetSizeOfCompactSize(nSize));
}

template <typename T>
uint64_t GetSerializeSize(const T& t)
{
 return (SizeComputer() << t).size();
}

//! Check if type contains a stream by seeing if has a GetStream() method.
template<typename T>
concept ContainsStream = requires(T t) { t.GetStream(); };

/** Wrapper that overrides the GetParams() function of a stream. */
template <typename SubStream, typename Params>
class ParamsStream
{
 const Params& m_params;
 // If ParamsStream constructor receives lvalue: Substream is reference type, m_substream references the arg. Otherwise m_substream is a substream instance moved from the arg. Holding a substream instance makes ParamsStream self-contained for cleanup (e.g. closing files if SubStream is a file stream).
 SubStream m_substream;

public:
 ParamsStream(SubStream&& substream, const Params& params LIFETIMEBOUND) : m_params{params}, m_substream{std::forward<SubStream>(substream)} {}

 template <typename NestedSubstream, typename Params1, typename Params2, typename... NestedParams>
 ParamsStream(NestedSubstream&& s, const Params1& params1 LIFETIMEBOUND, const Params2& params2 LIFETIMEBOUND, const NestedParams&... params LIFETIMEBOUND)
 : ParamsStream{::ParamsStream{std::forward<NestedSubstream>(s), params2, params...}, params1} {}

 template <typename U> ParamsStream& operator<<(const U& obj) { ::Serialize(*this, obj); return *this; }
 template <typename U> ParamsStream& operator>>(U&& obj) { ::Unserialize(*this, obj); return *this; }
 void write(std::span<const std::byte> src) { GetStream().write(src); }
 void read(std::span<std::byte> dst) { GetStream().read(dst); }
 void ignore(size_t num) { GetStream().ignore(num); }
 bool empty() const { return GetStream().empty(); }
 size_t size() const { return GetStream().size(); }

 //! Get reference to stream parameters.
 template <typename P>
 const auto& GetParams() const
 {
 if constexpr (std::is_convertible_v<Params, P>) {
 return m_params;
 } else {
 return m_substream.template GetParams<P>();
 }
 }

 //! Get reference to underlying stream.
 auto& GetStream()
 {
 if constexpr (ContainsStream<SubStream>) {
 return m_substream.GetStream();
 } else {
 return m_substream;
 }
 }
 const auto& GetStream() const
 {
 if constexpr (ContainsStream<SubStream>) {
 return m_substream.GetStream();
 } else {
 return m_substream;
 }
 }
};

// Explicit template deduction guide required for single-parameter constructor so Substream&& is treated as forwarding reference, and SubStream is deduced as reference type for lvalue arguments.
template <typename Substream, typename Params>
ParamsStream(Substream&&, const Params&) -> ParamsStream<Substream, Params>;

// Template deduction guide for multiple params arguments that creates a nested ParamsStream.
template <typename Substream, typename Params1, typename Params2, typename... Params>
ParamsStream(Substream&& s, const Params1& params1, const Params2& params2, const Params&... params) ->
 ParamsStream<decltype(ParamsStream{std::forward<Substream>(s), params2, params...}), Params1>;

/** Wrapper that serializes objects with the specified parameters. */
template <typename Params, typename T>
class ParamsWrapper
{
 const Params& m_params;
 T& m_object;

public:
 explicit ParamsWrapper(const Params& params, T& obj) : m_params{params}, m_object{obj} {}

 template <typename Stream>
 void Serialize(Stream& s) const
 {
 ParamsStream ss{s, m_params};
 ::Serialize(ss, m_object);
 }
 template <typename Stream>
 void Unserialize(Stream& s)
 {
 ParamsStream ss{s, m_params};
 ::Unserialize(ss, m_object);
 }
};

// Helper macro for SerParams structs. Allows defining SerParams instances and applying them directly to an object via function call syntax, e.g. `constexpr SerParams FOO{....}; ss << FOO(obj);`.
#define SER_PARAMS_OPFUNC \
 /** Return a wrapper around t that (de)serializes it with specified parameter params. See SER_PARAMS for more information on serialization parameters. */ \
 template <typename T> \
 auto operator()(T&& t) const \
 { \
 return ParamsWrapper{*this, t}; \
 }

#endif // TKN_SERIALIZE_H
