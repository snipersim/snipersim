/* Can a plugin alone emulate an instruction QEMU does not know?  Claims the
 * AME word 0xa4a0002b (ame.release, CUSTOM-1): at execution it reads a1,
 * writes a0 = a1 + 35, stores a marker to the address in a2, then moves the
 * PC past the instruction so QEMU's illegal-instruction code never runs. */
#include <qemu-plugin.h>
#include <glib.h>
#include <stdio.h>
#include <string.h>
QEMU_PLUGIN_EXPORT int qemu_plugin_version = QEMU_PLUGIN_VERSION;
static struct qemu_plugin_register *ra0, *ra1, *ra2;
static uint64_t vaddr_claimed;
static int hits;
static void find_regs(void) {
   GArray *regs = qemu_plugin_get_registers();
   for (guint i = 0; i < regs->len; i++) {
      qemu_plugin_reg_descriptor *d = &g_array_index(regs, qemu_plugin_reg_descriptor, i);
      if (!strcmp(d->name, "a0")) ra0 = d->handle;
      if (!strcmp(d->name, "a1")) ra1 = d->handle;
      if (!strcmp(d->name, "a2")) ra2 = d->handle;
   }
   g_array_free(regs, TRUE);
}
static uint64_t rd64(struct qemu_plugin_register *r) {
   GByteArray *b = g_byte_array_new(); uint64_t v = 0;
   if (qemu_plugin_read_register(r, b) && b->len >= 8) memcpy(&v, b->data, 8);
   g_byte_array_free(b, TRUE); return v;
}
static void emulate(unsigned int cpu_index, void *udata) {
   uint64_t pc = (uint64_t)(uintptr_t)udata;
   if (!ra0) find_regs();
   uint64_t a1 = rd64(ra1), a2 = rd64(ra2), a0 = a1 + 35;
   GByteArray *b = g_byte_array_new(); g_byte_array_append(b, (guint8 *)&a0, 8);
   bool wr = qemu_plugin_write_register(ra0, b);
   uint64_t marker = 0x1234567890abcdefULL; GByteArray *m = g_byte_array_new(); g_byte_array_append(m, (guint8 *)&marker, 8);
   bool wm = qemu_plugin_write_memory_vaddr(a2, m);
   hits++;
   fprintf(stderr, "[emu-probe] pc=0x%" PRIx64 " a1=%" PRIu64 " -> a0=%" PRIu64 " (reg write %s, mem write %s); skipping the instruction\n", pc, a1, a0, wr ? "ok" : "FAILED", wm ? "ok" : "FAILED");
   g_byte_array_free(b, TRUE); g_byte_array_free(m, TRUE);
   qemu_plugin_set_pc(pc + 4);          /* RW_REGS_PC: exits the CPU loop, restarts at pc+4 */
}
static void tb_trans(struct qemu_plugin_tb *tb, void *udata) {
   size_t n = qemu_plugin_tb_n_insns(tb);
   for (size_t i = 0; i < n; i++) {
      struct qemu_plugin_insn *insn = qemu_plugin_tb_get_insn(tb, i);
      uint8_t bytes[4] = {0}; qemu_plugin_insn_data(insn, bytes, 4);
      uint32_t w = bytes[0] | bytes[1] << 8 | bytes[2] << 16 | bytes[3] << 24;
      if (w == 0xa4a0002b) {
         vaddr_claimed = qemu_plugin_insn_vaddr(insn);
         qemu_plugin_register_vcpu_insn_exec_cb(insn, emulate, QEMU_PLUGIN_CB_RW_REGS_PC, (void *)(uintptr_t)vaddr_claimed);
      }
   }
}
static void at_exit(qemu_plugin_id_t id, void *p) { fprintf(stderr, "[emu-probe] %d emulated executions\n", hits); }
QEMU_PLUGIN_EXPORT int qemu_plugin_install(qemu_plugin_id_t id, const qemu_info_t *info, int argc, char **argv) {
   qemu_plugin_register_vcpu_tb_trans_cb(id, tb_trans, NULL);
   qemu_plugin_register_atexit_cb(id, at_exit, NULL);
   return 0;
}
