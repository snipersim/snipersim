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

#endif
