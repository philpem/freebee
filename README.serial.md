# Serial port usage (Linux)

A pseudoterminal (pty) will be created on launch of FreeBee. The file `serial-pty` in the current directory will be symlinked to the pty (e.g. /dev/pts/?). This will be the proxy for /dev/tty000 (the built-in serial port) on the 3b1.

## Logging in to 3b1 via serial port
  * 3b1: Make sure getty is running on tty000
    * Boot FreeBee and run `setgetty 000 1` as root
  * Linux: Run terminal emulator pointed at our pty
    * Install package "minicom", "picocom", "putty", or "screen" (minicom recommended)
    * Then run:
      * Minicom: `minicom -D ./serial-pty` (Ctrl-A + Q to quit)
      * Picocom: `picocom --omap delbs --send-cmd "sx -vv" --receive-cmd "rx -vv" ./serial-pty` (Ctrl-A + Ctrl-Q to quit)
      * Putty: `putty -serial ./serial-pty` (use Shift-Backspace to backspace, or configure to send ^H)
      * Screen: `screen ./serial-pty` (Ctrl-A + \\ to quit)

## Transferring files via Xmodem
  * Once a serial connection has been made, files can be transferred via Xmodem
  * If using Picocom, "rzsz" (or "lrzsz") package must be installed and "--send-cmd" and "--receive-cmd" parameters specified on cmd line
  * Sending a local file to 3b1
    * Initiate receive on 3b1 with: `umodem -rb <filename>`
    * Then quickly select file to send:
      * Minicom: Ctrl-A + s, select xmodem
      * Picocom: Ctrl-A + Ctrl-S
  * Receiving a file from 3b1
    * Send file on 3b1 with: `umodem -sb <filename>`
    * Then quickly receive file with:
      * Minicom: Ctrl-A + r, select xmodem
      * Picocom: Ctrl-A + Ctrl-R

## Connecting out from 3b1 to Linux machine via serial port
  * 3b1: Make sure getty is **NOT** running on tty000
    * Boot FreeBee and run `setgetty 000 0` as root
  * 3b1: Add the line "DIR tty000 0 9600" to /usr/lib/uucp/L-devices as root
    * e.g. as root: `echo "DIR tty000 0 9600" >> /usr/lib/uucp/L-devices`
  * Linux: Run `sudo agetty pts/? vt100` where pts/? is the pty linked to by ./serial-pty
    * e.g. if serial-pty -> /dev/pts/2, run `sudo agetty pts/2 vt100`
  * 3b1: Run `cu -l tty000` to connect to your Linux machine
    * Login to your Linux machine - when done, exiting your Linux shell should also end the agetty
    * If you'd like to login again, run the agetty command again
    * When done with cu, enter `~.` + Enter to exit cu
