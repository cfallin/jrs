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

Installation
============

Compiling
---------

1. Install OpenSSL and APR (Apache Portable Runtime). On Ubuntu/Debian, these
   packages are `libssl-dev` and `libapr1-dev`.

2. Install the `scons` build tool if you do not already have it.

3. Compile with `scons`. This will produce a static binary `jrs` suitable for
   distribution to a compute cluster without the need to install the
   dependencies on each node.

Installing
----------

JRS can be installed either as a Debian package or "from scratch". To build a
Debian package, do the following:

    $ ./build-deb.sh
    $ dpkg -i jrs_1.0-1_amd64.deb   # for example

Alternatively, simply do `make install` to install JRS.

The JRS daemons should be started on boot for an ordinary compute cluster
configuration. JRS installs an init-script at `/etc/init.d/jrs-daemon`. This
script can be used to manually start/stop/restart the daemon(s) as well.

Configuration
-------------

JRS requires one "metadata server", which coordinates resource sharing among
all clients, and one or more "compute nodes", which actually run jobs. To
configure JRS, first determine which nodes should serve each of these roles.

1. Configure the master by editing `node.master` in the JRS installation
   directory. This file should be updated on all nodes if the master is
   changed.

2. Configure the compute node list by editing `node.list` in the JRS
   installation directory. This file should also be propagated to all nodes.

3. Set up a shared secret among all machines. This shared secret is used to
   establish encrypted communcation between clients, the metadata server and
   the compute nodes. Place random data of arbitrary length in the `secret`
   file and propagate it to all nodes. For example:

       dd if=/dev/random of=secret bs=256 count=1

Using
=====

Once JRS installed, its use is very simple. Just use `jrs-submit` with a Condor
submit file:

    $ jrs-submit test.condor

The submit process will run, and show status updates, until all jobs are done.
If the process is killed and restarted, it will restart all jobs, so make sure
to run it on a node that will remain stable.

Authorship and License
======================

JRS is fully original code Copyright (c) 2013 Chris Fallin <cfallin@c1f.net>.
It is released under the GNU General Public License, version 2 or later.
