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
from a directory of output, `tools/dumpstats.py -d <dir>` reports it next to the
ordinary statistics whenever the directory holds Pac-Sim region data
(`pacsim.estimated_sim_time`, `pacsim.wall_time`, `pacsim.l2_miss_rate`;
`--pacsim-segs` lists the regions), and `tools/pacsim.py -d <dir>` prints the
estimate alone; all three paths call the same function.

## The recorder

Pac-Sim's recorder side -- the online BBV clustering and the intra-barrier
analysis that make the sampling adaptive rather than periodic -- lives in
`sift/recorder/pacsim.{h,cc}` and is built into the SDE recorder.  If
`--pacsim` is given to a recorder that does not carry it (an old build),
`record-trace` stops with an error rather than tracing unsampled: an
unsampled run produces output that looks ordinary and says nothing about
the sampling not having happened.

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
