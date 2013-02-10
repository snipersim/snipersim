#ifndef SATURATING_PREDICTOR_H
#define SATURATING_PREDICTOR_H

#include <stdint.h>
#include "fixed_types.h"

// From http://www.josuttis.com/tmplbook/meta/pow3.hpp.html
template <int N> class Pow2
{
public:
   enum { pow = 2 * Pow2<N-1>::pow };
};

template <> class Pow2<0>
{
public:
   enum { pow = 1 };
};

#define SAT_PRED_DEBUG 0

template <unsigned n>
class SaturatingPredictor {

public:

   SaturatingPredictor(UInt32 initial_value)
   {
      m_counter = initial_value;
   }

   bool predict()
   {
      return (m_counter >= 0);
   }

   void reset(bool prediction = 0)
   {
      if (prediction)
      {
         // Make this counter favor taken
         m_counter = Pow2<n-1>::pow - 1;
      }
      else
      {
         // Make this counter favor not-taken
         m_counter = -Pow2<n-1>::pow;
      }
   }

   // update
   // true - branch taken
   // false - branch not-taken
   void update(bool actual)
   {
      if (actual)
      {
         // Move towards taken
         ++(*this);
      }
      else
      {
         // Move towards not-taken
         --(*this);
      }
   }

   SaturatingPredictor& operator++()
   {
#if SAT_PRED_DEBUG
      cout << "operator++ called! Old val = " << (int) m_counter;
#endif
      // Maximum signed value for n bits is (2^(n-1)-1)
      if (m_counter != (Pow2<n-1>::pow - 1))
      {
         ++m_counter;
      }
#if SAT_PRED_DEBUG
      cout << " New val = " << (int) m_counter << endl;
#endif
      return *this;
   }

   SaturatingPredictor& operator--()
   {
#if SAT_PRED_DEBUG
      cout << "operator-- called! Old val = " << (int) m_counter;
#endif
      // Minimum signed value for n bits is -(2^(n-1))
      if (m_counter != (-Pow2<n-1>::pow))
      {
         --m_counter;
      }
#if SAT_PRED_DEBUG
      cout << " New val = " << (int) m_counter << endl;
#endif
      return *this;
   }

private:

   int8_t m_counter;

};

#endif /* SATURATING_PREDICTOR_H */
