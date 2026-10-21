# Simulator side of magic-api.c (testing/test_magic.py): answers the user
# command 0x5a5a with its argument plus one, and prints each marker with
# what it carried, so that the test sees both ends of each command.
import sim

class MagicApi:
  def setup(self, args):
    sim.util.register_command(0x5a5a, lambda core, arg: arg + 1)
  def hook_magic_marker(self, thread, core, a, b, s):
    print('[MAGIC-API] marker thread=%d a=%d b=%d str=%s' % (thread, a, b, s or ''))

sim.util.register(MagicApi())
