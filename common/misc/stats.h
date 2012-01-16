#pragma once

#include "simulator.h"
#include "itostr.h"
#include <strings.h>

class StatsMetricBase
{
   public:
      String objectName;
      UInt32 index;
      String metricName;
      StatsMetricBase(String _objectName, UInt32 _index, String _metricName) :
         objectName(_objectName), index(_index), metricName(_metricName)
      {}
      virtual String recordMetric() = 0;
};

template <class T> class StatsMetric : public StatsMetricBase
{
   public:
      T *metric;
      StatsMetric(String _objectName, UInt32 _index, String _metricName, T *_metric) :
         StatsMetricBase(_objectName, _index, _metricName), metric(_metric)
      {}
      virtual String recordMetric()
      {
         return itostr(*metric);
      }
};

typedef String (*StatsCallback)(String objectName, UInt32 index, String metricName, void *arg);
class StatsMetricCallback : public StatsMetricBase
{
   public:
      StatsCallback func;
      void *arg;
      StatsMetricCallback(String _objectName, UInt32 _index, String _metricName, StatsCallback _func, void *_arg) :
         StatsMetricBase(_objectName, _index, _metricName), func(_func), arg(_arg)
      {}
      virtual String recordMetric()
      {
         return func(objectName, index, metricName, arg);
      }
};


class StatsManager {
   public:
      StatsManager();
      ~StatsManager();
      void registerMetric(StatsMetricBase *metric);
      void recordStats(String prefix, String fileName = "");
      void recordStats(String prefix, FILE *fp);
      StatsMetricBase *getMetricObject(String objectName, UInt32 index, String metricName);
      template <class T> T * getMetric(String objectName, UInt32 index, String metricName);
   private:
      FILE *m_fp;
      std::vector<StatsMetricBase *> m_objects;
};

template <class T> void registerStatsMetric(String objectName, UInt32 index, String metricName, T *metric)
{
   Sim()->getStatsManager()->registerMetric(new StatsMetric<T>(objectName, index, metricName, metric));
}

template <class T> T *
StatsManager::getMetric(String objectName, UInt32 index, String metricName)
{
   StatsMetricBase* metric = getMetricObject(objectName, index, metricName);
   if (metric) {
      StatsMetric<T>* m = dynamic_cast<StatsMetric<T>*>(metric);
      LOG_ASSERT_ERROR(m, "Casting stats metric %s[%u].%s to invalid type", objectName.c_str(), index, metricName.c_str());
      return m->metric;
   } else
      return NULL;
}



class StatHist {
  private:
    static const int HIST_MAX = 20;
    unsigned long n, s, s2, min, max;
    unsigned long hist[HIST_MAX];
    char dummy[64];
  public:
    StatHist() : n(0), s(0), s2(0), min(0), max(0) { bzero(hist, sizeof(hist)); }
    StatHist & operator += (StatHist & stat);
    void update(unsigned long v);
    void print();
};
