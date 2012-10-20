#include "stats.h"
#include "simulator.h"
#include "hooks_manager.h"
#include "utils.h"
#include "itostr.h"

#include <math.h>
#include <stdio.h>
#include <sstream>
#include <unordered_set>
#include <string>

StatsManager::StatsManager()
   : m_fp(NULL)
{
}

StatsManager::~StatsManager()
{
   if (m_fp)
      fclose(m_fp);
}

void
StatsManager::init()
{
   String filename = "sim.stats.delta";
   filename = Sim()->getConfig()->formatOutputFileName(filename);
   m_fp = fopen(filename.c_str(), "w");
   LOG_ASSERT_ERROR(m_fp, "Cannot open %s for writing", filename.c_str());

   recordStatsBase();
}

void
StatsManager::recordStats(String prefix)
{
   LOG_ASSERT_ERROR(m_fp, "m_fp not yet set up !?");

   // Allow lazily-maintained statistics to be updated
   Sim()->getHooksManager()->callHooks(HookType::HOOK_PRE_STAT_WRITE, 0);

   for(StatsObjectList::iterator it1 = m_objects.begin(); it1 != m_objects.end(); ++it1)
   {
      for (StatsMetricList::iterator it2 = it1->second.begin(); it2 != it1->second.end(); ++it2)
      {
         for(StatsIndexList::iterator it3 = it2->second.begin(); it3 != it2->second.end(); ++it3)
         {
            if (!it3->second->isDefault())
            {
               fprintf(m_fp, "%s.%s[%u].%s %s\n", prefix.c_str(), it3->second->objectName.c_str(), it3->second->index,
                                                  it3->second->metricName.c_str(), it3->second->recordMetric().c_str());
            }
         }
      }
   }
   fflush(m_fp);
}

void
StatsManager::recordStatsBase()
{
   // Print out all possible parameters without any actual statistics
   String filename = "sim.stats.base";
   filename = Sim()->getConfig()->formatOutputFileName(filename);
   FILE *fp = fopen(filename.c_str(), "w");
   LOG_ASSERT_ERROR(fp, "Cannot open %s for writing", filename.c_str());

   for(StatsObjectList::iterator it1 = m_objects.begin(); it1 != m_objects.end(); ++it1)
   {
      for (StatsMetricList::iterator it2 = it1->second.begin(); it2 != it1->second.end(); ++it2)
      {
         fprintf(fp, "%s[].%s\n", it1->first.c_str(), it2->first.c_str());
      }
   }
   fflush(fp);
}

void
StatsManager::registerMetric(StatsMetricBase *metric)
{
   std::string _objectName(metric->objectName.c_str()), _metricName(metric->metricName.c_str());
   m_objects[_objectName][_metricName][metric->index] = metric;
}

StatsMetricBase *
StatsManager::getMetricObject(String objectName, UInt32 index, String metricName)
{
   std::string _objectName(objectName.c_str()), _metricName(metricName.c_str());
   if (m_objects.count(_objectName) == 0)
      return NULL;
   if (m_objects[_objectName].count(_metricName) == 0)
      return NULL;
   if (m_objects[_objectName][_metricName].count(index) == 0)
      return NULL;
   return m_objects[_objectName][_metricName][index];
}


StatHist &
StatHist::operator += (StatHist & stat)
{
   if (n == 0) { min = stat.min; max = stat.max; }
   n += stat.n;
   s += stat.s;
   s2 += stat.s2;
   if (stat.n && stat.min < min) min = stat.min;
   if (stat.n && stat.max > max) max = stat.max;
   for(int i = 0; i < HIST_MAX; ++i)
      hist[i] += stat.hist[i];
   return *this;
}

void
StatHist::update(unsigned long v)
{
   if (n == 0) {
      min = v;
      max = v;
   }
   n++;
   s += v;
   s2 += v*v;
   if (v < min) min = v;
   if (v > max) max = v;
   int bin = floorLog2(v) + 1;
   if (bin >= HIST_MAX) bin = HIST_MAX - 1;
      hist[bin]++;
}

void
StatHist::print()
{
   printf("n(%lu), avg(%.2f), std(%.2f), min(%lu), max(%lu), hist(%lu",
      n, n ? s/float(n) : 0, n ? sqrt((s2/n - (s/n)*(s/n))*n/float(n-1)) : 0, min, max, hist[0]);
   for(int i = 1; i < HIST_MAX; ++i)
      printf(",%lu", hist[i]);
   printf(")\n");
}
