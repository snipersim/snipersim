#ifndef FIXED_POINT_H
#define FIXED_POINT_H

#include "fixed_types.h"

#include <cassert>
#include <cstdlib>

// PIN requires special handling when floating point operations are used inside callbacks
// To avoid this, use integer arithmetic with values rescaled by 10k

template<SInt64 one>
class TFixedPoint
{
   private:
      SInt64 m_value;
      static const SInt64 m_one = one;

   public:
      TFixedPoint() : m_value(0) {}
      TFixedPoint(SInt64 i) { this->set_int(i); }

      void set_raw(SInt64 value) { this->m_value = value; }
      void set_int(SInt64 value) { assert((INT64_MAX / this->m_one) > llabs(value)); this->m_value = value * this->m_one; }
      static TFixedPoint from_raw(SInt64 value) { TFixedPoint fp; fp.set_raw(value); return fp; }

      bool operator== (const TFixedPoint& fp) const { return this->m_value == fp.m_value; }
      bool operator== (SInt64 i) const { return this->m_value == i * m_one; }

      TFixedPoint operator+ (const TFixedPoint& fp) const { return TFixedPoint::from_raw(this->m_value + fp.m_value); }
      TFixedPoint operator+ (SInt64 i) const { return TFixedPoint::from_raw(this->m_value + i * m_one); }

      TFixedPoint operator- (const TFixedPoint& fp) const { return TFixedPoint::from_raw(this->m_value - fp.m_value); }
      TFixedPoint operator- (SInt64 i) const { return TFixedPoint::from_raw(this->m_value - i * m_one); }

      TFixedPoint operator* (const TFixedPoint& fp) const { return TFixedPoint::from_raw(this->m_value * fp.m_value / this->m_one); }
      TFixedPoint operator* (SInt64 i) const { return TFixedPoint::from_raw(this->m_value * i); }

      TFixedPoint operator/ (const TFixedPoint& fp) const { return TFixedPoint::from_raw(/*__int128_t*/(fp.m_one) * this->m_value / fp.m_value); }
      TFixedPoint operator/ (SInt64 i) const { return TFixedPoint::from_raw(this->m_value / i); }

      static SInt64 floor(const TFixedPoint fp) { return fp.m_value / fp.m_one; }
};

template <SInt64 one> inline TFixedPoint<one> operator/ (SInt64 i, TFixedPoint<one>& fp) { return TFixedPoint<one>(i) / fp; }

// Default: 16k so mul/div can be done using shift operations
typedef TFixedPoint<__UINT64_C(0x4000)> FixedPoint;

template<SInt64 one>
std::ostream & operator<<(std::ostream &os, const TFixedPoint<one> & f)
{
   os << TFixedPoint<one>::floor((f*one))/(1.0*one);
   return os;
}

#endif // FIXED_POINT_H
