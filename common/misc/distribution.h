#ifndef __DISTRIBUTION_H
#define __DISTRIBUTION_H

#include "fixed_types.h"
#include "subsecond_time.h"
#include <tr1/random>

class FloatDistribution
{
public:
   virtual ~FloatDistribution() {}
   virtual double next() = 0;
};

class NormalFloatDistribution : public FloatDistribution
{
public:
   NormalFloatDistribution(double mean, double stddev, unsigned seed = 1000)
      : generator(seed)
      , distribution(mean, stddev)
   {
   }
   double next()
   {
      return distribution(generator);
   }
private:
   //The following code is for C++ 2011 only (-std-c++11). C++0x in GCC 4.4 does not support this, but it does support the TR1 implementation
   //std::default_random_engine generator;
   //std::normal_distribution<double> distribution;
   std::tr1::ranlux64_base_01 generator;
   std::tr1::normal_distribution<double> distribution;
};

class TimeDistribution
{
public:
   virtual ~TimeDistribution() {}
   virtual SubsecondTime next() = 0;
};

class ConstantTimeDistribution : public TimeDistribution
{
public:
   ConstantTimeDistribution(SubsecondTime constant) : value(constant) {}

   SubsecondTime next()
   {
      return value;
   }
private:
   SubsecondTime value;
};

class NormalTimeDistribution : public TimeDistribution
{
public:
   NormalTimeDistribution(SubsecondTime mean, SubsecondTime stddev, unsigned seed = 1000)
      : normal_dist(mean.getFS(), stddev.getFS())
   {
   }
   SubsecondTime next() {
      double normal = normal_dist.next();
      SubsecondTime value = SubsecondTime::FS(normal);
      return value;
   }
private:
   NormalFloatDistribution normal_dist;
};
#endif /* __DISTRIBUTION_H */
