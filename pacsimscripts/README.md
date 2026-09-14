# PacSim
An online sampled simulation methodology that speeds up Sniper by choosing what
to simulate in detail while the application runs, rather than on a fixed
schedule. See *Status of the recorder* below for what is and is not currently
in the tree.

# Running an application

```bash
run-sniper --pacsim -- cmdline
```

`--dynamic-sampling` is an alias for the same flag, for anyone who would rather
name the technique than the tool. Either spelling sets the run up the same way:
the simulator starts in fast-forward rather than cache warm-up, because Pac-Sim
schedules its own warm-up, and the recorder is told to do the live sampling.

The estimated run time is printed at the end of the run. To get it again later
from a directory of output, `pacsim.py -d <dir>` reports the same number from
the same code.

## Status of the recorder

Pac-Sim's recorder side -- the online BBV clustering and intra-barrier analysis
that make the sampling adaptive rather than periodic -- is **not currently in
the tree**. It was removed by commit `f472416`, which replaced the SIFT recorder
with a different implementation; the last commit carrying a working Pac-Sim
recorder is `0c5db2a`. What remains here is the simulator-side magic interface
(`SIM_CMD_GET_SIM_TIME`, `GET_INS_NUM`, `GET_BARRIER_REACHED`) and these
analysis scripts, so restoring Pac-Sim means porting the recorder onto the
current one rather than reverting a patch.

Until that lands, `--pacsim` stops with an error rather than running. That is
deliberate: an unsampled run produces output that looks perfectly ordinary and
says nothing about the sampling not having happened.

# Citation

If you use or improve PacSim, please cite our TACO 2024 paper.
```bibtex
@article{liu2024pacsim,
  title={Pac-Sim: Simulation of multi-threaded workloads using intelligent, live sampling},
  author={Liu, Changxi and Sabu, Alen and Chaudhari, Akanksha and Kang, Qingxuan and Carlson, Trevor E},
  journal={ACM Transactions on Architecture and Code Optimization (TACO)},
  volume={21},
  number={4},
  pages={1--26},
  year={2024}
}
```
