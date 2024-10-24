"""
simuserroi.py

run with: ./run-sniper -ggeneral/inst_mode_init=detailed -gperf_model/fast_forward/oneipc/include_memory_latency=true --pinplay-addr-trans --frontend-pc-control 0x7f1a6c5d20d3:100:0x7f1a6c5d20d3:100:0x7f1a6c5d20d3:100:1 -ssimuserroi --roi-script --trace-args="-forcepccountwarmupindetailed 1"

"""

import sim

SIM_USER_ROI = 0x0be0000f

class SimUserROI:
  def setup(self, args):
    roiscript = sim.config.get_bool('general/roi_script')
    if not roiscript:
      print('[SimUserROI] ERROR: --roi-script is not set, but is required when using a start instruction count. Aborting')
      sim.control.abort()
      return
    sim.util.register_command(SIM_USER_ROI, self.set_roi)

  # Out-of-bound set-roi
  def set_roi(self, cmd, arg):
    if (arg == 0): # start
      print('[SCRIPT] Start of ROI: beginning ROI')
      sim.control.set_roi(True)
    elif (arg == 1): # stop
      print('[SCRIPT] End of ROI: beginning ROI')
      sim.control.set_roi(False)
    elif (arg == 2): # warmup start
      print ('[SCRIPT] Start of WARMUP: beginning WARMUP')
      sim.control.set_instrumentation_mode(sim.control.WARMUP)

sim.util.register(SimUserROI())
