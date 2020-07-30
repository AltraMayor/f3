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

.. highlight:: bash

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

f3write and f3read can be installed on Windows, but currently f3probe, f3fix,
and f3brew `require Linux <#the-extra-applications-for-linux>`__.  To use them
on a Windows machine, use the `Docker Installation <#docker>`__.  For f3write
and f3read, read on.

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

f3write and f3read can be installed on Mac, but currently f3probe, f3fix, and
f3brew `require Linux <#the-extra-applications-for-linux>`__.  To use them on
Mac, use the `Docker Installation <#docker>`__.  For f3write and f3read, read
on.

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

Docker
------

Quick Start
~~~~~~~~~~~

A pre-built `image <https://cloud.docker.com/repository/docker/peron/f3>`__
is available over at Docker Hub, ready to be used.  With docker started,
run::

    docker run -it --rm --device <device> peron/f3:latest <f3-command> [<f3-options>] <device>

For example, to probe a drive mounted at /dev/sdb::

    docker run -it --rm --device /dev/sdb peron/f3:latest f3probe --destructive --time-ops /dev/sdb

Drive Permissions / Passthrough
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Getting the drive device to map into the Docker container is tricky for Mac and
Windows.  Passing through devices on Mac and Windows is a well-documented issue
(`[github]
<https://github.com/docker/for-mac/issues/3110#issuecomment-456853036>`__
`[stackexchange]
<https://devops.stackexchange.com/questions/4572/how-to-pass-a-dev-disk-device-on-macos-into-linux-docker/6076#6076>`__
`[tty]
<https://christopherjmcclellan.wordpress.com/2019/04/21/using-usb-with-docker-for-mac/#tldr>`__)
On Linux it should just work, but on Mac or Windows, Docker tends to map the
drive as a normal directory rather than a mounted drive and you will get an
error like :code:`f3probe: Can't open device '/opt/usb': Is a directory`, that
is if you can map it at all.

To solve this, we can use docker-machine to create a VirtualBox VM
(boot2docker), in which to run the Docker container.  Since VirtualBox *can*
handle device pass-through, we can pass the device through to the VirtualBox VM
which can then pass the device through to the Docker container.  Milad Alizadeh
wrote up some good instructions `here
<https://mil.ad/docker/2018/05/06/access-usb-devices-in-container-in-mac.html>`__
which are geared towards USB devices, but it shouldn't be too hard to adapt to
other drive types.  Here's what I typed into my Mac terminal (probably
similar for Windows, but untested)::

    docker-machine create -d virtualbox default
    docker-machine stop
    vboxmanage modifyvm default --usb on
    docker-machine start
    vboxmanage usbfilter add 0 --target default --name flashdrive --vendorid 0x0123 --productid 0x4567
    eval $(docker-machine env default)


For the usbfilter add command, note that the "name" argument is the new name
you're giving the filter so you can name it whatever you want.
:code:`--vendorid` and :code:`--productid` can be found on Mac in "System
Information" under "USB". You can also try searching for the right device in
:code:`vboxmanage list usbhost`.

Alternatively, you may opt to add the device through the VirtualBox GUI
application instead::

    docker-machine create -d virtualbox default
    docker-machine stop
    # open VirtualBox and manually add the drive device before proceeding to the next command
    docker-machine start
    eval $(docker-machine env default)

Once you've run the above commands, unplug and replug the flash drive and run::

    docker-machine ssh default "lsblk"

to list the devices. Search for the correct drive - the "SIZE" column may be
helpful in locating the device of interest. For example, :code:`sdb` is a common
mount point for a USB drive.  Now you should be able to run the command from
Quick Start::

    docker run --rm -it --device /dev/sdb peron/f3 f3probe --destructive --time-ops /dev/sdb

You may find it useful to enter a bash prompt in the Docker container to poke
around the filesystem::

    docker run --rm -it --device /dev/sdb peron/f3 bash

so that you can run commands like :code:`ls /dev/*`.

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

Thanks to our growing community of flash fraud fighters,
we have the following graphical user interfaces (GUI) available for F3:

`F3 QT <https://github.com/zwpwjwtz/f3-qt>`__ is a Linux GUI that uses
QT. F3 QT supports ``f3write``, ``f3read``, ``f3probe``, and ``f3fix``. Author:
Tianze.

Please support the above project by testing it and giving feedback to their
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
