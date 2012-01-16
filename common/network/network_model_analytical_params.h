#ifndef NETWORK_MODEL_ANALYTICAL_PARAMS_H
#define NETWORK_MODEL_ANALYTICAL_PARAMS_H

// These are retrieved from global config object
struct NetworkModelAnalyticalParameters
{
   double Tw2;         // wire delay between adjacent nodes on mesh
   double s;           // switch delay, relative to Tw2
   SInt32 n;              // dimension of network
   double W;           // channel width
   SubsecondTime update_interval; // interval in time between utilization updates
   SubsecondTime proc_cost;
};

#endif // NETWORK_MODEL_ANALYTICAL_PARAMS_H
