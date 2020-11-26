# Enhanced UNIX PC Diagnostics Disk

This is the "enhanced diagnostics" disk for the UNIX PC / 3B1 that
makes it possible to format a disk with up to 16 heads.  The disk
image is directly bootable.  When it boots, choose to boot from
the floppy and the file to boot is `/unix`.

The files here are:

* `bootable-extended-diag.img.gz`: The bootable image. Copy this file
to `discim` before booting. (Uncompress first before using.)

* `diag.img.gz`: An MS-DOS formatted floppy image with the files. This
can be used to import the files into a running system using the `msdos`
command.  (Uncompress first before using.)  It's not strictly necessary,
as the bootable image also has all the files on it.

* `Install`: A script to make the bootable disk image from the files.

* `Install.bak`: The original version. This did not install a boot loader.

* `README`: The original README file.

* `README.adr`: Arnold Robbins's additional notes.

* `s4diag`: The actual diagnostics program.

#### Last Modified:
Thu Nov 26 10:54:27 IST 2020
