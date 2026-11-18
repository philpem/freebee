# Using the FreeBee MCP server

FreeBee has a built-in [MCP](https://modelcontextprotocol.io/) server.
With it, an AI agent can work an AT&T UNIX PC (3B1, PC 7300) like a person
at the keyboard:

- look at the screen
- type, press the 3B1's own keys, and use the mouse
- log in on the serial port and run commands
- change floppies
- record videos with sound, ready for YouTube
- look inside the 68010: registers, memory, breakpoints

It works on macOS and Linux.

## How it works

- **Every FreeBee offers MCP.** It is on by default. Each FreeBee opens a
  socket in `/tmp/freebee-$UID/`, a folder only your user can open.
- **`freebee -mcp-bridge` is the bridge.** It is the one MCP server your
  agent starts. It finds all running FreeBees and passes each tool call to
  the VM you name.
- **VMs can come and go.** The bridge keeps running.

## Setup

1. Build FreeBee: `make` (see [README.md](README.md)).
2. Add the bridge to your agent once.
   - Claude Code:
     `claude mcp add freebee -s user -- /path/to/freebee -mcp-bridge`
   - Other MCP clients: add a stdio server with that command.
3. Start a VM from a shell, in the folder with its `.freebee.toml`.
4. In the agent: run `vm-list`, then `vm-info` and `screenshot`.

**Headless works.** Start FreeBee with `FREEBEE_HEADLESS=1` and there is
no window, but the agent still sees the screen. Add `SDL_AUDIODRIVER=dummy`
for no sound on the host either (recordings still get the sound).

## Settings

| Setting | What it does |
|---|---|
| `[mcp]` `enabled` | `false` turns the MCP server off (default: `true`) |
| `[mcp]` `name` | The VM's name in the tools (default: the folder's name) |
| `FREEBEE_MCP=0` | Turns MCP off; `FREEBEE_MCP=1` turns it on, whatever the file says |
| `FREEBEE_NAME=x` | The VM's name, over `[mcp]` `name` |
| `FREEBEE_HEADLESS=1` | No window |
| `[cpu]` `turbo` | The CPU runs N times as fast as a real 3B1's 10 MHz, while time stays real (default: 1) |
| `[modem]` `present` | Whether the kernel finds the built-in modem (default: `true`). See [The beep](#the-beep) |

## Tools

Every tool takes `vm`: a name or PID from `vm-list`. You may leave it out
when only one VM runs.

| Tool | What it does |
|---|---|
| `vm-list` | List the running VMs: name, PID, folder, who started it |
| `vm-wait` | Wait for a VM to start (`up`), end (`gone`) or start again (`restarted`) |
| `vm-info` | Describe the VM: run state, speed, guest time, disks, floppy, serial pty, mouse, recording. Use it first |
| `vm-power` | `pause`, `resume`, `reset` or `quit` |
| `vm-speed` | `real` time or `max` (as fast as the host can, for installs); in real time, `turbo` makes the CPU N times as fast |
| `screenshot` | A picture of the screen (PNG); `region` crops, `scale` zooms, `aspect` shows it 4:3 like the monitor, `pointer` marks the mouse |
| `wait-screen` | Wait until the screen changes, settles, or both. Use it instead of sleeping |
| `key` | Press keys and combinations, with the 3B1's own keys (Enter, Exit, Cancl, Cmd, Help, Page, Beg, End...) |
| `type-text` | Type a string (US layout) |
| `mouse-move`, `mouse-click`, `mouse-drag`, `mouse-button` | Use the mouse |
| `pointer-locate` | Find where the mouse pointer really is |
| `serial-write`, `serial-read`, `serial-expect` | Write, read and wait for text on the serial port tty000 |
| `serial-run` | Run a shell command on tty000: get its output and exit status in one call |
| `media` | Insert or eject a floppy (raw or ImageDisk `.IMD`), or change the second hard disk |
| `sound` | Hear the beeper and the dialer: when, how long, which tones |
| `record` | Record a video with sound for YouTube: chapters, captions, a contact sheet and an ffmpeg command |
| `debug-registers` | The 68010's registers and the next instructions |
| `memory-read`, `memory-write` | Read or patch memory: RAM, video RAM, the page map, the boot ROM (read) |
| `disassemble` | 68010 code, as the CPU sees it |
| `debug-run` | Run until the CPU gets to an address (up to 16 breakpoints) |
| `debug-step` | Run some instructions, then stop |
| `register-write` | Set a register of the stopped CPU |
| `steps` | Run several tools in one call; stop at the first wait that is not met |

## How to drive a 3B1

The server sends these rules to every agent that connects
([src/mcp/instructions.txt](src/mcp/instructions.txt)):

- **Look before you act.** Take a screenshot first. The keyboard and
  mouse tools take `then`, so they can wait and look in the same call.
- **Don't sleep.** Use `wait-screen` or `serial-expect`. One call can
  wait up to 25 minutes. The blinking cursor does not count as a change.
  The clock on the top line changes every minute: for long waits, put it
  in `ignore`: `{"x":280,"y":0,"w":200,"h":16}`.
- **Return and Enter are different keys.** Shell lines end with Return.
  The Office (the UA) takes Enter, Exit, Cancl, Cmd, Help and the arrows.
- **Commands go over the serial port.** Log in with `serial-write`
  `"root\n"`. The login asks "Please type the terminal name": answer
  `vt100`. Then `serial-run` returns each command's output and exit
  status. It copes with `#` and `@`, which the serial tty takes as its
  erase and kill keys. Long or multi-line commands go through a script,
  sent with XON/XOFF flow control, so nothing is lost.
- **The mouse is relative.** The tools home the pointer to the top-left
  corner, then move it one pixel a count. The guest must have turned the
  mouse on (the Office does). If a click misses, use `pointer-locate`.
- **The pixels are tall.** The screen is 720x348, shown 4:3 on the
  monitor. Tool coordinates are always screen pixels.
- **Never stop a VM you did not start**, unless the user asked.

A login in one call:

```json
{"steps": [
  {"tool": "serial-write", "args": {"text": "root\n"}},
  {"tool": "serial-expect", "args": {"text": "RETURN:"}},
  {"tool": "serial-write", "args": {"text": "vt100\n"}},
  {"tool": "serial-expect", "args": {"text": "# "}},
  {"tool": "serial-run", "args": {"command": "uname -a"}}
]}
```

## Recording videos

1. `record` `start`. Options:
   - `file`: the name, without an extension (default: `freebee-take-<date>-<time>`)
   - `color`: the phosphor, `white`, `green` or `amber`
   - `audio`: the beeper and dialer (default: on)
   - `freeze`: the VM runs only during tool calls. The video then has no
     pauses while the agent thinks. Let it run with a wait or a mark's
     `hold-ms`.
   - `skip-still-ms`: cut stretches where nothing moves or sounds
     longer than this
2. Use the tools as usual. While it records, `type-text` types like a
   person and the mouse moves like a hand.
3. `record` `mark` as you go:
   - `chapter`: a chapter title, for the YouTube description
   - `caption`: a subtitle, with `duration-ms`
   - `zoom`: a close-up of `region`, for the edit
   - `note`: a note for the edit
   - `hold-ms`: record this much more before the next call
4. `record` `stop`. It returns:
   - the video, `<file>.avi`: lossless ZMBV, 60 frames a second, with
     44.1 kHz sound (a new part after 1 GiB)
   - `<file>.json`: the files and marks, for the edit
   - `<file>.srt`: the captions as subtitles
   - the chapters, ready to paste into YouTube
   - an ffmpeg command that renders it for YouTube: 4K, sharp pixels,
     4:3 in a 16:9 frame
   - a contact sheet: pictures from across the video

The video runs in guest time. A paused VM records nothing, and
`vm-speed max` does not make the video faster.

The render command looks like this:

```sh
ffmpeg -i take.avi \
  -vf 'scale=iw*7:ih*7:flags=neighbor,scale=2880:2160:flags=area,setsar=1,pad=3840:2160:480:0' \
  -c:v libx264 -preset slow -crf 18 -pix_fmt yuv420p \
  -c:a aac -b:a 192k -ar 48000 -movflags +faststart take.mp4
```

## The beep

The 3B1 has no separate beeper: the kernel's `beep()` plays a tone on the
phone dialer. The kernel times that tone from its phone scan, and starts
the phone scan only when its boot probe finds the built-in modem (it raises
DTR and wants DSR back).

FreeBee does not emulate the modem. With `[modem]` `present = true` (the
default), the probe finds a modem whose DSR simply follows DTR. The kernel
then starts its phone scan, and the 3B1 beeps. With `false`, the probe
fails as it always did, and the 3B1 never beeps.

## Debugging

- `debug-run` stops before the instruction at an address. Then
  `debug-step`, `debug-registers`, `memory-read` and `disassemble` show
  what is going on; `vm-power` `resume` lets it go on.
- Virtual addresses (below 0x400000) go through the 3B1's page map, as
  the CPU sees them. `physical: true` reads RAM itself. Video RAM is at
  0x420000, the boot ROM at 0x800000.
- For the kernel's symbols, run `nm /unix` in the guest with `serial-run`.
  Its addresses are decimal.

## Where the code is

| File | What it holds |
|---|---|
| `src/mcp/server.c` | The socket in each FreeBee, the registry, the serial ring, the screen history |
| `src/mcp/tools.c` | The tools |
| `src/mcp/record.c` | The video recorder (ZMBV AVI, sound, contact sheet) |
| `src/mcp/bridge.c` | `freebee -mcp-bridge` |
| `src/mcp/tools.json` | The tools' descriptions and arguments |
| `src/mcp/instructions.txt` | The rules sent to every agent |
| `src/keys.c` | The key names and the key queue (shared with the control socket) |

## Limits

- Text on the screen is pixels: read it from screenshots.
- No snapshots, no network.
- One serial port, tty000.
- `zoom` marks are notes for the edit: the render command does not zoom.
