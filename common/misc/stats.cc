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
#include <cstring>
#include <db.h>
#include <zlib.h>

template <> UInt64 makeStatsValue<UInt64>(UInt64 t) { return t; }
template <> UInt64 makeStatsValue<SubsecondTime>(SubsecondTime t) { return t.getFS(); }
template <> UInt64 makeStatsValue<ComponentTime>(ComponentTime t) { return t.getElapsedTime().getFS(); }

StatsManager::StatsManager()
   : m_keyid(0)
   , m_prefixnum(0)
   , m_db(NULL)
{
}

StatsManager::~StatsManager()
{
   if (m_db)
      m_db->close(m_db, 0);
}

void
StatsManager::init()
{
   String filename = Sim()->getConfig()->formatOutputFileName("sim.stats.db");
   int ret;

   ret = db_create(&m_db, NULL, 0);
   LOG_ASSERT_ERROR(ret == 0, "Cannot create DB");

   ret = m_db->open(m_db, NULL, filename.c_str(), NULL, DB_HASH, DB_CREATE | DB_TRUNCATE, 0);
   LOG_ASSERT_ERROR(ret == 0, "Cannot create DB");

   recordStatsBase();
}

class StatStream
{
   private:
      std::stringstream value;
      z_stream zstream;
      static const size_t chunksize = 64*1024;
      static const int level = 9;
      char buffer[chunksize];

      void write(const char* data, size_t size)
      {
         zstream.next_in = (Bytef*)data;
         zstream.avail_in = size;
         doCompress(false);
      }
      void doCompress(bool finish)
      {
         int ret;
         do
         {
            zstream.next_out = (Bytef*)buffer;
            zstream.avail_out = chunksize;
            ret = deflate(&zstream, finish ? Z_FINISH : Z_NO_FLUSH);
            assert(ret != Z_STREAM_ERROR);
            value.write(buffer, chunksize - zstream.avail_out);
         }
         while(zstream.avail_out == 0);
         assert(zstream.avail_in == 0);     /* all input will be used */
         if (finish)
            assert(ret == Z_STREAM_END);
      }

   public:
      StatStream()
      {
         zstream.zalloc = Z_NULL;
         zstream.zfree = Z_NULL;
         zstream.opaque = Z_NULL;
         int ret = deflateInit(&zstream, level);
         assert(ret == Z_OK);
      }
      void writeInt32(SInt32 value)
      {
         this->write((const char*)&value, sizeof(value));
      }
      void writeUInt64(UInt64 value)
      {
         this->write((const char*)&value, sizeof(value));
      }
      void writeString(std::string value)
      {
         this->writeInt32(value.size());
         this->write(value.c_str(), value.size());
      }
      void writeString(String value)
      {
         this->writeInt32(value.size());
         this->write(value.c_str(), value.size());
      }
      std::string str()
      {
         doCompress(true);
         return value.str();
      }
};

void
StatsManager::recordStats(String prefix)
{
   LOG_ASSERT_ERROR(m_db, "m_db not yet set up !?");

   // Allow lazily-maintained statistics to be updated
   Sim()->getHooksManager()->callHooks(HookType::HOOK_PRE_STAT_WRITE, 0);

   StatStream data;
   data.writeInt32(m_prefixnum++);

   for(StatsObjectList::iterator it1 = m_objects.begin(); it1 != m_objects.end(); ++it1)
   {
      for (StatsMetricList::iterator it2 = it1->second.begin(); it2 != it1->second.end(); ++it2)
      {
         data.writeInt32(it2->second.first); // Metric ID
         for(StatsIndexList::iterator it3 = it2->second.second.begin(); it3 != it2->second.second.end(); ++it3)
         {
            if (!it3->second->isDefault())
            {
               data.writeInt32(it3->second->index);
               data.writeUInt64(it3->second->recordMetric());
            }
         }
         data.writeInt32(-12345); // Last
      }
   }

   db_write(std::string("d") + prefix.c_str(), data.str());
}

void
StatsManager::recordStatsBase()
{
   StatStream s;

   // Record all possible parameters without any actual statistics
   for(StatsObjectList::iterator it1 = m_objects.begin(); it1 != m_objects.end(); ++it1)
   {
      for (StatsMetricList::iterator it2 = it1->second.begin(); it2 != it1->second.end(); ++it2)
      {
         s.writeInt32(it2->second.first);    // Metric ID
         s.writeString(it1->first);          // Object name
         s.writeString(it2->first);          // Metric name
      }
   }

   db_write(std::string("k"), s.str());
}

void
StatsManager::db_write(std::string key, std::string data)
{
   DBT _key, _data;
   memset(&_key, 0, sizeof(DBT));
   memset(&_data, 0, sizeof(DBT));

   _key.data = (void*)key.c_str();
   _key.size = key.size();

   _data.data = (void*)data.c_str();
   _data.size = data.size();

   int res = m_db->put(m_db, NULL, &_key, &_data, DB_OVERWRITE_DUP);
   LOG_ASSERT_ERROR(res == 0, "Error when writing stats");
   m_db->sync(m_db, 0);
}

void
StatsManager::registerMetric(StatsMetricBase *metric)
{
   std::string _objectName(metric->objectName.c_str()), _metricName(metric->metricName.c_str());
   m_objects[_objectName][_metricName].second[metric->index] = metric;
   if (m_objects[_objectName][_metricName].first == 0)
   {
      m_objects[_objectName][_metricName].first = ++m_keyid;
      if (m_db)
      {
         // Metrics name record was already written, but a new metric was registered afterwards: write a new record
         recordStatsBase();
      }
   }
}

StatsMetricBase *
StatsManager::getMetricObject(String objectName, UInt32 index, String metricName)
{
   std::string _objectName(objectName.c_str()), _metricName(metricName.c_str());
   if (m_objects.count(_objectName) == 0)
      return NULL;
   if (m_objects[_objectName].count(_metricName) == 0)
      return NULL;
   if (m_objects[_objectName][_metricName].second.count(index) == 0)
      return NULL;
   return m_objects[_objectName][_metricName].second[index];
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
