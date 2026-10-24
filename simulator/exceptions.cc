#include "exceptions.h"
#include "simulator.h"
#include "trace_manager.h"
#include "hooks_manager.h"
#include "config.hpp"

#include <signal.h>
#include <stdio.h>
#include <execinfo.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdint.h>

/* The backtrace file's path, fixed when the handler is registered: a crash
   during Simulator::release() finds the simulator half destroyed, and asking
   it for the output directory then faulted inside the handler, which ends the
   process with no file at all (the signal is blocked while its handler runs,
   so a second fault is fatal).  The same for everything else here: only
   async-signal-safe calls, nothing that allocates. */
static char g_backtrace_path[4096];

static void write_str(int fd, const char *s)
{
   size_t n = strlen(s);
   while (n)
   {
      ssize_t w = write(fd, s, n);
      if (w <= 0) return;
      s += w; n -= (size_t)w;
   }
}

static void write_dec(int fd, uintptr_t v)
{
   char buf[32]; int i = sizeof(buf);
   buf[--i] = 0;
   do { buf[--i] = (char)('0' + v % 10); v /= 10; } while (v);
   write_str(fd, buf + i);
}

static void exceptionHandler(int sig, siginfo_t *scp, void *ctxt)
{
   static volatile sig_atomic_t in_handler = 0;
   if (in_handler)
   {
      signal(sig, SIG_DFL);
      raise(sig);
      return;
   }
   in_handler = 1;

   write_str(2, "\n[SNIPER] Internal exception: ");
   write_str(2, strsignal(sig));
   write_str(2, ". Access Address = 0x");
   { char hex[2 + 2 * sizeof(void *) + 1]; snprintf(hex, sizeof(hex), "%lx", (unsigned long)(uintptr_t)scp->si_addr); write_str(2, hex); }
   write_str(2, "\n\n");

   const int BACKTRACE_SIZE = 32;
   void *backtrace_buffer[BACKTRACE_SIZE];
   int backtrace_n = backtrace(backtrace_buffer, BACKTRACE_SIZE);

   int fd = g_backtrace_path[0] ? open(g_backtrace_path, O_WRONLY | O_CREAT | O_TRUNC, 0644) : -1;
   if (fd >= 0)
   {
      /* tools/gen_backtrace.py: object name, marker (0: not relocated), the
         frames, then the message; frames 0 and 1 are this handler and libc */
      write_str(fd, "sniper\n0\n");
      for (int i = 2; i < backtrace_n; ++i)
      {
         write_str(fd, " ");
         write_dec(fd, (uintptr_t)backtrace_buffer[i]);
      }
      write_str(fd, "\n");
      write_str(fd, strsignal(sig));
      write_str(fd, ". Access Address = 0x");
      { char hex[2 + 2 * sizeof(void *) + 1]; snprintf(hex, sizeof(hex), "%lx", (unsigned long)(uintptr_t)scp->si_addr); write_str(fd, hex); }
      write_str(fd, "\n\n");
      close(fd);
   }
   else
   {
      write_str(2, "[SNIPER] Backtrace (no output directory for debug_backtrace.out):\n");
      backtrace_symbols_fd(backtrace_buffer + 2, backtrace_n > 2 ? backtrace_n - 2 : 0, 2);
   }

   /* Hide the errors of SIFT writers that lose their reader: the root cause
      is this failure.  Only while the simulator is whole. */
   Simulator *sim = Sim();
   if (sim && sim->isRunning() && sim->getTraceManager())
      sim->getTraceManager()->mark_done();

   // Disable the handler and re-raise the signal
   signal(sig, SIG_DFL);
   raise(sig);
}

static void handleSigUsr1(int sig, siginfo_t *scp, void *ctxt)
{
   Sim()->getHooksManager()->callHooks(HookType::HOOK_SIGUSR1, 0);
}

void registerExceptionHandler()
{
   String path = Sim()->getConfig()->formatOutputFileName("debug_backtrace.out");
   strncpy(g_backtrace_path, path.c_str(), sizeof(g_backtrace_path) - 1);

   /* An alternate stack: a crash from stack exhaustion has none to run the
      handler on otherwise. */
   static char altstack[1 << 16];
   stack_t ss; ss.ss_sp = altstack; ss.ss_size = sizeof(altstack); ss.ss_flags = 0;
   sigaltstack(&ss, NULL);

   struct sigaction sa;
   sigemptyset(&sa.sa_mask);
   sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
   sa.sa_sigaction = exceptionHandler;

   sigaction(SIGFPE, &sa, NULL);
   sigaction(SIGILL, &sa, NULL);
   sigaction(SIGPIPE, &sa, NULL);
   sigaction(SIGSEGV, &sa, NULL);
   sigaction(SIGBUS, &sa, NULL);
   sigaction(SIGABRT, &sa, NULL);   /* a failed assert or LOG_ASSERT_ERROR gets a backtrace too */

   sa.sa_sigaction = handleSigUsr1;
   sigaction(SIGUSR1, &sa, NULL);

   auto signal_to_ignore = Sim()->getCfg()->getInt("general/signals_to_ignore");
   if (signal_to_ignore)
   {
      signal(signal_to_ignore, SIG_IGN);
   }
}
