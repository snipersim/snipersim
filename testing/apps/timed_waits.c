/* timed_waits: the guest's timeouts in the guest's time.
 *
 * No time the guest gives reaches the host's kernel (sift/guest_time.h):
 * every timed wait here ends at its deadline in the guest's time -- the
 * simulator's, or the one-IPC model's where no simulator answers -- and the
 * elapsed time each reports is measured with clock_gettime, the guest's
 * clock.  Long timeouts (2 s) must not take 2 s of the host's.  A worker
 * thread computes for a while, then writes a pipe and posts a semaphore:
 * the waits on those end when it does, before their 1 s timeouts.
 * testing/test_qemu.py timed-waits runs it live and to a file, on x86-64,
 * RISC-V and AArch64; test_apps' timed-waits under SDE.  Prints one line
 * per wait,
 *
 *   timed_waits: <name> want=<ns> got=<ns> ret=<result>
 *
 * and "timed_waits: done".  Exits 1 if a wait returned the wrong result.
 * TIMED_WAITS_ONLY=<name,name...> runs only those waits. */
#define _GNU_SOURCE
#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <semaphore.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/select.h>
#include <time.h>
#include <unistd.h>

static uint64_t now_ns(clockid_t c)
{
   struct timespec ts;
   clock_gettime(c, &ts);
   return (uint64_t)ts.tv_sec * 1000000000u + (uint64_t)ts.tv_nsec;
}

static struct timespec rel(uint64_t ns)
{
   struct timespec ts = { (time_t)(ns / 1000000000u), (long)(ns % 1000000000u) };
   return ts;
}

static struct timespec abs_after(clockid_t c, uint64_t ns)
{
   return rel(now_ns(c) + ns);
}

static int bad = 0;

static int on(const char *name)
{
   const char *only = getenv("TIMED_WAITS_ONLY");
   if (!only)
      return 1;
   const size_t n = strlen(name);
   for (const char *p = only; (p = strstr(p, name)); p += n)
      if ((p == only || p[-1] == ',') && (p[n] == ',' || !p[n]))
         return 1;
   return 0;
}
static void report(const char *name, uint64_t want, uint64_t t0, long ret, long want_ret)
{
   const uint64_t got = now_ns(CLOCK_MONOTONIC) - t0;
   printf("timed_waits: %s want=%llu got=%llu ret=%ld\n", name, (unsigned long long)want, (unsigned long long)got, ret);
   if (ret != want_ret)
   {
      printf("timed_waits: %s returned %ld, not %ld\n", name, ret, want_ret);
      bad = 1;
   }
}

static int pipefd[2];
static sem_t sem;
static volatile uint64_t sink;

static void *worker(void *arg)
{
   (void)arg;
   /* about 2 ms of instructions at 1 IPC, 1 GHz */
   for (uint64_t i = 0; i < 200000; i++)
      sink += i ^ (sink >> 3);
   if (write(pipefd[1], "x", 1) != 1)
      return NULL;
   sem_post(&sem);
   return NULL;
}

int main(void)
{
   uint64_t t0;
   long r;
   struct timespec ts;
   setvbuf(stdout, NULL, _IOLBF, 0);

   if (on("nanosleep"))
   {
      t0 = now_ns(CLOCK_MONOTONIC);
      ts = rel(1000000);
      r = nanosleep(&ts, NULL);
      report("nanosleep", 1000000, t0, r, 0);
   }
   if (on("clock_nanosleep-abs"))
   {
      t0 = now_ns(CLOCK_MONOTONIC);
      ts = abs_after(CLOCK_MONOTONIC, 3000000);
      r = clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, NULL);
      report("clock_nanosleep-abs", 3000000, t0, r, 0);
   }
   if (on("poll"))
   {
      t0 = now_ns(CLOCK_MONOTONIC);
      r = poll(NULL, 0, 5);
      report("poll", 5000000, t0, r, 0);
   }
   if (on("ppoll"))
   {
      t0 = now_ns(CLOCK_MONOTONIC);
      ts = rel(2000000);
      r = ppoll(NULL, 0, &ts, NULL);
      report("ppoll", 2000000, t0, r, 0);
   }
   if (on("select"))
   {
      t0 = now_ns(CLOCK_MONOTONIC);
      struct timeval tv = { 0, 3000 };
      r = select(0, NULL, NULL, NULL, &tv);
      report("select", 3000000, t0, r, 0);
      if (tv.tv_sec || tv.tv_usec)
      {
         printf("timed_waits: select left %ld.%06ld s in its timeval, not 0\n", (long)tv.tv_sec, (long)tv.tv_usec);
         bad = 1;
      }
   }
   if (on("epoll_wait"))
   {
      int ep = epoll_create1(0);
      struct epoll_event ev;
      t0 = now_ns(CLOCK_MONOTONIC);
      r = epoll_wait(ep, &ev, 1, 4);
      report("epoll_wait", 4000000, t0, r, 0);
   }
   if (on("sigtimedwait"))
   {
      sigset_t set;
      sigemptyset(&set);
      sigaddset(&set, SIGUSR2);
      sigprocmask(SIG_BLOCK, &set, NULL);
      t0 = now_ns(CLOCK_MONOTONIC);
      ts = rel(3000000);
      r = sigtimedwait(&set, NULL, &ts);
      report("sigtimedwait", 3000000, t0, r < 0 ? -errno : r, -EAGAIN);
   }
   if (on("cond_timedwait-realtime"))
   {
      pthread_mutex_t m = PTHREAD_MUTEX_INITIALIZER;
      pthread_cond_t cv = PTHREAD_COND_INITIALIZER;
      pthread_mutex_lock(&m);
      t0 = now_ns(CLOCK_MONOTONIC);
      ts = abs_after(CLOCK_REALTIME, 10000000);
      r = pthread_cond_timedwait(&cv, &m, &ts);
      pthread_mutex_unlock(&m);
      report("cond_timedwait-realtime", 10000000, t0, r, ETIMEDOUT);
   }
   sem_init(&sem, 0, 0);
   if (on("sem_timedwait"))
   {
      t0 = now_ns(CLOCK_MONOTONIC);
      ts = abs_after(CLOCK_REALTIME, 2000000);
      r = sem_timedwait(&sem, &ts);
      report("sem_timedwait", 2000000, t0, r < 0 ? -errno : r, -ETIMEDOUT);
   }
   /* a long one: 2 s of the guest's time, not of the host's */
   if (on("poll-2s"))
   {
      t0 = now_ns(CLOCK_MONOTONIC);
      r = poll(NULL, 0, 2000);
      report("poll-2s", 2000000000, t0, r, 0);
   }
   /* ended by another thread, before the timeout */
   if (on("poll-ready"))
   {
      if (pipe(pipefd))
         return 1;
      pthread_t w;
      pthread_create(&w, NULL, worker, NULL);
      struct pollfd pfd = { pipefd[0], POLLIN, 0 };
      t0 = now_ns(CLOCK_MONOTONIC);
      r = poll(&pfd, 1, 1000);
      report("poll-ready", 0, t0, r, 1);
      t0 = now_ns(CLOCK_MONOTONIC);
      ts = abs_after(CLOCK_REALTIME, 1000000000);
      r = sem_timedwait(&sem, &ts);
      report("sem_timedwait-posted", 0, t0, r < 0 ? -errno : r, 0);
      pthread_join(w, NULL);
   }

   printf("timed_waits: done\n");
   return bad;
}
