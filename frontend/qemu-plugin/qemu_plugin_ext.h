/* The plugin-API additions for emulating an instruction-set extension
 * (tools/riscv/qemu-11.1.1-ime.patch adds them to QEMU: a translation-time
 * claim, raising exceptions, plugin-defined registers, plugin-backed system
 * registers).  A stock QEMU lacks them, and the same plugin must load there
 * for the x86 frontend, so they are bound weakly: a null function pointer
 * means the QEMU does not have the call and the plugin takes its older
 * path (the exception-and-set_pc emulation, printed errors, no registers). */
#ifndef SNIPER_QEMU_PLUGIN_EXT_H
#define SNIPER_QEMU_PLUGIN_EXT_H

#if QEMU_PLUGIN_VERSION >= 7 && !defined(QEMU_PLUGIN_HAS_INSN_CLAIM)
typedef bool (*qemu_plugin_vcpu_insn_claim_cb_t)(unsigned int vcpu_index, struct qemu_plugin_insn *insn, void *udata);
typedef void (*qemu_plugin_reg_read_cb_t)(unsigned int vcpu_index, unsigned int reg, GByteArray *buf, void *udata);
typedef void (*qemu_plugin_reg_write_cb_t)(unsigned int vcpu_index, unsigned int reg, const uint8_t *data, void *udata);
typedef uint64_t (*qemu_plugin_sysreg_read_cb_t)(unsigned int vcpu_index, unsigned int index, void *udata);
typedef void (*qemu_plugin_sysreg_write_cb_t)(unsigned int vcpu_index, unsigned int index, uint64_t value, void *udata);
extern "C" {
__attribute__((weak)) void qemu_plugin_register_vcpu_insn_claim_cb(qemu_plugin_id_t id, qemu_plugin_vcpu_insn_claim_cb_t cb, void *udata);
__attribute__((weak)) bool qemu_plugin_raise_exception(uint64_t vaddr, uint64_t cause, uint64_t tval);
__attribute__((weak)) bool qemu_plugin_register_vcpu_registers(unsigned int vcpu_index, const char *feature, const char *tags,
                                                               const char *const names[], const unsigned int bits[], const char *const types[],
                                                               unsigned int n, qemu_plugin_reg_read_cb_t read, qemu_plugin_reg_write_cb_t write, void *udata);
__attribute__((weak)) bool qemu_plugin_register_sysreg_cb(unsigned int index, qemu_plugin_sysreg_read_cb_t read, qemu_plugin_sysreg_write_cb_t write, void *udata);
}
#define SNP_HAVE_CLAIM      (qemu_plugin_register_vcpu_insn_claim_cb != nullptr)
#define SNP_HAVE_RAISE      (qemu_plugin_raise_exception != nullptr)
#define SNP_HAVE_REGISTERS  (qemu_plugin_register_vcpu_registers != nullptr)
#define SNP_HAVE_SYSREGS    (qemu_plugin_register_sysreg_cb != nullptr)
#elif defined(QEMU_PLUGIN_HAS_INSN_CLAIM)
#define SNP_HAVE_CLAIM true
#define SNP_HAVE_RAISE true
#define SNP_HAVE_REGISTERS true
#define SNP_HAVE_SYSREGS true
#else
#define SNP_HAVE_CLAIM false
#define SNP_HAVE_RAISE false
#define SNP_HAVE_REGISTERS false
#define SNP_HAVE_SYSREGS false
#endif

/* Waiting outside the run loop (tools/qemu-11.1.1-plugin-wait.patch): a vCPU
 * that must wait for Sniper leaves the run loop first, so that its wait holds
 * up no exclusive section -- what makes a multi-threaded guest possible in
 * user mode.  Bound weakly for the same reason as the above: a QEMU without
 * it runs single-threaded guests, waiting in the callback, and refuses a
 * second thread on a live channel. */
#if QEMU_PLUGIN_VERSION >= 7 && !defined(QEMU_PLUGIN_HAS_VCPU_WAIT)
typedef void (*qemu_plugin_vcpu_wait_cb_t)(unsigned int vcpu_index, void *udata);
extern "C" {
__attribute__((weak)) void qemu_plugin_vcpu_wait_at(uint64_t vaddr, qemu_plugin_vcpu_wait_cb_t cb, void *udata, uintptr_t host_pc);
}
#define SNP_HAVE_WAIT       (qemu_plugin_vcpu_wait_at != nullptr)
#elif defined(QEMU_PLUGIN_HAS_VCPU_WAIT)
#define SNP_HAVE_WAIT       true
#else
#define SNP_HAVE_WAIT       false
#endif

/* The guest's counters (tools/qemu-11.1.1-plugin-counters.patch): user mode
 * answers RISC-V's time, cycle and instret, AArch64's CNTVCT and PMCCNTR
 * and x86's TSC from the host's clock; with the call the plugin answers
 * them with Sniper's.  The counter kinds are QEMU's enum, passed as an int. */
enum { SNP_COUNTER_TIME = 0, SNP_COUNTER_CYCLES = 1, SNP_COUNTER_INSTRET = 2 };
#if QEMU_PLUGIN_VERSION >= 7 && !defined(QEMU_PLUGIN_HAS_COUNTER_CB)
typedef uint64_t (*snp_counter_cb_t)(unsigned int vcpu_index, int counter, uint64_t freq_hz, uint64_t host_value, void *udata);
extern "C" {
__attribute__((weak)) void qemu_plugin_register_counter_cb(qemu_plugin_id_t id, snp_counter_cb_t cb, void *udata);
}
#define SNP_HAVE_COUNTERS   (qemu_plugin_register_counter_cb != nullptr)
#define SNP_COUNTER_CB(f)   (f)
#elif defined(QEMU_PLUGIN_HAS_COUNTER_CB)
/* Compiled against the patched header, but loaded into whichever QEMU runs
 * it -- the system-mode builds do not carry the patch -- so still bound
 * weakly: a strong reference left the plugin unloadable there */
extern "C" {
__attribute__((weak)) void qemu_plugin_register_counter_cb(qemu_plugin_id_t id, qemu_plugin_counter_cb_t cb, void *udata);
}
#define SNP_HAVE_COUNTERS   (qemu_plugin_register_counter_cb != nullptr)
#define SNP_COUNTER_CB(f)   ((qemu_plugin_counter_cb_t)(f))
#else
#define SNP_HAVE_COUNTERS   false
#endif

#endif
