# Fixed Installation Disk

`08_Foundation_Set_Ver_3.51_no_phinit.IMD.gz` is a replacement for
`08_Foundation_Set_Ver_3.51.IMD` is a replacement for in the Foundation
Software set for the 3B1.  Use `gunzip` to decompress it before using
it to install the OS.

The file was created by hex-editing the original to avoid calls
to `phinit`/`modeminit` at startup.  Using this disk allows the
emulated OS to come fully up to a login prompt at boot.

Thanks to Jesse Booth for the work!

#### Last Modified:
Thu Nov 26 10:50:20 IST 2020
