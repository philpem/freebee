# freebee
FreeBee - AT&amp;T 3B1 emulator

FreeBee is an emulator for the AT&T 3B1. It's a work-in-progress, but currently works well enough to boot the operating system.


## Maintained by

Phil Pemberton -- <philpem@philpem.me.uk>


## Things which are emulated...

  * Revision P5.1 motherboard with 68010 processor, WD2010 hard drive controller and P5.1 upgrade.
  * 720x348 pixel monochrome bitmapped graphics.
  * 4MB RAM (2MB on the motherboard, 2MB on expansion cards).
    * This is the maximum allowed by the memory mapper.
  * Keyboard and mouse.
  * WD2010 MFM Winchester hard disk controller.
    * Maximum 1400 cylinders (limited by the UNIX OS, see [the UNIX PC FAQ, section 5.6](http://www.unixpc.org/FAQ)).
    * Heads fixed at 16.
    * Sectors per track fixed at 17.
    * Fixed 512 bytes per sector.
  * WD2797 floppy disk controller.
    * Double-sided, 512 bytes per sector, 10 sectors per track, any number of tracks.
  * Realtime clock.
    * Reading the RTC reads the date and time from the host.
    * Year is fixed at 1987 due to Y2K issues in the UNIX PC Kernel.


## Things which aren't emulated fully (or at all)

  * Serial ports (or Combo Card)
  * Printer port
  * Modem
    * You will get errors that '/dev/ph0 cannot be opened' and that there was a problem with the modem. Ignore these.
  * Second hard drive (WD2010 driver stores the drive-select state, but doesn't use it)


# Build instructions

  - Install the `libsdl2-dev` package
  - Clone a copy of Freebee (remember to check out the submodules too)
    - `git clone --recurse-submodules https://github.com/philpem/freebee`
  - Build Freebee (run 'make')


# Running Freebee

  - Download the 3B1 ROMs from Bitsavers: [link](http://bitsavers.org/pdf/att/3b1/firmware/3b1_roms.zip)
  - Download the 3B1 Foundation disk set from Bitsavers: [here](http://bitsavers.org/bits/ATT/unixPC/system_software_3.51/)
    * The disk images on unixpc.org don't work: the boot track is missing.
    * Use the replacement version of the `08_Foundation_Set_Ver_3.51.IMD` image we have provided in the `os-install` directory instead of the one in the Foundation set.
  - Unzip the ROMs ZIP file and put the ROMs in a directory called `roms`:
    * Rename `14C 72-00616.bin` to `14c.bin`.
    * Rename `15C 72-00617.bin` to `15c.bin`.
  - Create a hard drive image file:
    * `dd if=/dev/zero of=hd.img bs=512 count=$(expr 17 \* 8 \* 1024)`
    * This creates a "Miniscribe 64MB" (CHS 1024:8:17, 512 bytes per sector).
    * Note that you need the Enhanced Diagnostics disk to format 16-head hard drives.
  - A bootable copy of the Enhanced Diagnostics disk is supplied in
    `enhanced_diag/bootable-extended-diag.img.gz`. Uncompress it before
    using.
  - Install the operating system
    * Follow the instructions in the [3B1 Software Installation Guide](http://bitsavers.org/pdf/att/3b1/999-801-025IS_ATT_UNIX_PC_System_Software_Installation_Guide_1987.pdf) to install UNIX.
    * Copy `01_Diagnostic_Disk_Ver_3.51.IMD` to `discim` in the Freebee directory.
    * To change disks:
      * Press F11 to release the disk image.
      * Copy the next disk image as `discim` in the Freebee directory.
      * Press F11 to load the disk image.
    * Instead of `08_Foundation_Set_Ver_3.51.IMD` use
      `08_Foundation_Set_Ver_3.51_no_phinit.IMD` in the `os-install` directory.
      This will allow the emulated Unix PC to come all the way up to
      a login prompt after the installation.
  - After installation has finished (when the login prompt appears):
    * Log in as `root`
    * `cd /etc`
    * `cp rc rc.old`
    * `sed 's/.phinit .modeminit//' rc.old > rc`
    * `reboot`
    * The above commands disable the phone and modem initialisation, which crash due to un-emulated hardware.
  - Files can be imported using the `msdos` command which allows reading a 360k MS-DOS floppy image.
    * Use dosbox to copy files to a DOS disk image (`discim`).


# Keyboard commands

  * F10 -- Grab/Release mouse cursor
  * F11 -- Load/Unload floppy disk image
  * Alt-F12 -- Exit


# Useful links

  * [AT&T 3B1 Information](unixpc.taronga.com) -- the "Taronga archive".
    * Includes the STORE, comp.sources.3b1, XINU and a very easy to read HTML version of the 3B1 FAQ.
    * Also includes (under "Kernel Related") tools to build an Enhanced Diagnostics disk which allows formatting hard drives with more than 8 heads or 1024 cylinders.  (We have provided a bootable disk in the `enhanced-diag` directory.)
  * [unixpc.org](http://www.unixpc.org/)
  * Bitsavers: [documentation and firmware (ROMs)](http://bitsavers.org/pdf/att/3b1/), [software](http://bitsavers.org/bits/ATT/unixPC/)

# Other Notes

  * To make an MS-DOS disk under Linux (9 tracks per sector):

	dd if=/dev/zero of=dos.img bs=1k count=360
	/sbin/mkfs.fat dos.img
	sudo mount -o loop -t msdos dos.img /mnt
	... copy files to /mnt ...
	sudo umount /mnt

  * To make a 10 track per sector disk image, just use `count=400` in the `dd` command and then format the disk under Unix with `iv` and `mkfs`.

