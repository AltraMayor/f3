## Compile stable software on Linux, Apple Mac, Windows/Cygwin, and FreeBSD

```
make
```
If you want to install f3write and f3read, run the following command:

```
make install
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


## Use example of f3write/f3read

```
$ ./f3write /media/michel/5EBD-5C80/
$ ./f3read /media/michel/5EBD-5C80/
```

Please replace "/media/michel/5EBD-5C80/" with the appropriate path.
USB devices are mounted in "/Volumes" on Macs.

If you have installed f3read and f3write, you can remove the "./"
that is shown before their names.

For more information see http://oss.digirati.com.br/f3/


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

### Bash scripts

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
