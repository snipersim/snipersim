#include "log.h"
#include "simulator.h"
#include "config.h"
#include "transport.h"
#include "core.h"
#include "core_manager.h"

#include <vector>
#include <sstream>
#include <algorithm>

// -- outputSummary
//
// Collect output summaries for all the cores and send them to process
// zero. This process then formats the output to look pretty. Only
// process zero writes to the output stream passed in.

static void gatherSummaries(std::vector<String> &summaries)
{
   Config *cfg = Config::getSingleton();
   Transport::Node *global_node = Transport::getSingleton()->getGlobalNode();

   for (UInt32 p = 0; p < cfg->getProcessCount(); p++)
   {
      LOG_PRINT("Collect from process %d", p);

      const Config::CoreList &cl = cfg->getCoreListForProcess(p);

      // signal process to send
      if (p != 0)
         global_node->globalSend(p, &p, sizeof(p));

      // receive summary
      for (UInt32 c = 0; c < cl.size(); c++)
      {
         LOG_PRINT("Collect from core %d", cl[c]);

         Byte *buf;

         buf = global_node->recv();
         assert(*((core_id_t*)buf) == cl[c]);
         delete [] buf;

         buf = global_node->recv();
         summaries[cl[c]] = String((char*)buf);
         delete [] buf;
      }
   }

   for (UInt32 i = 0; i < summaries.size(); i++)
   {
      LOG_ASSERT_ERROR(!summaries[i].empty(), "Summary %d is empty!", i);
   }

   LOG_PRINT("Done collecting.");
}

class Table
{
public:
   Table(unsigned int rows,
         unsigned int cols)
      : m_table(rows * cols)
      , m_rows(rows)
      , m_cols(cols)
   { }

   String& operator () (unsigned int r, unsigned int c)
   {
      return at(r,c);
   }

   String& at(unsigned int r, unsigned int c)
   {
      assert(r < rows() && c < cols());
      return m_table[ r * m_cols + c ];
   }

   const String& at(unsigned int r, unsigned int c) const
   {
      assert(r < rows() && c < cols());
      return m_table[ r * m_cols + c ];
   }

   String flatten() const
   {
      std::vector<unsigned int> col_widths;

      for (unsigned int i = 0; i < cols(); i++)
         col_widths.push_back(0);

      for (unsigned int r = 0; r < rows(); r++)
         for (unsigned int c = 0; c < cols(); c++)
            if (at(r,c).length() > col_widths[c])
               col_widths[c] = at(r,c).length();

      std::stringstream out;

      for (unsigned int r = 0; r < rows(); r++)
      {
         for (unsigned int c = 0; c < cols(); c++)
         {
            out << at(r,c);

            unsigned int padding = col_widths[c] - at(r,c).length();
            out << String(padding, ' ') << " | ";
         }

         out << '\n';
      }

      return out.str().c_str();
   }

   unsigned int rows() const
   {
      return m_rows;
   }

   unsigned int cols() const
   {
      return m_cols;
   }

   typedef std::vector<String>::size_type size_type;

private:
   std::vector<String> m_table;
   unsigned int m_rows;
   unsigned int m_cols;
};

void addRowHeadings(Table &table, const std::vector<String> &summaries)
{
   // take row headings from first summary

   const String &sum = summaries[0];
   String::size_type pos = 0;

   for (Table::size_type i = 1; i < table.rows(); i++)
   {
      String::size_type end = sum.find(':', pos);
      String heading = sum.substr(pos, end-pos);
      pos = sum.find('\n', pos) + 1;
      assert(pos != String::npos);

      table(i,0) = heading;
   }
}

void addColHeadings(Table &table)
{
   for (Table::size_type i = 0; i < Config::getSingleton()->getApplicationCores(); i++)
   {
      std::stringstream heading;
      heading << "Core " << i;
      table(0, i+1) = heading.str().c_str();
   }

   if (Sim()->getConfig()->getSimulationMode() == Config::FULL)
   {
      for (unsigned int i = 0; i < Config::getSingleton()->getProcessCount(); i++)
      {
         unsigned int core_num = Config::getSingleton()->getThreadSpawnerCoreNum(i);
         std::stringstream heading;
         heading << "TS " << i;
         table(0, core_num + 1) = heading.str().c_str();
      }
   }

   table(0, Config::getSingleton()->getMCPCoreNum()+1) = "MCP";
}

void addCoreSummary(Table &table, core_id_t core, const String &summary)
{
   String::size_type pos = summary.find(':')+1;

   for (Table::size_type i = 1; i < table.rows(); i++)
   {
      String::size_type end = summary.find('\n',pos);
      String value = summary.substr(pos, end-pos);
      pos = summary.find(':',pos)+1;
      assert(pos != String::npos);

      table(i, core+1) = value;
   }
}

String formatSummaries(const std::vector<String> &summaries)
{
   // assume that each core outputs the same information
   // assume that output is formatted as "label: value"

   // from first summary, find number of rows needed
   unsigned int rows = count(summaries[0].begin(), summaries[0].end(), '\n');

   // fill in row headings
   Table table(rows+1, Config::getSingleton()->getTotalCores()+1);

   addRowHeadings(table, summaries);
   addColHeadings(table);

   for (unsigned int i = 0; i < summaries.size(); i++)
   {
      addCoreSummary(table, i, summaries[i]);
   }

   return table.flatten();
}

void CoreManager::outputSummary(std::ostream &os)
{
   LOG_PRINT("Starting CoreManager::outputSummary");

   // Note: Using the global_node only works here because the lcp has
   // finished and therefore is no longer waiting on a receive. This
   // is not the most obvious thing, so maybe there should be a
   // cleaner solution.

   Config *cfg = Config::getSingleton();
   Transport::Node *global_node = Transport::getSingleton()->getGlobalNode();

   // wait for my turn...
   if (cfg->getCurrentProcessNum() != 0)
   {
      Byte *buf = global_node->recv();
      assert(*((UInt32*)buf) == cfg->getCurrentProcessNum());
      delete [] buf;
   }

   // send each summary
   const Config::CoreList &cl = cfg->getCoreListForProcess(cfg->getCurrentProcessNum());

   for (UInt32 i = 0; i < cl.size(); i++)
   {
      LOG_PRINT("Output summary core %i", cl[i]);
      std::stringstream ss;
      m_cores[i]->outputSummary(ss);
      global_node->globalSend(0, &cl[i], sizeof(cl[i]));
      global_node->globalSend(0, ss.str().c_str(), ss.str().length()+1);
   }

   // format (only done on proc 0)
   if (cfg->getCurrentProcessNum() != 0)
      return;

   std::vector<String> summaries(cfg->getTotalCores());
   String formatted;

   gatherSummaries(summaries);
   formatted = formatSummaries(summaries);

   os << formatted;

   LOG_PRINT("Finished outputSummary");
}
