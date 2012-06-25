#ifndef __MOVING_AVERAGE_H__
#define __MOVING_AVERAGE_H__

#include <vector>
#include <cmath>

#include "fixed_types.h"
#include "modulo_num.h"
#include "log.h"

template <class T>
class MovingAverage
{
   public:
      enum AvgType_t
      {
         ARITHMETIC_MEAN = 0,
         GEOMETRIC_MEAN,
         MEDIAN,
         NUM_AVG_TYPES
      };

      MovingAverage(UInt32 max_window_size);
      virtual ~MovingAverage();

      static AvgType_t parseAvgType(String avg_type)
      {
         if (avg_type == "arithmetic_mean")
            return ARITHMETIC_MEAN;
         else if (avg_type == "geometric_mean")
            return GEOMETRIC_MEAN;
         else if (avg_type == "median")
            return MEDIAN;
         else
         {
            LOG_PRINT_ERROR("Unsupported Average Type: %s", avg_type.c_str());
            return (AvgType_t) -1;
         }
      }

      static MovingAverage<T>* createAvgType(AvgType_t avg_type, UInt32 max_window_size);

      virtual void update(T next_num) = 0;
      virtual T compute() const = 0;
      virtual T compute(T next_num) { update(next_num); return compute(); }

   protected:
      std::vector<T> m_num_list;
      UInt32 m_max_window_size;
      ModuloNum m_curr_window_front;
      ModuloNum m_curr_window_back;

      void addToWindow(T next_num);

      void printElements()
      {
         printf("Elements: ");
         ModuloNum curr_num(m_curr_window_front);
         for ( ; curr_num != m_curr_window_back; curr_num = curr_num+1)
            printf("%f ", (float) m_num_list[curr_num.getValue()]);
         printf("\n");
      }
};

template<class T>
MovingAverage<T>::MovingAverage(UInt32 max_window_size)
   : m_max_window_size(max_window_size)
{
   m_num_list.resize(m_max_window_size + 1);
   m_curr_window_front = ModuloNum(m_max_window_size + 1);
   m_curr_window_back = ModuloNum(m_max_window_size + 1);
}

template<class T>
MovingAverage<T>::~MovingAverage()
{ }

template<class T>
void MovingAverage<T>::addToWindow(T next_num)
{
   m_num_list[m_curr_window_back.getValue()] = next_num;
   m_curr_window_back = m_curr_window_back + 1;
   if (m_curr_window_back == m_curr_window_front)
   {
      m_curr_window_front = m_curr_window_front + 1;
   }
}

template <class T>
class MovingArithmeticMean : public MovingAverage<T>
{
   private:
      T sum;

   public:
      MovingArithmeticMean(UInt32 max_window_size):
         MovingAverage<T>(max_window_size),
         sum(SubsecondTime::Zero())
      { }

      void update(T next_num)
      {
         UInt32 curr_window_size = (this->m_curr_window_back - this->m_curr_window_front).getValue();
         if (curr_window_size == this->m_max_window_size)
         {
            curr_window_size --;
            sum -= this->m_num_list[this->m_curr_window_front.getValue()];
         }
         sum += next_num;
         curr_window_size ++;

         this->addToWindow(next_num);
      }

      T compute() const
      {
         UInt32 curr_window_size = (this->m_curr_window_back - this->m_curr_window_front).getValue(); // recompute
         return sum / curr_window_size;
      }
};

template <class T>
class MovingGeometricMean : public MovingAverage<T>
{
   public:
      MovingGeometricMean(UInt32 max_window_size):
         MovingAverage<T>(max_window_size)
      { }

      void update(T next_num)
      {
         this->addToWindow(next_num);
      }

      T compute() const
      {
         UInt32 curr_window_size = (this->m_curr_window_back - this->m_curr_window_front).getValue();
         T geometric_mean = SubsecondTime::Zero();
         geometric_mean.setInternalDataForced(1);

         ModuloNum curr_num(this->m_curr_window_front);
         for ( ; curr_num != this->m_curr_window_back; curr_num = curr_num+1)
            geometric_mean *= this->m_num_list[curr_num.getValue()];

         //FIXME Use of internal data
         SubsecondTime ret;
         ret.setInternalDataForced(pow((double) geometric_mean.getInternalDataForced(), (double) 1.0/curr_window_size));
         return ret;
      }
};

template <class T>
class MovingMedian : public MovingAverage<T>
{
   public:
      MovingMedian(UInt32 max_window_size):
         MovingAverage<T>(max_window_size)
      { }

      void update(T next_num)
      {
         this->addToWindow(next_num);
      }

      T compute() const
      {
         UInt32 curr_window_size = (this->m_curr_window_back - this->m_curr_window_front).getValue();
         UInt32 median_index = (this->m_curr_window_front + (curr_window_size / 2)).getValue();
         return this->m_num_list[median_index];
      }
};

template <class T>
MovingAverage<T>* MovingAverage<T>::createAvgType(AvgType_t avg_type, UInt32 max_window_size)
{
   switch(avg_type)
   {
      case ARITHMETIC_MEAN:
         return new MovingArithmeticMean<T>(max_window_size);

      case GEOMETRIC_MEAN:
         return new MovingGeometricMean<T>(max_window_size);

      case MEDIAN:
         return new MovingMedian<T>(max_window_size);

      default:
         LOG_PRINT_ERROR("Unsupported Average Type: %u", (UInt32) avg_type);
         return NULL;
   }
}

#endif /* __MOVING_AVERAGE_H__ */
