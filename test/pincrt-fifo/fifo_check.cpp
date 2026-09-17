/* fifo_check: two checks of how a C runtime handles FIFOs, written to be
 * compiled twice from the same source --
 *
 *   natively, against glibc                 (make fifo_check_native)
 *   as a Pin tool, against PinCRT           (make obj-intel64/fifo_check.so)
 *
 * -- so that the runtimes can be compared on identical code.  run.sh runs
 * each check under a timeout and reports a hang as HANG.
 *
 * Check 1, "fopen-lock": one thread calls fopen(FIFO, "w") on a FIFO that
 * never gets a reader, so that call blocks in open(2).  Once it is blocked,
 * another thread calls fopen() on an ordinary regular file.  With glibc the
 * second call returns at once: the first thread holds nothing but its own
 * open(2).  With PinCRT the second call never returns: fopen() blocks on the
 * FIFO while holding a runtime-wide lock that every other fopen() needs.
 *
 * Check 2, "fcntl": fcntl(fd, F_SETFL, O_NONBLOCK) on the read end of an
 * empty pipe, then read().  With glibc read() returns -1/EAGAIN at once.
 * With PinCRT the fcntl reports success but read() blocks forever: F_SETFL
 * does not reach the kernel.
 *
 * Neither check needs anything beyond POSIX; the native build is plain C++
 * with pthreads.  Paths come from the environment (FIFO_DIR) so the same
 * binary and tool read the same files. */

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifdef PIN_TOOL
#include "pin.H"
#else
#include <pthread.h>
#endif

static char g_dir[512];
static int g_raw_blocker = 0;   /* 1: block in a raw open(2) instead of fopen() */
static int g_raw_probe = 0;     /* 1: probe with a raw open(2) instead of fopen() */
static char g_fifo[600], g_regular[600], g_marker[600];

static void paths()
{
   const char *d = getenv("FIFO_DIR");
   if (!d) { fprintf(stderr, "FIFO_DIR not set\n"); exit(2); }
   snprintf(g_dir, sizeof(g_dir), "%s", d);
   g_raw_blocker = getenv("RAW_BLOCKER") != 0;
   g_raw_probe = getenv("RAW_PROBE") != 0;
   snprintf(g_fifo, sizeof(g_fifo), "%s/noreader.fifo", d);   /* created by run.sh, never opened for reading */
   snprintf(g_regular, sizeof(g_regular), "%s/regular.txt", d);
   snprintf(g_marker, sizeof(g_marker), "%s/blocker-started", d);
}

/* --- check 1 ------------------------------------------------------------ */

/* Runs on its own thread: announce, then block in fopen() forever. */
static void blocker_body()
{
   int m = open(g_marker, O_WRONLY | O_CREAT | O_TRUNC, 0666);   /* raw syscall: no stdio involved */
   fprintf(stderr, "blocker: marker %s -> fd %d (errno %d: %s)\n", g_marker, m, m < 0 ? errno : 0, m < 0 ? strerror(errno) : "ok");
   if (m >= 0) close(m);
   if (g_raw_blocker)
   {
      fprintf(stderr, "blocker: raw open(%s, O_WRONLY) -- no reader, so this blocks in open(2) with no stdio involved\n", g_fifo);
      int fd = open(g_fifo, O_WRONLY);
      fprintf(stderr, "blocker: open returned %d (unexpected: a reader appeared?)\n", fd);
      return;
   }
   fprintf(stderr, "blocker: fopen(%s, \"w\") -- no reader, so this blocks in open(2)\n", g_fifo);
   FILE *f = fopen(g_fifo, "w");
   fprintf(stderr, "blocker: fopen returned %p (unexpected: a reader appeared?)\n", (void *)f);
}

/* Runs on another thread once the blocker is inside fopen(). */
/* raw write(2): a breadcrumb that does not touch the runtime's stdio */
static void raw(const char *msg) { ssize_t r = write(2, msg, strlen(msg)); (void)r; }

static void probe_body()
{
   raw("probe: reached (raw write, no stdio)\n");
   fprintf(stderr, "probe: fprintf works while the blocker is in fopen()\n");
   if (g_raw_probe)
   {
      raw("probe: fprintf returned (raw write); now a raw open(2) of a regular file\n");
      int fd = open(g_regular, O_WRONLY | O_CREAT | O_TRUNC, 0666);
      raw("probe: open returned (raw write)\n");
      if (fd >= 0) close(fd);
      fprintf(stderr, "RESULT check1 open-lock: raw open(regular) returned %d -- %s\n",
              fd, fd >= 0 ? "no lock held across the other thread's blocked open" : "failed");
      return;
   }
   raw("probe: fprintf returned (raw write); now fopen() of a regular file\n");
   FILE *f = fopen(g_regular, "w");
   raw("probe: fopen returned (raw write)\n");
   if (f) { fputs("ok\n", f); fclose(f); }
   fprintf(stderr, "RESULT check1 fopen-lock: fopen(regular) returned %s -- %s\n",
           f ? "a stream" : "NULL", f ? "no lock held across the blocked fopen" : "failed");
}

/* --- check 2 ------------------------------------------------------------ */

static void fcntl_body()
{
   int fds[2];
   if (pipe(fds) != 0) { fprintf(stderr, "pipe: %s\n", strerror(errno)); exit(2); }
   errno = 0;
   int rc = fcntl(fds[0], F_SETFL, O_NONBLOCK);
   int err = errno;
   int fl = fcntl(fds[0], F_GETFL);
   fprintf(stderr, "fcntl(F_SETFL, O_NONBLOCK) returned %d (errno %d: %s); F_GETFL returned %d, O_NONBLOCK %s\n",
           rc, err, strerror(err), fl, (fl != -1 && (fl & O_NONBLOCK)) ? "set" : "NOT set");
   fprintf(stderr, "read() on the empty pipe: EAGAIN expected at once, a hang means F_SETFL was ignored\n");
   char c;
   ssize_t n = read(fds[0], &c, 1);
   fprintf(stderr, "RESULT check2 fcntl: read returned %ld errno=%d (%s)\n",
           (long)n, errno, n < 0 ? strerror(errno) : "");
}

/* --- drivers ------------------------------------------------------------ */

#ifdef PIN_TOOL

KNOB<int> KnobCheck(KNOB_MODE_WRITEONCE, "pintool", "check", "1", "1: fopen-lock, 2: fcntl");

/* Both bodies run on application threads, inside analysis routines, which
 * is where the SIFT recorder ran them: the application's first thread calls
 * block_here() (the tool blocks in fopen() there and never returns) and its
 * main thread, once it sees the marker, calls probe(). */
static VOID on_block() { blocker_body(); }
static VOID on_probe() { probe_body(); PIN_ExitProcess(0); }

static VOID image(IMG img, VOID *)
{
   RTN b = RTN_FindByName(img, "block_here");
   if (RTN_Valid(b)) { RTN_Open(b); RTN_InsertCall(b, IPOINT_BEFORE, (AFUNPTR)on_block, IARG_END); RTN_Close(b); }
   RTN r = RTN_FindByName(img, "probe");
   if (RTN_Valid(r)) { RTN_Open(r); RTN_InsertCall(r, IPOINT_BEFORE, (AFUNPTR)on_probe, IARG_END); RTN_Close(r); }
}

int main(int argc, char *argv[])
{
   PIN_InitSymbols();
   if (PIN_Init(argc, argv)) return 1;
   paths();
   if (KnobCheck.Value() == 2)
   {
      fcntl_body();          /* needs no application: done before it starts */
      PIN_ExitProcess(0);
   }
   IMG_AddInstrumentFunction(image, 0);
   PIN_StartProgram();
   return 0;
}

#elif defined(DR_CLIENT)

/* DynamoRIO twin: the same two functions are wrapped with drwrap, so the
 * bodies run on the application threads in the client's (private) libc. */
#include "dr_api.h"
#include "drmgr.h"
#include "drwrap.h"

static void pre_block(void *, void **) { blocker_body(); }
static void pre_probe(void *, void **) { probe_body(); dr_exit_process(0); }

static void module_load(void *, const module_data_t *mod, bool)
{
   app_pc b = (app_pc)dr_get_proc_address(mod->handle, "block_here");
   app_pc p = (app_pc)dr_get_proc_address(mod->handle, "probe");
   if (b) drwrap_wrap(b, pre_block, NULL);
   if (p) drwrap_wrap(p, pre_probe, NULL);
}

/* Mode 3: both bodies on client threads (dr_create_client_thread), so only
 * the client's libc is in play; the application is then just something that
 * stays alive, such as /bin/sleep. */
static void dr_blocker(void *) { blocker_body(); }
static void dr_prober(void *)
{
   while (access(g_marker, F_OK) != 0)
      dr_sleep(10);
   dr_sleep(200);
   probe_body();
   dr_exit_process(0);
}

extern "C" DR_EXPORT void dr_client_main(client_id_t, int argc, const char *argv[])
{
   paths();
   if (argc > 1 && argv[1][0] == '2') { fcntl_body(); dr_exit_process(0); }
   if (argc > 1 && argv[1][0] == '3')
   {
      dr_create_client_thread(dr_blocker, NULL);
      dr_create_client_thread(dr_prober, NULL);
      return;
   }
   drmgr_init();
   drwrap_init();
   drmgr_register_module_load_event(module_load);
}

#else

static void *blocker_thread(void *) { blocker_body(); return 0; }

static void wait_for_marker()
{
   while (access(g_marker, F_OK) != 0)
      usleep(10 * 1000);
   usleep(200 * 1000);   /* let the blocker get from the marker into open(2) */
}

int main(int argc, char *argv[])
{
   paths();
   int check = argc > 1 ? atoi(argv[1]) : 1;
   if (check == 2) { fcntl_body(); return 0; }
   pthread_t t;
   pthread_create(&t, 0, blocker_thread, 0);
   wait_for_marker();
   probe_body();
   return 0;                /* exit() ends the blocked thread too */
}

#endif
