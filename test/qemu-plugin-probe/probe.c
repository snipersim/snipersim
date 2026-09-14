/*
 * Verification plugin for a Sniper QEMU frontend.
 *
 * Not a frontend: a probe that answers the questions the design rests on.
 *
 *   mode=count    per-vCPU instruction counting via scoreboard + cond_cb,
 *                 and whether insn data/size/vaddr give SIFT what it needs
 *   mode=block    block inside an insn callback for `ms` milliseconds every
 *                 `quantum` instructions -- the thing claimed to be safe when
 *                 single-threaded and to deadlock when not
 *   mode=time     request time control and drive guest time (system mode)
 */
#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include <glib.h>
#include <unistd.h>
#include "compat.h"

QEMU_PLUGIN_EXPORT int qemu_plugin_version = QEMU_PLUGIN_VERSION;

typedef struct { uint64_t insns; uint64_t quantum_insn; uint64_t traps; } vCPU;

static struct qemu_plugin_scoreboard *vcpus;
static qemu_plugin_u64 quantum_insn;
static uint64_t quantum = 100000;
static uint64_t block_ms;
static unsigned block_from_cpu;   /* only block vcpus >= this */
static volatile int blocked_now;
static int mode_barrier;             /* model Sniper's cross-thread barrier */
static uint64_t peer_progress;       /* bumped by vcpu 0 each trap */
static int mode_block, mode_time;
static const void *time_handle;
static int64_t virt_ns;
static GMutex lock;
static uint64_t first_bytes_seen;
static char first_disas[128];
static uint64_t total_traps, total_mem, total_flush;
static uint64_t range_lo, range_hi, range_count;
static uint64_t syscalls;
static int have_pa; static uint64_t first_pa, first_pa_va; static uint64_t pa_ok, pa_io;
static int64_t first_sys_num = -1;
static uint64_t first_sys_a1;

static void trap(unsigned int cpu_index, void *udata)
{
    vCPU *v = qemu_plugin_scoreboard_find(vcpus, cpu_index);
    v->insns += v->quantum_insn;
    v->quantum_insn = 0;
    v->traps++;
    g_mutex_lock(&lock); total_traps++; g_mutex_unlock(&lock);

    if (mode_barrier) {
        /* Faithful model of Sniper flow control: a vCPU may not continue until
         * its peer has also advanced.  vcpu 0 is the peer here.  If vcpu 0 is
         * stuck in start_exclusive() waiting for us, nobody moves. */
        if (cpu_index == 0) {
            g_mutex_lock(&lock); peer_progress++; g_mutex_unlock(&lock);
        } else {
            g_mutex_lock(&lock);
            uint64_t seen = peer_progress;
            g_mutex_unlock(&lock);
            gint64 deadline = g_get_monotonic_time() + 20 * G_TIME_SPAN_SECOND;
            fprintf(stderr, "PROBE vcpu=%u waiting for peer progress (seen=%" PRIu64 ")\n",
                    cpu_index, seen);
            for (;;) {
                g_usleep(1000);
                g_mutex_lock(&lock);
                uint64_t now = peer_progress;
                g_mutex_unlock(&lock);
                if (now > seen) break;
                if (g_get_monotonic_time() > deadline) {
                    fprintf(stderr, "PROBE vcpu=%u DEADLOCK-DETECTED: peer never advanced in 20s\n",
                            cpu_index);
                    _exit(42);
                }
            }
            fprintf(stderr, "PROBE vcpu=%u peer advanced, continuing\n", cpu_index);
        }
    }

    if (mode_block && cpu_index >= block_from_cpu) {
        /* Block this vCPU inside the callback, with running == true. */
        g_atomic_int_inc(&blocked_now);
        fprintf(stderr, "PROBE vcpu=%u BLOCKING for %" PRIu64 "ms (blocked_now=%d)\n",
                cpu_index, block_ms, blocked_now);
        g_usleep(block_ms * 1000);
        g_atomic_int_dec_and_test(&blocked_now);
        fprintf(stderr, "PROBE vcpu=%u released\n", cpu_index);
    }
    if (mode_time && time_handle) {
        g_mutex_lock(&lock);
        /* 1 GHz: 1 ns per instruction. Time only moves forward. */
        int64_t want = (int64_t)(v->insns);
        if (want > virt_ns) { virt_ns = want; qemu_plugin_update_ns(time_handle, want); }
        g_mutex_unlock(&lock);
    }
}

static void syscall_cb(SNP_SYSCALL_ARGS)
{
    g_mutex_lock(&lock);
    if (first_sys_num < 0) { first_sys_num = num; first_sys_a1 = a1; }
    syscalls++;
    g_mutex_unlock(&lock);
}

static void range_insn(unsigned int cpu_index, void *udata)
{
    g_mutex_lock(&lock); range_count++; g_mutex_unlock(&lock);
}

static void mem_cb(unsigned int cpu_index, qemu_plugin_meminfo_t info,
                   uint64_t vaddr, void *udata)
{
    g_mutex_lock(&lock);
    total_mem++;
    /* Physical address: only meaningful in system emulation. */
    struct qemu_plugin_hwaddr *hw = qemu_plugin_get_hwaddr(info, vaddr);
    if (hw) {
        if (qemu_plugin_hwaddr_is_io(hw)) {
            pa_io++;
        } else {
            pa_ok++;
            if (!have_pa) {
                have_pa = 1;
                first_pa_va = vaddr;
                first_pa = qemu_plugin_hwaddr_phys_addr(hw);
            }
        }
    }
    g_mutex_unlock(&lock);
}

static void tb_trans(SNP_TB_TRANS_ARGS)
{
    size_t n = qemu_plugin_tb_n_insns(tb);

    if (!first_bytes_seen) {
        struct qemu_plugin_insn *insn = qemu_plugin_tb_get_insn(tb, 0);
        size_t sz = qemu_plugin_insn_size(insn);
        uint8_t buf[16] = {0};
        size_t got = qemu_plugin_insn_data(insn, buf, sizeof(buf));
        char *d = qemu_plugin_insn_disas(insn);
        g_mutex_lock(&lock);
        if (!first_bytes_seen) {
            first_bytes_seen = 1;
            snprintf(first_disas, sizeof(first_disas),
                     "vaddr=0x%" PRIx64 " size=%zu data_got=%zu bytes=%02x%02x%02x disas=%s",
                     qemu_plugin_insn_vaddr(insn), sz, got, buf[0], buf[1], buf[2],
                     d ? d : "(null)");
        }
        g_mutex_unlock(&lock);
        g_free(d);
    }

    qemu_plugin_register_vcpu_tb_exec_inline_per_vcpu(
        tb, QEMU_PLUGIN_INLINE_ADD_U64, quantum_insn, n);
    qemu_plugin_register_vcpu_tb_exec_cond_cb(
        tb, trap, QEMU_PLUGIN_CB_NO_REGS,
        QEMU_PLUGIN_COND_GE, quantum_insn, quantum, NULL);

    if (range_hi) {
        for (size_t i = 0; i < n; i++) {
            struct qemu_plugin_insn *in = qemu_plugin_tb_get_insn(tb, i);
            uint64_t va = qemu_plugin_insn_vaddr(in);
            if (va >= range_lo && va < range_hi) {
                qemu_plugin_register_vcpu_insn_exec_cb(in, range_insn,
                                                       QEMU_PLUGIN_CB_NO_REGS, NULL);
            }
        }
    }

    for (size_t i = 0; i < n; i++) {
        qemu_plugin_register_vcpu_mem_cb(qemu_plugin_tb_get_insn(tb, i), mem_cb,
                                         QEMU_PLUGIN_CB_NO_REGS,
                                         QEMU_PLUGIN_MEM_RW, NULL);
    }
}

static void flush_cb(SNP_FLUSH_ARGS)
{
    g_mutex_lock(&lock); total_flush++; g_mutex_unlock(&lock);
    fprintf(stderr, "PROBE tb_flush #%" PRIu64 "\n", total_flush);
}

static void at_exit(SNP_UDATA_ARGS)
{
    uint64_t total = 0;
    for (int i = 0; i < qemu_plugin_num_vcpus(); i++) {
        vCPU *v = qemu_plugin_scoreboard_find(vcpus, i);
        total += v->insns + v->quantum_insn;
        if (v->insns || v->quantum_insn)
            fprintf(stderr, "PROBE vcpu=%d insns=%" PRIu64 " traps=%" PRIu64 "\n",
                    i, v->insns + v->quantum_insn, v->traps);
    }
    fprintf(stderr, "PROBE total_insns=%" PRIu64 " traps=%" PRIu64 " mem=%" PRIu64 " tb_flushes=%" PRIu64 "\n",
            total, total_traps, total_mem, total_flush);
    fprintf(stderr, "PROBE first_insn %s\n", first_disas);
    if (have_pa || pa_io) fprintf(stderr, "PROBE hwaddr ram=%" PRIu64 " io=%" PRIu64 " first va=0x%" PRIx64 " -> pa=0x%" PRIx64 "\n", pa_ok, pa_io, first_pa_va, first_pa);
    fprintf(stderr, "PROBE syscalls=%" PRIu64 " first_num=%" PRId64 " first_a1=0x%" PRIx64 "\n", syscalls, first_sys_num, first_sys_a1);
    if (range_hi) fprintf(stderr, "PROBE range_insns=%" PRIu64 " in [0x%" PRIx64 ",0x%" PRIx64 ")\n", range_count, range_lo, range_hi);
    if (mode_time) fprintf(stderr, "PROBE time_control=%s virt_ns=%" PRId64 "\n",
                           time_handle ? "granted" : "DENIED", virt_ns);
    qemu_plugin_scoreboard_free(vcpus);
}

QEMU_PLUGIN_EXPORT int qemu_plugin_install(qemu_plugin_id_t id,
                                           const qemu_info_t *info,
                                           int argc, char **argv)
{
    for (int i = 0; i < argc; i++) {
        g_auto(GStrv) t = g_strsplit(argv[i], "=", 2);
        if (!g_strcmp0(t[0], "quantum"))   quantum = g_ascii_strtoull(t[1], NULL, 10);
        else if (!g_strcmp0(t[0], "block")) { mode_block = 1; block_ms = g_ascii_strtoull(t[1], NULL, 10); }
        else if (!g_strcmp0(t[0], "time"))  mode_time = 1;
        else if (!g_strcmp0(t[0], "blockfrom")) block_from_cpu = g_ascii_strtoull(t[1], NULL, 10);
        else if (!g_strcmp0(t[0], "barrier")) mode_barrier = 1;
        else if (!g_strcmp0(t[0], "lo")) range_lo = g_ascii_strtoull(t[1], NULL, 0);
        else if (!g_strcmp0(t[0], "hi")) range_hi = g_ascii_strtoull(t[1], NULL, 0);
        else { fprintf(stderr, "bad option %s\n", argv[i]); return -1; }
    }

    vcpus = qemu_plugin_scoreboard_new(sizeof(vCPU));
    quantum_insn = qemu_plugin_scoreboard_u64_in_struct(vcpus, vCPU, quantum_insn);

    if (mode_time) {
        time_handle = qemu_plugin_request_time_control();
        fprintf(stderr, "PROBE requested time control: %s\n",
                time_handle ? "granted" : "DENIED");
    }

    SNP_REG_TB_TRANS(id, tb_trans);
    SNP_REG_FLUSH(id, flush_cb);
    SNP_REG_SYSCALL(id, syscall_cb);
    SNP_REG_ATEXIT(id, at_exit);
    return 0;
}
