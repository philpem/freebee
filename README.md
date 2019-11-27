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
    * Heads fixed at 8.
    * Sectors per track fixed at 16.
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
  * P5.1 expansion PAL --
    * 4th drive select pin
    * 2nd hard drive select


# Installation instructions

TODO.
