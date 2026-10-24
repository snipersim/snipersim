/* Pac-Sim in the Pin recorder: the adapter between the recorder and the
 * front-end-independent sampler in sift/pacsim_sampler.h.  Enabled by
 * -sniper:pacsim 1 (record-trace passes it for run-sniper --pacsim);
 * sampling runs inside the region of interest only. */
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
