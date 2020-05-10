f3 - Fight Flash Fraud
======================

f3 is a simple tool that tests flash cards capacity and performance to
see if they live up to claimed specifications. It fills the device with
pseudorandom data and then checks if it returns the same on reading.

F3 stands for Fight Flash Fraud, or Fight Fake Flash.

**Table of Contents**

-  `Examples <#examples>`__
-  `Installation <#installation>`__
-  `Other resources <#other-resources>`__

.. _examples:

Examples
========

Testing performance with f3read/f3write
---------------------------------------

Use these two programs in this order. f3write will write large files to
your mounted disk and f3read will check if the flash disk contains
exactly the written files::

    $ ./f3write /media/michel/5EBD-5C80/
    $ ./f3read /media/michel/5EBD-5C80/

Please replace "/media/michel/5EBD-5C80/" with the appropriate path. USB
devices are mounted in "/Volumes" on Macs.

If you have installed f3read and f3write, you can remove the "./" that
is shown before their names.

Quick capacity tests with f3probe
---------------------------------

f3probe is the fastest drive test and suitable for large disks because
it only writes what's necessary to test the drive. It operates directly
on the (unmounted) block device and needs to be run as a privileged
user::

    # ./f3probe --destructive --time-ops /dev/sdb

.. warning:: This will destroy any previously stored data on your disk!

Correcting capacity to actual size with f3fix
---------------------------------------------

f3fix creates a partition that fits the actual size of the fake drive.
Use f3probe's output to determine the parameters for f3fix::

    # ./f3fix --last-sec=16477878 /dev/sdb

Docker
======

Instead of building and installing the tools, and their depending packages, in your local OS,
the tools can be run from a Docker container.

The included Dockerfile installs all tools, both the base tools, and the extras.

Build
-----

To create a Docker image, run::

  $ docker build -t f3:latest .

Running
-------

Since we're dealing with attached devices, Docker needs to run in privileged mode::

  $ docker run -it --rm --privileged -v <device>:<device> f3:latest <f3-command> [<f3-options>] <device>

The commands, and their parameters, are as otherwise described in this document.
Since the commands are installed, they should not be prefixed with the dot-slash notation.

A pre-built `image <https://cloud.docker.com/repository/docker/peron/f3>`__ is available over at Docker Hub,
ready to be used.

Installation
============

Download and Compile
--------------------

The files of the stable version of F3 are
`here <https://github.com/AltraMayor/f3/releases>`__. The
following command uncompresses the files::

    $ unzip f3-7.2.zip


Compile stable software on Linux or FreeBSD
-------------------------------------------

To build::

    make

If you want to install f3write and f3read, run the following command::

    make install

Compile stable software on Windows/Cygwin
-----------------------------------------

If you haven't already, install the following Cygwin packages and their dependencies:

- `gcc-core`
- `make`
- `libargp-devel`

To build, you need special flags::

    export LDFLAGS="$LDFLAGS -Wl,--stack,4000000 -largp"
    make

If you want to install f3write and f3read, run the following command::

    make install

Compile stable software on Apple Mac
------------------------------------

Using HomeBrew
~~~~~~~~~~~~~~

If you have Homebrew already installed in your computer, the command
below will install F3::

    brew install f3

Using MacPorts
~~~~~~~~~~~~~~

If you use MacPorts instead, use the following command::

    port install f3

Compiling the latest development version from the source code
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Most of the f3 source code builds fine using XCode, the only dependency
missing is the GNU C library "argp". You can build argp from scratch, or
use the version provided by HomeBrew and MacPorts as "argp-standalone"

The following steps have been tested on OS X El Capitan 10.11.

1) Install Apple command line tools::

       xcode-select --install

See http://osxdaily.com/2014/02/12/install-command-line-tools-mac-os-x/
for details.

2) Install Homebrew or MacPorts

   HomeBrew::

     /usr/bin/ruby -e "$(curl -fsSL https://raw.githubusercontent.com/Homebrew/install/master/install)"

   See https://brew.sh/ for details.

   MacPorts: https://www.macports.org/install.php

3) Install argp library::

       brew install argp-standalone

   See http://brewformulas.org/ArgpStandalone and
   https://www.freshports.org/devel/argp-standalone/ for more
   information.

   Or, for MacPorts::

     port install argp-standalone

   See https://trac.macports.org/browser/trunk/dports/sysutils/f3/Portfile
   for more information.

4) Build F3::

   When using Homebrew, you can just run::

       make

   When using MacPorts, you will need to pass the location where MacPorts
   installed argp-standalone::

       make ARGP=/opt/local

The extra applications for Linux
--------------------------------

Install dependencies
~~~~~~~~~~~~~~~~~~~~

f3probe and f3brew require version 1 of the library libudev, and f3fix
requires version 0 of the library libparted to compile. On Ubuntu, you
can install these libraries with the following command::

    sudo apt-get install libudev1 libudev-dev libparted0-dev

On Fedora, you can install these libraries with the following command::

    sudo dnf install systemd-devel parted-devel

Compile the extra applications
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

::

    make extra

.. note::
   - The extra applications are only compiled and tested on Linux
     platform.
   - Please do not e-mail me saying that you want the extra
     applications to run on your platform; I already know that.
   - If you want the extra applications to run on your platform, help
     to port them, or find someone that can port them for you. If you
     do port any of them, please send me a patch to help others.
   - The extra applications are f3probe, f3brew, and f3fix.

If you want to install the extra applications, run the following
command::

    make install-extra

Other resources
===============

Graphical User Interfaces
-------------------------

Thanks to our growing community of fraud fighters, we have a couple of
graphical user interfaces (GUIs) available for F3:

`F3 QT <https://github.com/zwpwjwtz/f3-qt>`__ is a Linux GUI that uses
QT. F3 QT supports ``f3write``, ``f3read``, ``f3probe``, and ``f3fix``. Author:
Tianze.

`F3 X <https://github.com/insidegui/F3X>`__ is a OS X GUI that uses
Cocoa. F3 X supports ``f3write`` and ``f3read``. Author: Guilherme
Rambo.

Please support these projects testing and giving feedback to their
authors. This will make their code improve as it has improved mine.

Files
-----

::

    changelog   - Change log for package maintainers
    f3read.1    - Man page for f3read and f3write
                In order to read this manual page, run `man ./f3read.1`
                To install the page, run
                `install --owner=root --group=root --mode=644 f3read.1 /usr/share/man/man1`
    LICENSE     - License (GPLv3)
    Makefile    - make(1) file
    README      - This file
    *.h and *.c - C code of F3

Bash scripts
------------

Although the simple scripts listed in this section are ready for use,
they are really meant to help you to write your own scripts. So you can
personalize F3 to your specific needs::

    f3write.h2w - Script to create files exactly like H2testw.
        Use example: `f3write.h2w /media/michel/5EBD-5C80/`

    log-f3wr    - Script that runs f3write and f3read, and records
                  their output into a log file.
        Use example: `log-f3wr log-filename /media/michel/5EBD-5C80/`

Please notice that all scripts and use examples above assume that
f3write, f3read, and the scripts are in the same folder.
