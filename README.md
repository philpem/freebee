# freebee
FreeBee - AT&amp;T 3B1 emulator

FreeBee is an emulator for the AT&T 3B1. It's a work-in-progress, but currently works well enough to boot the operating system and to compile programs with the standard C compiler.


## Maintained by

Phil Pemberton -- <philpem@philpem.me.uk>

## Limitations

### Mac OS X support

There have historically been a few instability issues on the Mac OS X platform, related to the SDL libraries. I've not heard much about them.

Unfortunately I don't have a recent Mac, so I won't be able to reproduce or test issues which affect OS X versions later than High Sierra.
Bugs will likely be left open until someone with a more recent system is able to look into them (unless you open a pull request with a fix!)

If you have an issue on OSX and have a Windows or Linux system, please try to reproduce the issue on there as it will be easier for me to test.
Please list all the platforms you've tested on (and the result) in the issue.

In summary: all support is on a best-effort basis, but **I cannot guarantee that bugs reported solely on Mac OS X will be fixed as I don't have the equipment to test with.**


### Memory mapper emulation inaccuracy

There is a workaround in the memory mapping emulation, which allows supervisor-mode writes to low memory. If this is disabled, the kernel will fail to boot
with a `PAGEIN` or `PAGEOUT` panic.

If anyone can figure this out, 


## Things which are emulated...

  * Revision P5.1 motherboard with 68010 processor, WD2010 hard drive controller and P5.1 upgrade.
  * 720x348 pixel monochrome bitmapped graphics.
  * 4MB RAM (2MB on the motherboard, 2MB on expansion cards).
    * This is the maximum allowed by the memory mapper.
  * Keyboard and mouse.
  * WD2010 MFM Winchester hard disk controller.
    * Two separate drives.
    * Maximum 1400 cylinders (limited by the UNIX OS, see [the UNIX PC FAQ, section 5.6](http://www.unixpc.org/FAQ)).
    * Heads fixed at 8.
    * Sectors per track fixed at 17.
    * Fixed 512 bytes per sector.
    * Those numbers are the default configuration; see below for more information.
  * WD2797 floppy disk controller.
    * Double-sided, 512 bytes per sector, 10 sectors per track, any number of tracks.
  * Realtime clock.
    * Reading the RTC reads the date and time from the host.
    * Year is fixed at 1987 due to Y2K issues in the UNIX PC Kernel.
  * Serial port.
    * Linux only: file 'serial-pty' is symlinked to PTY that can be used to access tty000
    * Usage instructions: [README.serial.md](README.serial.md)



## Things which aren't emulated fully (or at all)

  * Printer port
  * Modem
    * You will get errors that '/dev/ph0 cannot be opened' and that there was a problem with the modem. Ignore these.


# Build instructions

  - Install the `libsdl2-dev` package
  - Clone a copy of Freebee (remember to check out the submodules too)
    - `git clone --recurse-submodules https://github.com/philpem/freebee`
  - Build Freebee (run 'make')


# Running Freebee

## Initial Setup
  - Download the 3B1 ROMs from Bitsavers: [link](http://bitsavers.org/pdf/att/3b1/firmware/3b1_roms.zip)
  - Unzip the ROMs ZIP file and put the ROMs in a directory called `roms`:
    * Rename `14C 72-00616.bin` to `14c.bin`
    * Rename `15C 72-00617.bin` to `15c.bin`

## Option 1: Use an existing drive image
  - Arnold Robbins created a drive image installed with all sorts of tools: [here](https://www.skeeve.com/3b1/)
  - David Gesswein created a drive image for the VCF Museum: [here](http://www.pdp8online.com/3b1/demos.shtml)
  - Uncompress either of these images in the Freebee directory and rename the image to `hd.img`, or create a `.freebee.toml` file pointing to the image. (See the CONFIGURATION section of the man page.)

## Option 2: Do a fresh install
  - Download the 3B1 Foundation disk set from Bitsavers: [here](http://bitsavers.org/bits/ATT/unixPC/system_software_3.51/)
    * The disk images on unixpc.org don't work: the boot track is missing.
    * Use the replacement version of the `08_Foundation_Set_Ver_3.51.IMD` image which is available [here](https://www.skeeve.com/3b1/os-install/index.html).
  - Create a hard drive image file:
    * Use the `makehdimg` program supplied in the `tools` directory to create an initial `hd.img` file with the number of cylinders, heads and sectors per track that you want.  Limits: 1400 cylinders, 16 heads, 17 sectors per track.
    * When using the diagnostics disk to initialize the hard disk, select "Other" and supply the correct values that correspond to the numbers used with `makehdimg`.
    * Alternatively, you can use `dd if=/dev/zero of=hd.img bs=512 count=$(expr 17 \* 8 \* 1024)` to create a disk matching the compiled-in defaults. Initialize the disk using the "Miniscribe 64MB" (CHS 1024:8:17, 512 bytes per sector) choice.
    * The second hard drive file is optional. If present, it should be called `hd2.img`.  You can copy an existing `hd.img` to `hd2.img` as a quick way to get a disk with a filesystem already on it. When Unix is up and running, use `mount /dev/fp012 /mnt` to mount the second drive. You may want to run `fsck` on it first, just to be safe.
  - You can also use the ICUS Enhanced Diagnostics disk. A bootable copy is
  available [here](https://www.skeeve.com/3b1/enhanced-diag/index.html).
  Uncompress it before using.
  - Install the operating system:
    * Follow the instructions in the [3B1 Software Installation Guide](http://bitsavers.org/pdf/att/3b1/999-801-025IS_ATT_UNIX_PC_System_Software_Installation_Guide_1987.pdf) to install UNIX.
    * Copy `01_Diagnostic_Disk_Ver_3.51.IMD` to `floppy.img` in the Freebee directory.
    * If you wish to increase the swap space size, do so with the diagnostics
      disk before installing the OS. See these [instructions](https://groups.google.com/g/comp.sys.att/c/8XLILI3K8-Y/m/VxVMJNdt9NQJ).
    * To change disks:
      * Press F11 to release the disk image.
      * Copy the next disk image to `floppy.img` in the Freebee directory.
      * Press F11 to load the disk image.
    * Instead of `08_Foundation_Set_Ver_3.51.IMD` use `08_Foundation_Set_Ver_3.51_no_phinit.IMD` from [here](https://www.skeeve.com/3b1/os-install/index.html).
      This will allow the emulated Unix PC to come all the way up to
      a login prompt after the installation.

## Importing files
  - Files can be imported using the 3b1 `msdos` command which allows reading a 360k MS-DOS floppy image.
    * Use dosbox to copy files to a DOS disk image named `floppy.img`. This image must be in the same directory as the Freebee executable (or path specified in the .freebee.toml config file).
    * If the floppy.img file wasn't present on boot or was updated, hit F11 to load/unload the floppy image.
    * Run `msdos` from the 3b1 command prompt, grab the mouse cursor with F10 if you haven't already, then COPY files to the hard drive.
  - Another option is to use the s4tools [here](https://github.com/dgesswein/s4-3b1-pc7300) which allow you to export the file system image out of the disk image and import the fs image back. In particular, there is an updated `sysv` Linux kernel module which allows mounting the fs image as a usable filesystem under Linux.

## Scaling the display

You can scale the display by setting scale factors in the `.freebee.toml` file.
Scale values must be greater than zero and less than or equal to 45. This
facility is useful on large displays.

# Keyboard commands

  * F10 -- Grab/Release mouse cursor
  * F11 -- Load/Unload floppy disk image (`floppy.img`)
  * Alt-F12 -- Exit

# 3b1-specific key mappings

  * F1-F8 -- "soft keys" at bottom of screen
  * F9 -- SUSPD key (brings up list of windows)
  * Alt-Esc -- EXIT key
  * Alt-Backspace -- CANCL key
  * Page Up -- HELP key
  * Page Down -- PAGE key
  * Insert -- CMD key
  * Enter -- RETURN key
  * Alt-Enter -- ENTER key
  * Pause Break -- RESET BREAK key

# Useful links

  * [AT&T 3B1 Information](http://unixpc.taronga.com) -- the "Taronga archive".
    * Includes the STORE, comp.sources.3b1, XINU and a very easy to read HTML version of the 3B1 FAQ.
    * Also includes (under "Kernel Related") tools to build an Enhanced Diagnostics disk which provides more options formatting hard drives.
  * [unixpc.org](http://www.unixpc.org/)
  * Bitsavers: [documentation and firmware (ROMs)](http://bitsavers.org/pdf/att/3b1/), [software](http://bitsavers.org/bits/ATT/unixPC/)

# Other Notes

  * To make an MS-DOS disk under Linux (9 tracks per sector):

	<code>dd if=/dev/zero of=dos.img bs=1k count=360<br/>
	/sbin/mkfs.fat dos.img<br/>
	sudo mount -o loop -t msdos dos.img /mnt<br/>
	... copy files to /mnt ...<br/>
	sudo umount /mnt<br/></code>

  * To make a 10 track per sector disk image, just use `count=400` in the `dd` command and then format the disk under Unix with `iv` and `mkfs`.

  * See this part of the [FAQ](https://stason.org/TULARC/pc/3b1-faq/4-4-How-do-I-get-multiple-login-windows.html) on setting up multiple login windows.
