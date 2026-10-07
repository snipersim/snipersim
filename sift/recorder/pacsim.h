/* Pac-Sim: online sampled simulation, driven from the recorder.
 *
 * The recorder cuts the run into regions at loop back-edges (once a region
 * has run its instruction budget) and at OpenMP region entries, clusters
 * each finished region by its basic-block vector, and asks the simulator
 * for detail only when the region about to start looks unlike anything
 * measured so far; the rest is fast-forwarded and extrapolated from the
 * measured members of its cluster.  Regions and their measurements are
 * written to <pacsim-dir>/pacsim_regions.json, which tools/pacsim/
 * pacsim_dumpjson.py turns into the estimated run time (run-sniper prints
 * it).  Liu, Sabu, Chaudhari, Kang, Carlson, "Pac-Sim: Simulation of
 * multi-threaded workloads using intelligent, live sampling", TACO 2024.
 *
 * Enabled by -sniper:pacsim 1 (record-trace passes it for run-sniper
 * --pacsim); sampling runs inside the region of interest only. */
#ifndef PACSIM_H
#define PACSIM_H

namespace pacsim
{
   void activate();
   /* called from beginROI: sampling starts with the ROI, in detailed mode */
   void roiBegin(int tid);
   /* called from endROI before the channels are ended: the last region is
      measured and pacsim_regions.json written */
   void roiEnd(int tid);
}

#endif
