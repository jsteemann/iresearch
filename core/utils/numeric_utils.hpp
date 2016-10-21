//
// IResearch search engine 
// 
// Copyright � 2016 by EMC Corporation, All Rights Reserved
// 
// This software contains the intellectual property of EMC Corporation or is licensed to
// EMC Corporation from third parties. Use of this software and the intellectual property
// contained therein is expressly limited to the terms and conditions of the License
// Agreement under which it is provided by or on behalf of EMC.
// 

#ifndef IRESEARCH_NUMERIC_UTILS_H
#define IRESEARCH_NUMERIC_UTILS_H

#include "utils/string.hpp"

NS_ROOT
NS_BEGIN(numeric_utils)

inline CONSTEXPR bool is_big_endian() { 
  return *(uint16_t*)"\0\xff" < 0x100; 
}

IRESEARCH_API size_t encode64(uint64_t value, byte_type* out, size_t shift = 0);
IRESEARCH_API uint64_t decode64(const byte_type* out);
IRESEARCH_API const bytes_ref& mini64();
IRESEARCH_API const bytes_ref& maxi64();

IRESEARCH_API size_t encode32(uint32_t value, byte_type* out, size_t shift = 0);
IRESEARCH_API uint32_t decode32(const byte_type* out);
IRESEARCH_API const bytes_ref& mini32();
IRESEARCH_API const bytes_ref& maxi32();

IRESEARCH_API size_t encodef32(uint32_t value, byte_type* out, size_t shift = 0);
IRESEARCH_API uint32_t decodef32(const byte_type* out);
IRESEARCH_API int32_t ftoi32(float_t value);
IRESEARCH_API float_t i32tof(int32_t value);
IRESEARCH_API const bytes_ref& minf32();
IRESEARCH_API const bytes_ref& maxf32();
IRESEARCH_API const bytes_ref& finf32();
IRESEARCH_API const bytes_ref& nfinf32();

IRESEARCH_API size_t encoded64(uint64_t value, byte_type* out, size_t shift = 0);
IRESEARCH_API uint64_t decoded64(const byte_type* out);
IRESEARCH_API int64_t dtoi64(double_t value);
IRESEARCH_API double_t i64tod(int64_t value);
IRESEARCH_API const bytes_ref& mind64();
IRESEARCH_API const bytes_ref& maxd64();
IRESEARCH_API const bytes_ref& dinf64();
IRESEARCH_API const bytes_ref& ndinf64();

template<typename T>
struct numeric_traits;

template<>
struct numeric_traits<int32_t> {
  typedef int32_t integral_t;
  static const bytes_ref& min() { return mini32(); } 
  static const bytes_ref& max() { return maxi32(); } 
  inline static integral_t integral(integral_t value) { return value; }
  CONSTEXPR static size_t size() { return sizeof(integral_t)+1; }
  static size_t encode(integral_t value, byte_type* out, size_t offset = 0) {
    return encode32(value, out, offset);
  }
  static integral_t decode(const byte_type* in) {
    return decode32(in);
  }
}; // numeric_traits

template<>
struct numeric_traits<int64_t> {
  typedef int64_t integral_t;
  static const bytes_ref& min() { return mini64(); } 
  static const bytes_ref& max() { return maxi64(); } 
  inline static integral_t integral(integral_t value) { return value; }
  CONSTEXPR static size_t size() { return sizeof(integral_t)+1; }
  static size_t encode(integral_t value, byte_type* out, size_t offset = 0) {
    return encode64(value, out, offset);
  }
  static integral_t decode(const byte_type* in) {
    return decode64(in);
  }
}; // numeric_traits

#ifndef FLOAT_T_IS_DOUBLE_T
template<>
struct numeric_traits<float_t> {
  typedef int32_t integral_t;
  static const bytes_ref& ninf() { return nfinf32(); }
  static const bytes_ref& min() { return minf32(); } 
  static const bytes_ref& max() { return maxf32(); } 
  static const bytes_ref& inf() { return finf32(); }
  static float_t floating(integral_t value) { return i32tof(value); }
  static integral_t integral(float_t value) { return ftoi32(value); }
  CONSTEXPR static size_t size() { return sizeof(integral_t)+1; }
  static size_t encode(integral_t value, byte_type* out, size_t offset = 0) {
    return encodef32(value, out, offset);
  }
  static float_t decode(const byte_type* in) {
    return floating(decodef32(in));
  }
}; // numeric_traits
#endif

template<>
struct numeric_traits<double_t> {
  typedef int64_t integral_t;
  static const bytes_ref& ninf() { return ndinf64(); }
  static const bytes_ref& min() { return mind64(); } 
  static const bytes_ref& max() { return maxd64(); } 
  static const bytes_ref& inf() { return dinf64(); }
  static double_t floating(integral_t value) { return i64tod(value); }
  static integral_t integral(double_t value) { return dtoi64(value); }
  CONSTEXPR static size_t size() { return sizeof(integral_t)+1; }
  static size_t encode(integral_t value, byte_type* out, size_t offset = 0) {
    return encoded64(value, out, offset);
  }
  static double_t decode(const byte_type* in) {
    return floating(decoded64(in));
  }
}; // numeric_traits

NS_END // numeric_utils
NS_END // ROOT

#endif // IRESEARCH_NUMERIC_UTILS_H
