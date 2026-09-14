/*
 * Plugin API compatibility shim.
 *
 * QEMU 11 (plugin API v7) replaced the leading qemu_plugin_id_t on callbacks
 * with a trailing void *userdata, and the registration calls grew a matching
 * argument.  Everything else we use -- time control, scoreboards, insn_data,
 * memory callbacks -- is unchanged from v3, so this is the whole difference.
 *
 * The header also moved: include/qemu/qemu-plugin.h -> include/plugins/.
 */
#ifndef SNIPER_QEMU_COMPAT_H
#define SNIPER_QEMU_COMPAT_H

#include <qemu-plugin.h>

#if QEMU_PLUGIN_VERSION >= 7
  #define SNP_TB_TRANS_ARGS   struct qemu_plugin_tb *tb, void *snp_ud
  #define SNP_UDATA_ARGS      void *snp_ud
  #define SNP_FLUSH_ARGS      void *snp_ud
  #define SNP_REG_TB_TRANS(id, fn)  qemu_plugin_register_vcpu_tb_trans_cb((id), (fn), NULL)
  #define SNP_REG_ATEXIT(id, fn)    qemu_plugin_register_atexit_cb((id), (fn), NULL)
  #define SNP_REG_FLUSH(id, fn)     qemu_plugin_register_flush_cb((id), (fn), NULL)
  #define SNP_SYSCALL_ARGS    unsigned int vcpu_index, int64_t num, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6, uint64_t a7, uint64_t a8, void *snp_ud
  #define SNP_REG_SYSCALL(id, fn)   qemu_plugin_register_vcpu_syscall_cb((id), (fn), NULL)
  #define SNP_VCPU_ARGS       unsigned int cpu_index, void *snp_ud
  #define SNP_REG_VCPU_INIT(id, fn) qemu_plugin_register_vcpu_init_cb((id), (fn), NULL)
  #define SNP_REG_VCPU_EXIT(id, fn) qemu_plugin_register_vcpu_exit_cb((id), (fn), NULL)
#else
  #define SNP_TB_TRANS_ARGS   qemu_plugin_id_t snp_id, struct qemu_plugin_tb *tb
  #define SNP_UDATA_ARGS      qemu_plugin_id_t snp_id, void *snp_ud
  /* v3: flush is simple_cb_t -- id only, no userdata at all. */
  #define SNP_FLUSH_ARGS      qemu_plugin_id_t snp_id
  #define SNP_REG_TB_TRANS(id, fn)  qemu_plugin_register_vcpu_tb_trans_cb((id), (fn))
  #define SNP_REG_ATEXIT(id, fn)    qemu_plugin_register_atexit_cb((id), (fn), NULL)
  #define SNP_REG_FLUSH(id, fn)     qemu_plugin_register_flush_cb((id), (fn))
  #define SNP_SYSCALL_ARGS    qemu_plugin_id_t snp_id, unsigned int vcpu_index, int64_t num, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6, uint64_t a7, uint64_t a8
  #define SNP_REG_SYSCALL(id, fn)   qemu_plugin_register_vcpu_syscall_cb((id), (fn))
  #define SNP_VCPU_ARGS       qemu_plugin_id_t snp_id, unsigned int cpu_index
  #define SNP_REG_VCPU_INIT(id, fn) qemu_plugin_register_vcpu_init_cb((id), (fn))
  #define SNP_REG_VCPU_EXIT(id, fn) qemu_plugin_register_vcpu_exit_cb((id), (fn))
#endif

#endif
