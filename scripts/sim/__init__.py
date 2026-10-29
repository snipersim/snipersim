import sim_config as config
import sim_stats as stats
import sim_hooks as hooks
import sim_dvfs as dvfs
import sim_control as control
import sim_bbv as bbv
import sim_mem as mem
import sim_thread as thread
import sim.util

import os, sqlite3
# Hooks run on the simulator's threads, not the one that imported this:
# periodic-stats.py's db_delete ran on a core's thread and Python 3's sqlite3
# refused the connection there (ProgrammingError: "SQLite objects created in
# a thread can only be used in that same thread").  The callbacks are
# serialized by the interpreter lock; SQLite itself is built thread-safe.
stats.db = sqlite3.connect(os.path.join(config.output_dir, 'sim.stats.sqlite3'), check_same_thread = False)
