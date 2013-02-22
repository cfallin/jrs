JRS: Job-Running System
=======================

JRS, or "Job-Running System", is a very basic batch system -- that is, a suite
of network software that can spawn "jobs" on a cluster of "compute nodes". In
this context, a "job" is just a command line -- for example, a simulator or a
script to perform a mathematical computation -- and a "compute node" is just an
ordinary Linux machine sitting on a network. JRS does not presume to know about
any other infrastructure -- for example, it does not know about any shared
filesystems or any network authentication systems. Its design is intentionally
simple: its only purpose is to dispatch command lines to remote hosts.

JRS is a (distant) little cousin to mature systems such as
[Condor](http://research.cs.wisc.edu/htcondor/) and
[PBS](https://en.wikipedia.org/wiki/Portable_Batch_System). It differs from
these systems in that it is extremely small and very simple to set up.
