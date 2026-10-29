/* timers: the guest's timers in the guest's time.
 *
 * alarm, setitimer, the POSIX timers and timerfd are kept by the front end
 * (sift/guest_time.h GuestTimers) and fire when the guest's time -- the
 * simulator's, or the one-IPC model's without one -- passes their expiry;
 * the kernel would time them in host seconds.  Each expiry below is measured
 * with clock_gettime, the guest's clock.  testing/test_qemu.py user-timers
 * and test_apps' timers run it, to a file and live.  Prints
 *
 *   timers: <name> want=<ns> got=<ns> ret=<result>
 *
 * per timer ("-spin": the expiry was caught while the thread computed, at a
 * look the front end makes every so often), and "timers: done". */
#define _GNU_SOURCE
#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <sys/timerfd.h>
#include <sys/times.h>
#include <time.h>
#include <unistd.h>

static uint64_t now_ns(void)
{
   struct timespec ts;
   clock_gettime(CLOCK_MONOTONIC, &ts);
   return (uint64_t)ts.tv_sec * 1000000000u + (uint64_t)ts.tv_nsec;
}

static int bad = 0;
static void report(const char *name, uint64_t want, uint64_t t0, long ret, long want_ret)
{
   const uint64_t got = now_ns() - t0;
   printf("timers: %s want=%llu got=%llu ret=%ld\n", name, (unsigned long long)want, (unsigned long long)got, ret);
   if (ret != want_ret)
   {
      printf("timers: %s returned %ld, not %ld\n", name, ret, want_ret);
      bad = 1;
   }
}

static int on(const char *name)
{
   const char *only = getenv("TIMERS_ONLY");
   if (!only)
      return 1;
   const size_t n = strlen(name);
   for (const char *p = only; (p = strstr(p, name)); p += n)
      if ((p == only || p[-1] == ',') && (p[n] == ',' || !p[n]))
         return 1;
   return 0;
}

static volatile sig_atomic_t alarms = 0;
static void on_alarm(int sig) { (void)sig; alarms++; }

int main(void)
{
   uint64_t t0;
   struct itimerval it;
   struct itimerspec its;
   volatile uint64_t x = 1;
   setvbuf(stdout, NULL, _IOLBF, 0);
   struct sigaction sa;
   memset(&sa, 0, sizeof(sa));
   sa.sa_handler = on_alarm;
   sigaction(SIGALRM, &sa, NULL);

   if (on("itimer-spin"))
   {
      /* a one-shot ITIMER_REAL, caught while computing */
      alarms = 0;
      t0 = now_ns();
      it = (struct itimerval){ { 0, 0 }, { 0, 2000 } };
      setitimer(ITIMER_REAL, &it, NULL);
      while (!alarms && now_ns() - t0 < 1000000000u)
         x++;
      report("itimer-spin", 2000000, t0, alarms, 1);
   }

   if (on("itimer-pause"))
   {
      /* a periodic ITIMER_REAL, five times, in pause() */
      alarms = 0;
      t0 = now_ns();
      it = (struct itimerval){ { 0, 1000 }, { 0, 1000 } };
      setitimer(ITIMER_REAL, &it, NULL);
      while (alarms < 5)
         pause();
      it = (struct itimerval){ { 0, 0 }, { 0, 0 } };
      setitimer(ITIMER_REAL, &it, NULL);
      report("itimer-pause", 5000000, t0, alarms, 5);
   }

   if (on("alarm-left"))
   {
      /* alarm's seconds left */
      alarm(10);
      const unsigned left = alarm(0);
      printf("timers: alarm-left %u\n", left);
      if (left != 10)
         bad = 1;
   }

   if (on("posix-timer"))
   {
      /* a POSIX timer on CLOCK_MONOTONIC: its signal, with its value */
      sigset_t set;
      sigemptyset(&set);
      sigaddset(&set, SIGRTMIN);
      sigprocmask(SIG_BLOCK, &set, NULL);
      struct sigevent ev;
      memset(&ev, 0, sizeof(ev));
      ev.sigev_notify = SIGEV_SIGNAL;
      ev.sigev_signo = SIGRTMIN;
      ev.sigev_value.sival_int = 42;
      timer_t tid;
      if (timer_create(CLOCK_MONOTONIC, &ev, &tid) != 0)
         return 1;
      t0 = now_ns();
      its = (struct itimerspec){ { 0, 0 }, { 0, 3000000 } };
      timer_settime(tid, 0, &its, NULL);
      siginfo_t si;
      struct timespec limit = { 1, 0 };
      const int sig = sigtimedwait(&set, &si, &limit);
      report("posix-timer", 3000000, t0, sig == SIGRTMIN && si.si_code == SI_TIMER ? si.si_value.sival_int : -1, 42);

      /* timer_gettime: what is left of 10 ms, at once */
      its = (struct itimerspec){ { 0, 0 }, { 0, 10000000 } };
      timer_settime(tid, 0, &its, NULL);
      struct itimerspec cur;
      timer_gettime(tid, &cur);
      printf("timers: gettime-left %ld\n", cur.it_value.tv_nsec);
      if (cur.it_value.tv_sec || cur.it_value.tv_nsec > 10000000 || cur.it_value.tv_nsec < 9000000)
         bad = 1;
      timer_delete(tid);
   }

   if (on("timerfd"))
   {
      /* a timerfd, read */
      const int fd = timerfd_create(CLOCK_MONOTONIC, 0);
      t0 = now_ns();
      its = (struct itimerspec){ { 0, 0 }, { 0, 4000000 } };
      timerfd_settime(fd, 0, &its, NULL);
      uint64_t n = 0;
      const long r = read(fd, &n, sizeof(n));
      report("timerfd", 4000000, t0, r == 8 ? (long)n : -1, 1);
      close(fd);
   }

   /* times(): the CPU time, in clock ticks */
   struct tms tm;
   times(&tm);
   printf("timers: times-utime %ld\n", (long)tm.tms_utime);

   printf("timers: done\n");
   return bad || !x;
}
