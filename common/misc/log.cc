#include <unistd.h>
#include <sys/time.h>
#include <sys/syscall.h>
#include <stdarg.h>
#include <string.h>
#include <signal.h>

#include "log.h"
#include "config.h"
#include "simulator.h"
#include "core_manager.h"
#include "config.hpp"

// When debugging, it helps to be able to attach to the thread you would like to investigate directly,
// instead of running the program from the beginning in GDB.
//#define LOG_SIGSTOP_ON_ERROR

Log *Log::_singleton;

const size_t Log::MODULE_LENGTH;

static String formatFileName(const char* s)
{
   return Sim()->getConfig()->formatOutputFileName(s);
}

Log::Log(Config &config)
   : _coreCount(config.getTotalCores())
   , _startTime(0)
{
   initFileDescriptors();
   getEnabledModules();
   getDisabledModules();

   _loggingEnabled = initIsLoggingEnabled();
   _anyLoggingEnabled = (!_enabledModules.empty()) || _loggingEnabled;

   assert(_singleton == NULL);
   _singleton = this;
}

Log::~Log()
{
   _singleton = NULL;

   for (core_id_t i = 0; i < _coreCount; i++)
   {
      if (_coreFiles[i])
         fclose(_coreFiles[i]);
      if (_simFiles[i])
         fclose(_simFiles[i]);
   }

   delete [] _coreLocks;
   delete [] _simLocks;
   delete [] _coreFiles;
   delete [] _simFiles;

   if (_systemFile)
      fclose(_systemFile);
}

Log* Log::getSingleton()
{
   assert(_singleton);
   return _singleton;
}

bool Log::isEnabled(const char* module)
{
   // either the module is specifically enabled, or all logging is
   // enabled and this one isn't disabled
   return _anyLoggingEnabled &&
      (_enabledModules.find(module) != _enabledModules.end()
       || (_loggingEnabled && _disabledModules.find(module) == _disabledModules.end())
      );
}

void Log::initFileDescriptors()
{
   _coreFiles = new FILE* [_coreCount];
   _simFiles = new FILE* [_coreCount];

   for (core_id_t i = 0; i < _coreCount; i++)
   {
      _coreFiles[i] = NULL;
      _simFiles[i] = NULL;
   }

   _coreLocks = new Lock [_coreCount];
   _simLocks = new Lock [_coreCount];

   _systemFile = NULL;
}

void Log::parseModules(std::set<String> &mods, String list)
{
   String delimiters = " ";

   String::size_type lastPos = list.find_first_not_of(delimiters, 0);
   String::size_type pos     = list.find_first_of(delimiters, lastPos);

   std::set<String> unformatted;

   while (String::npos != pos || String::npos != lastPos)
   {
      unformatted.insert(list.substr(lastPos, pos - lastPos));
      lastPos = list.find_first_not_of(delimiters, pos);
      pos = list.find_first_of(delimiters, lastPos);
   }

   for (std::set<String>::iterator it = unformatted.begin();
        it != unformatted.end();
        it++)
   {
      String formatted;

      for (unsigned int i = 0; i < std::min(MODULE_LENGTH, it->length()); i++)
      {
         formatted.push_back((*it)[i]);
      }

      for (unsigned int i = formatted.length(); i < MODULE_LENGTH; i++)
      {
         formatted.push_back(' ');
      }

      assert(formatted.length() == MODULE_LENGTH);
      mods.insert(formatted);
   }
}

void Log::getDisabledModules()
{
   try
   {
      String disabledModules = Sim()->getCfg()->getString("log/disabled_modules");
      parseModules(_disabledModules, disabledModules);
   }
   catch (...)
   {
      // FIXME: is log initialized at this point?
      LOG_PRINT_ERROR("Exception while reading disabled modules.");
   }
}

void Log::getEnabledModules()
{
   try
   {
      String enabledModules = Sim()->getCfg()->getString("log/enabled_modules");
      parseModules(_enabledModules, enabledModules);
   }
   catch (...)
   {
      // FIXME: is log initialized at this point?
      LOG_PRINT_ERROR("Exception while reading enabled modules.");
   }
}

bool Log::initIsLoggingEnabled()
{
   try
   {
      return Sim()->getCfg()->getBool("log/enabled");
   }
   catch (...)
   {
      assert(false);
      return false;
   }
}

UInt64 Log::getTimestamp()
{
   timeval t;
   gettimeofday(&t, NULL);
   UInt64 time = (((UInt64)t.tv_sec) * 1000000 + t.tv_usec);
   if (_startTime == 0) _startTime = time;
   return time - _startTime;
}

void Log::discoverCore(core_id_t *core_id, bool *sim_thread)
{
   CoreManager *core_manager;

   if (!Sim() || !(core_manager = Sim()->getCoreManager()))
   {

      *core_id = INVALID_CORE_ID;
      *sim_thread = false;
      return;
   }
   else
   {
      *core_id = core_manager->getCurrentCoreID();
      *sim_thread = core_manager->amiSimThread();
   }
}

void Log::getFile(core_id_t core_id, bool sim_thread, FILE **file, Lock **lock)
{
   // we use on-demand file allocation to prevent contention between
   // processes for files

   *file = NULL;
   *lock = NULL;

   if (core_id == INVALID_CORE_ID)
   {
      if (_systemFile == NULL)
      {
         char filename[256];
         sprintf(filename, "system.log");
         _systemFile = fopen(formatFileName(filename).c_str(), "w");
         assert(_systemFile != NULL);
      }

      *file = _systemFile;
      *lock = &_systemLock;
   }
   else if (sim_thread)
   {
      // sim thread file
      if (_simFiles[core_id] == NULL)
      {
         assert(core_id < _coreCount);
         char filename[256];
         sprintf(filename, "sim_%u.log", core_id);
         _simFiles[core_id] = fopen(formatFileName(filename).c_str(), "w");
         assert(_simFiles[core_id] != NULL);
      }

      *file = _simFiles[core_id];
      *lock = &_simLocks[core_id];
   }
   else
   {
      // core file
      if (_coreFiles[core_id] == NULL)
      {
         assert(core_id < _coreCount);
         char filename[256];
         sprintf(filename, "app_%u.log", core_id);
         _coreFiles[core_id] = fopen(formatFileName(filename).c_str(), "w");
         assert(_coreFiles[core_id] != NULL);
      }

      // Core file
      *file = _coreFiles[core_id];
      *lock = &_coreLocks[core_id];
   }
}

String Log::getModule(const char *filename)
{
   // TODO: Give each thread a _module map to cache entries. (Hash table?)

   // ScopedLock sl(_modules_lock);
   // map<const char*, String>::const_iterator it = _modules.find(filename);

   // if (it != _modules.end())
   // {
   //    return it->second;
   // }
   // else
   // {
      // build module string
      String mod;

      // find actual file name ...
      const char *ptr = strrchr(filename, '/');
      if (ptr != NULL)
         filename = ptr + 1;

      for (UInt32 i = 0; i < MODULE_LENGTH && filename[i] != '\0'; i++)
         mod.push_back(filename[i]);

      while (mod.length() < MODULE_LENGTH)
         mod.push_back(' ');

   //   pair<const char*, String> p(filename, mod);
   //   _modules.insert(p);

      return mod;
   // }
}

void Log::log(ErrorState err, const char* source_file, SInt32 source_line, const char *format, ...)
{
   core_id_t core_id;
   bool sim_thread;
   discoverCore(&core_id, &sim_thread);

   FILE *file;
   Lock *lock;

   getFile(core_id, sim_thread, &file, &lock);
   int tid = syscall(__NR_gettid);


   char message[512];
   char *p = message;

   // This is ugly, but it just prints the time stamp, core number, source file/line
   if (core_id != INVALID_CORE_ID) // valid core id
      p += sprintf(p, "%-10llu [%5d]  [%2i]%s[%s:%4d]  ", (long long unsigned int) getTimestamp(), tid, core_id, (sim_thread ? "* " : "  "), source_file, source_line);
   else // who knows
      p += sprintf(p, "%-10llu [%5d]  [  ]  [%s:%4d]  ", (long long unsigned int) getTimestamp(), tid, source_file, source_line);

   switch (err)
   {
   case None:
   default:
      break;

   case Warning:
      p += sprintf(p, "*WARNING* ");
      break;

   case Error:
      p += sprintf(p, "*ERROR* ");
      break;
   };

   va_list args;
   va_start(args, format);
   p += vsprintf(p, format, args);
   va_end(args);

   p += sprintf(p, "\n");

   lock->acquire();

   fputs(message, file);
   fflush(file);

   lock->release();

   switch (err)
   {
   case Error:
      fflush(NULL);
      fputs(message, stderr);
#ifndef LOG_SIGSTOP_ON_ERROR
      abort();
#else
      while (1)
      {
         raise(SIGSTOP);
      }
#endif
      break;

   case Warning:
      fputs(message, stderr);
      break;

   case None:
   default:
      break;
   }
}
