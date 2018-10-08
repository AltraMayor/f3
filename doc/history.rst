History
=======

I started this project when I bought a 32GB microSDHC card for my
Android phone back in 2010, and found out that this card always fails
when one fills it up. Googling about this issue, I arrived at the blogs
`Fight Flash Fraud <https://fightflashfraud.wordpress.com/>`__ and
`SOSFakeFlash <https://sosfakeflash.wordpress.com/>`__, which recommend
the software H2testw (see
`here <https://fightflashfraud.wordpress.com/2008/11/24/h2testw-gold-standard-in-detecting-fake-capacity-flash/>`__
or
`here <https://sosfakeflash.wordpress.com/2008/09/02/h2testw-14-gold-standard-in-detecting-usb-counterfeit-drives/comment-page-3/#comment-9861>`__)
to test flash memories.

I downloaded H2testw and found two issues with it: (1) it is for Windows
only, and (2) it is not open source. However, its author, Harald
Bögeholz, was kind enough to include a text file that explains what it
does, and provided the pseudo random number generator used in H2testw.

F3 is my GPLv3 implementation of the algorithm of H2testw,
and other tools that I have been implementing to speed up the
identification of fake drives as well as making them usable:
``f3probe``, ``f3fix``, and ``f3brew``. My implementation of H2testw,
which I've broken into two applications named ``f3write`` and
``f3read``, runs on Linux, Macs, Windows/Cygwin, and FreeBSD.
``f3probe`` is the fastest way to identify fake drives and their real
sizes. ``f3fix`` enables users to use the real capacity of fake drives
without losing data. ``f3brew`` helps developers to infer how fake
drives work. ``f3probe``, ``f3fix``, and ``f3brew`` currently runs only
on Linux.

Change log
----------

Starting at version 2.0, F3 supports the platform Mac. Mac users may
want to check out Thijs Kuipers'
`page <https://www.broes.nl/2012/08/verify-the-integrity-of-a-flash-sd-card-on-a-mac/>`__
for help.

Starting at version 3.0, F3 supports the platform Windows/Cygwin, and
adopts H2testw's file format. People interested in exchanging files
between F3 and H2testw should read the `section <#comp_h2testw>`__ about
it to understand the caveats.

Starting at version 4.0, F3 supports the platform FreeBSD. **Mac
users:** Version 4.0 does not compile on Macs. The issue has been fixed
on version 5.0.

Starting at version 5.0, F3 includes ``f3probe`` and ``f3fix`` as
experimental, and for Linux only.

Starting at version 6.0, F3 includes ``f3brew`` as experimental, and for
Linux only. Linux users may want to check out Vasiliy Kaygorodov's
`page <https://serverissues.com/blog/2015/12/12/finding-out-chinese-flash-disk-slash-sdhc-card-real-size/>`__
or Ahmed Essam's
`page <https://ahmedspace.com/how-to-recover-a-corrupted-flash-memory-using-f3-tools-under-linux/>`__
for help.

Starting at version 7.0, ``f3probe``, ``f3fix``, and ``f3brew`` are stable.
They are for Linux only.
