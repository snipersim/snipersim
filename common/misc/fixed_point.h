#ifndef FIXED_POINT_H
#define FIXED_POINT_H

#include "fixed_types.h"

#include <cassert>
#include <cstdlib>

// PIN requires special handling when floating point operations are used inside callbacks
// To avoid this, use integer arithmetic with values rescaled by 10k

class FixedPoint {
   private:
      SInt64 m_value;
      static const SInt64 m_one = 10000;

   public:
      FixedPoint() : m_value(0) {}
      FixedPoint(SInt64 i) { this->set_int(i); }

      void set_raw(SInt64 value) { this->m_value = value; }
      void set_int(SInt64 value) { assert(INT64_MAX / this->m_one > abs(value)); this->m_value = value * this->m_one; }
      static FixedPoint from_raw(SInt64 value) { FixedPoint fp; fp.set_raw(value); return fp; }

      bool operator== (const FixedPoint& fp) const { return this->m_value == fp.m_value; }
      bool operator== (SInt64 i) const { return this->m_value == i * m_one; }

      FixedPoint operator+ (const FixedPoint& fp) const { return FixedPoint::from_raw(this->m_value + fp.m_value); }
      FixedPoint operator+ (SInt64 i) const { return FixedPoint::from_raw(this->m_value + i * m_one); }

      FixedPoint operator- (const FixedPoint& fp) const { return FixedPoint::from_raw(this->m_value - fp.m_value); }
      FixedPoint operator- (SInt64 i) const { return FixedPoint::from_raw(this->m_value - i * m_one); }

      FixedPoint operator* (const FixedPoint& fp) const { return FixedPoint::from_raw(this->m_value * fp.m_value / this->m_one); }
      FixedPoint operator* (SInt64 i) const { return FixedPoint::from_raw(this->m_value * i); }

      FixedPoint operator/ (const FixedPoint& fp) const { return FixedPoint::from_raw(__int128_t(this->m_one) * this->m_value / fp.m_value); }
      FixedPoint operator/ (SInt64 i) const { return FixedPoint::from_raw(this->m_value / i); }

      static SInt64 floor(const FixedPoint fp) { return fp.m_value / fp.m_one; }
};

inline FixedPoint operator/ (SInt64 i, FixedPoint& fp) { return FixedPoint(i) / fp; }

#endif // FIXED_POINT_H
