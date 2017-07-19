# f3 - Fight Flash Fraud

f3 is a simple tool that tests flash cards capacity and performance to see if they live up to claimed specifications.

For more information see http://oss.digirati.com.br/f3/

# Examples

## Testing performance with f3read/f3write

Use these two programs in this order. f3write will write large files to your mounted disk and f3read will check if the flash disk contains exactly the written files.

```
$ ./f3write /media/michel/5EBD-5C80/
$ ./f3read /media/michel/5EBD-5C80/
```

Please replace "/media/michel/5EBD-5C80/" with the appropriate path.
USB devices are mounted in "/Volumes" on Macs.

If you have installed f3read and f3write, you can remove the "./"
that is shown before their names.

## Quick capacity tests with f3probe

f3probe is the fastest drive test and suitable for large disks because it only writes what's necessary to test the drive. It operates directly on the (unmounted) block device and needs to be run as a privileged user.

**Warning**: This will destroy any previously stored data on your disk!

```
# ./f3probe --destructive --time-ops /dev/sdb
```

## Correcting capacity to actual size with f3fix
f3fix creates a partition that fits the actual size of the fake drive. Use f3probe's output to determine the parameters for i3fix.
```
# ./f3fix --last-sec=16477878 /dev/sdb
```

# Installation instructions

## Compile stable software on Linux or FreeBSD

```
make
```
If you want to install f3write and f3read, run the following command:

```
make install
```


## Compile stable software on Windows/Cygwin

```
export LDFLAGS="$LDFLAGS -Wl,--stack,4000000 -largp"
make
```
If you want to install f3write and f3read, run the following command:

```
make install
```


## Compile stable software on Apple Mac

### Using HomeBrew
If you have Homebrew already installed in your computer,
the command below will install F3:
```
brew install f3
```

### Using MacPorts
If you use MacPorts instead, use the following command:
```
port install f3
```


### Compiling the lastest development version from the source code

Most of the f3 source code builds fine using XCode, the only dependency missing
is the GNU C library "argp". You can build argp from scratch, or use the version
provided by HomeBrew and MacPorts as "argp-standalone"

The following steps have been tested on OS X El Capitan 10.11.

1) Install Apple command line tools.
```
xcode-select --install
```

See http://osxdaily.com/2014/02/12/install-command-line-tools-mac-os-x/
for details.

2) Install Homebrew or MacPorts

HomeBrew:
```
/usr/bin/ruby -e "$(curl -fsSL https://raw.githubusercontent.com/Homebrew/install/master/install)"
```
See http://brew.sh/ for details.

MacPorts: https://www.macports.org/install.php


3) Install argp library.
```
brew install argp-standalone
```
See http://brewformulas.org/ArgpStandalone and
https://www.freshports.org/devel/argp-standalone/ for more information.

Or, for MacPorts:
```
port install argp-standalone
```
See https://trac.macports.org/browser/trunk/dports/sysutils/f3/Portfile for more information.

4) Set compilation flags.
These following environment variables are used in the Makefile to locate
the argp library:

HomeBrew:
```
export CFLAGS="$CFLAGS -I/usr/local/include/"
export LDFLAGS="$LDFLAGS -L/usr/local/lib/ -largp"
```
MacPorts:
```
export CFLAGS="$CFLAGS -I/opt/local/include/"
export LDFLAGS="$LDFLAGS -L/opt/local/lib/ -largp"
```

5) Build F3.
```
make
```


## The extra applications for Linux

### Install dependencies

f3probe and f3brew require version 1 of the library libudev, and
f3fix requires version 0 of the library libparted to compile.
On Ubuntu, you can install these libraries with the following command:
```
sudo apt-get install libudev1 libudev-dev libparted0-dev
```

### Compile the extra applications

```
make extra
```

NOTES:
   - The extra applications are only compiled and tested on Linux platform.
   - Please do not e-mail me saying that you want the extra applications
     to run on your platform; I already know that.
   - If you want the extra applications to run on your platform,
     help to port them, or find someone that can port them for you.
     If you do port any of them, please send me a patch to help others.
   - The extra applications are f3probe, f3brew, and f3fix.

If you want to install the extra applications, run the following command:

```
make install-extra
```

# Other resources

## Files

    changelog   - Change log for package maintainers
    f3read.1    - Man page for f3read and f3write
                In order to read this manual page, run `man ./f3read.1`
                To install the page, run
                `install --owner=root --group=root --mode=644 f3read.1 /usr/share/man/man1`
    LICENSE     - License (GPLv3)
    Makefile    - make(1) file
    README      - This file
    *.h and *.c - C code of F3

## Bash scripts

Although the simple scripts listed in this section are ready for use,
they are really meant to help you to write your own scripts.
So you can personalize F3 to your specific needs.

    f3write.h2w - Script to create files exactly like H2testw.
        Use example: `f3write.h2w /media/michel/5EBD-5C80/`

    log-f3wr    - Script that runs f3write and f3read, and records
                  their output into a log file.
        Use example: `log-f3wr log-filename /media/michel/5EBD-5C80/`

Please notice that all scripts and use examples above assume that
f3write, f3read, and the scripts are in the same folder.
