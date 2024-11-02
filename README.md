# The Sniper Multi-Core Simulator

<img align="left" width="18%" alt="sniper-logo" src="https://github.com/user-attachments/assets/02b344ad-2163-4b4f-abb1-b0a0724b7fa9">

This is the source code for the Sniper multicore simulator originally developed
by the Performance Lab research group at Ghent University, Belgium.
Please refer to the NOTICE file in the top-level directory for
licensing and copyright information.

Sniper is a next-generation parallel, high-speed, and accurate x86 simulator. This multi-core simulator 
is based on the interval core model and the [Graphite](https://github.com/mit-carbon/Graphite) simulation 
infrastructure, allowing for fast and accurate simulation and trading off simulation speed for accuracy to 
allow a range of flexible simulation options when exploring different homogeneous and 
heterogeneous multi-core architectures.

The Sniper simulator allows one to perform timing simulations for both multi-program workloads and multi-threaded, 
shared-memory applications with 10s to 100+ cores at a high speed when compared to existing simulators. The main 
feature of the simulator is its core model which is based on interval simulation, a fast mechanistic core model. Interval simulation 
raises the level of abstraction in architectural simulation, which allows for faster simulator development and 
evaluation times; it does so by 'jumping' between miss events, called intervals. Sniper has been validated against multi-socket 
Intel Core2 and Nehalem systems, and provides average performance prediction errors within 25% at a simulation speed of up 
to several MIPS.

This simulator and the interval core model are useful for uncore and system-level studies that require more detail than the 
typical one-IPC models, but for which cycle-accurate simulators are too slow to allow workloads of meaningful sizes to be simulated. 
As an added benefit, the interval core model allows the generation of CPI stacks, which show the number of cycles lost due to 
different characteristics of the system, like the cache hierarchy or branch predictor, and leads to a better understanding of each 
component's effect on total system performance. This extends the use of Sniper to application characterization and hardware/software co-design.

For additional information, please see our website: <https://snipersim.org>

## Getting started

A good starting point is <https://snipersim.org/w/Getting_Started>, and for
more information about building the simulator and usage, please see 
<https://snipersim.org/w/Manual>.

## Publications

If you are using Sniper, please let us know by posting a message on
our user forum.  If you use Sniper 6.0 or later in your research,
(if you are using the Instruction-Window Centric core model, etc.),
please acknowledge us by referencing our TACO 2014 paper:

Trevor E. Carlson, Wim Heirman, Stijn Eyerman, Ibrahim Hur, Lieven
Eeckhout, "An Evaluation of High-Level Mechanistic Core Models".
In ACM Transactions on Architecture and Code Optimization (TACO),
Volume 11, Issue 3, October 2014, Article No. 28
http://dx.doi.org/10.1145/2629677

If you are using earlier versions of Sniper, please acknowledge
us by referencing our SuperComputing 2011 paper:

Trevor E. Carlson, Wim Heirman, Lieven Eeckhout, "Sniper: Exploring
the Level of Abstraction for Scalable and Accurate Parallel Multi-Core
Simulation". Proceedings of the International Conference for High
Performance Computing, Networking, Storage and Analysis (SC),
pages 52:1--52:12, November 2011.
http://dx.doi.org/10.1145/2063384.2063454

## Sniper Resources

More information on Sniper benchmarks and tutorials can be found at 
<https://github.com/snipersim/benchmarks> and <https://snipersim.org/w/Sniper_Tutorials>.
Also, check out <https://looppoint.github.io/> for our efforts on sampled simulation.

## Getting Help, Reporting bugs, and Requesting Features

Given below are some of the most common channels that we provide for users and developers to get help, report
bugs, request features, or engage in community discussions. 

* **Google Groups**: A Google Groups page that can be used to start
discussions or ask questions. Available at <https://groups.google.com/g/snipersim>.
* **GitHub Issues**: A GitHub Issues page for reporting bugs or requesting
features. Available at <https://github.com/snipersim/snipersim/issues>.
