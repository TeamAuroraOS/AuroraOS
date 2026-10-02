# Terminal

A Linux-style shell for the SD card and the console. Press **X** on the Home
Menu to open it: output is on the top screen and a touch keyboard on the
bottom one. **START**, or the `exit` command, closes it. The scrollback, the
command history and the working folder are kept until an app is launched.

| Piece | File |
|-------|------|
| Screen, keyboard, line editing, parsing | `src/os/Terminal.c`, `include/terminal.h` |
| The commands | `src/os/TermCmds.c` |
| What the two share | `src/os/term.h` |
| File operations (shared with the File Explorer) | `src/os/FileOps.c` |
| Power off and reboot | `src/power.c` |

Contents: [The prompt](#the-prompt), [Keys](#keys),
[Command-line syntax](#command-line-syntax), [Running apps](#running-apps),
[Commands](#commands), [Limits](#limits).

## The prompt

```
Nick@n3ds:/Aurora/Apps$
```

* The name is the user name from setup, or `user` when none was set.
* The host is `n3ds` on a New 3DS or New 2DS XL and `o3ds` on any other model.
* The path is the working folder. `/` is the root of the SD card. A path longer
  than 24 characters shows as `...` and its last folder.

Text uses the built-in 8x8 font, so columns line up: 49 columns by 23 rows. The
font is ASCII only, so any other character in a file name shows as one `?`.
Long lines wrap at the edge.

Colours: the name and host use the accent colour, and the path is blue. In
listings, folders are blue, Aurora apps green, pictures magenta and sound cyan.
Errors are red and warnings yellow.

## Keys

### Buttons

| Button | Does |
|--------|------|
| **A** | Enter: run the line |
| **B** | delete the character before the cursor |
| **Y** | Tab: complete the word under the cursor |
| **L** | Shift (same as the Shift key) |
| **R** | switch between letters and symbols (same as `?123` / `abc`) |
| D-pad up / down | older / newer command from the history |
| D-pad left / right | move the cursor along the line |
| **SELECT** | cancel the line, like Ctrl+C (it shows `^C`); hold it to stop a long `cp`, `rm` or `cat` |
| **START** | close the terminal |
| **L** + **R** | screenshot, as on every screen |

### Touch keyboard

The bar at the top of the bottom screen has **PgUp** and **PgDn**, which scroll
the output back and forward by half a screen. A bar on the right edge of the top
screen shows the position while scrolled. Typing jumps back to the bottom.

Letters (`abc`), and the same keys with Shift:

```
 1 2 3 4 5 6 7 8 9 0        ! @ # $ % ^ & * ( )
 q w e r t y u i o p        Q W E R T Y U I O P
 a s d f g h j k l -        A S D F G H J K L _
 z x c v b n m , . /        Z X C V B N M < > ?
[Shift][?123][Tab][ space ][Del][Enter]
```

Symbols (`?123`):

```
 1 2 3 4 5 6 7 8 9 0
 ! @ # $ % ^ & * ( )
 ~ ` | \ < > { } [ ]
 + = _ : ; " ' , ? -
[Shift][abc][Tab][ space ][Del][Enter]
```

* **Shift** once shifts the next key only. A second tap locks capitals (the key
  reads `Caps`), and Caps shifts letters but leaves digits and punctuation
  alone. A third tap turns it off. Shift does nothing on the symbol keys.
* A key acts as soon as it is touched. Held for 0.45 s, the character keys,
  space, Del, PgUp and PgDn repeat.

### Tab completion

**Tab** (or **Y**) completes the word under the cursor:

* The first word completes to a command name.
* Any other word, or a first word with a `/` in it, completes to a file or
  folder name, ignoring letter case. Dot files are offered only when the typed
  part starts with a dot.
* One match is completed in full, followed by `/` for a folder or a space for a
  file or command. Spaces and other special characters in the name are
  escaped with `\`.
* Several matches are completed as far as they agree. When they agree no
  further, Tab lists them below the line.
* Inside an open quote, Tab does nothing.

## Command-line syntax

```
COMMAND [OPTION]... [ARGUMENT]...
```

* Words are separated by spaces.
* `"..."` keeps spaces inside one word. Within it, `\"` is a quote and `\\` a
  backslash.
* `'...'` keeps everything inside as it is.
* `\` outside quotes takes the next character as it is: `cd Nintendo\ 3DS`.
* `*` matches any run of characters and `?` any one character, in the last part
  of a path, ignoring letter case: `rm *.log`, `ls Aurora/Apps/T*`. Dot files
  match only a pattern that starts with a dot. The matches replace the pattern,
  sorted by name. A pattern that matches nothing is passed on as it is, as bash
  does. A word whose `*` and `?` are all quoted or escaped is not a pattern.
* Options are letters after `-` and can be combined: `ls -la`. `--` ends the
  options, so `rm -- -name` removes a file called `-name`.
* `COMMAND --help` prints the command's usage.
* Paths: `/` is the root of the card, and a path not starting with `/` is
  relative to the working folder. `.` is the folder itself, `..` its parent,
  and `~` (alone or as `~/...`) the root. Letter case does not matter, as on
  any FAT card.

Not supported: pipes (`|`), redirection (`<`, `>`), `;`, `&` and `&&`. A line
using any of them is refused with a message. There are no variables, so `$` is
an ordinary character.

## Running apps

```
./NAME.bin
PATH/TO/NAME.bin
NAME
```

* A first word with a `/` in it runs that file: `./Tetris.bin`,
  `/Aurora/Apps/Tetris.bin`.
* A first word that is not a command runs `/Aurora/Apps/NAME`, or failing that
  `/Aurora/Apps/NAME.bin`, from any folder: `Tetris`. Commands come first, so
  an app called `ls.bin` has to be run by its path.
* Only an AOS1 or AUR1 container whose payloads lie inside the file is started.
  Otherwise the shell says why:

  | Message | Cause |
  |---------|-------|
  | `aurora: X: command not found` | no command and no app by that name |
  | `aurora: X: No such file or directory` | the path does not exist |
  | `aurora: X: Is a directory` | the path is a folder |
  | `aurora: X: cannot execute: not an Aurora app (see 'info')` | no AOS1 or AUR1 header |
  | `aurora: X: cannot execute: the header points past the end of the file` | a damaged container |
  | `aurora: X: the app did not start` | the loader could not read it; the reason was on the bottom screen |

* The terminal prints `Starting NAME` and the app replaces the OS, as it does
  from the Home Menu. **HOME** returns to a fresh Home Menu, so the terminal
  starts empty next time. If the loader fails, it says why on the bottom
  screen; **B** comes back to the terminal.

## Commands

| Command | Does |
|---------|------|
| [`cat`](#cat) | print files |
| [`cd`](#cd) | change the working folder |
| [`clear`](#clear) | clear the screen |
| [`cp`](#cp) | copy files and folders |
| [`date`](#date) | print the date and time |
| [`echo`](#echo) | print text |
| [`exit`](#exit) | close the terminal |
| [`help`](#help) | list the commands, or explain one |
| [`info`](#info) | what Aurora can do with a file |
| [`ls`](#ls) | list a folder |
| [`mkdir`](#mkdir) | make folders |
| [`mv`](#mv) | move or rename |
| [`poweroff`](#poweroff) | turn the console off |
| [`pwd`](#pwd) | print the working folder |
| [`reboot`](#reboot) | restart the console |
| [`rm`](#rm) | remove files and folders |
| [`shutdown`](#shutdown) | turn off, or restart |
| [`systemctl`](#systemctl) | look at Aurora's services |
| [`touch`](#touch) | make empty files |
| [`uname`](#uname) | print system information |
| [`whoami`](#whoami) | print the user name |

Errors use coreutils' wording, for example
`rm: cannot remove 'Apps': Is a directory`.

### cat

```
cat FILE...
```

Prints each file in turn. Tabs go to the next multiple of 8 columns, and
carriage returns are dropped, so Windows text prints cleanly. Characters
outside ASCII, and control characters, show as `?`. Holding **SELECT** stops a
long file (`cat: interrupted`). A folder gives `cat: 'X': Is a directory`.

### cd

```
cd [DIR]
cd -
```

| Form | Goes to |
|------|---------|
| `cd` or `cd ~` | the root of the card |
| `cd DIR` | DIR, relative to the working folder unless it starts with `/` |
| `cd ..` | the parent folder |
| `cd -` | the previous working folder, and prints it |

The new path takes each folder's spelling from the card, so `cd aurora/apps`
shows `/Aurora/Apps` in the prompt.

```
cd "Nintendo 3DS"
cd /Aurora/Apps
```

### clear

```
clear
```

Empties the scrollback and the screen. The history is kept.

### cp

```
cp [-r] [-n] SRC DEST
cp [-r] [-n] SRC... DIR
```

| Option | Means |
|--------|-------|
| `-r`, `-R` | copy folders and everything in them |
| `-n` | do not replace a file that is already there |
| `-f` | accepted; replacing is the default |

* With one SRC and a DEST that is not a folder, the copy is called DEST. When
  DEST is an existing folder, or there are several SRCs, each goes into it under
  its own name.
* A file already at the destination is replaced, unless `-n`. A folder already
  there is not merged: `cp: cannot copy 'A' to 'B': File exists`.
* A folder without `-r` is skipped:
  `cp: -r not specified; omitting directory 'X'`.
* A folder cannot be copied into itself.
* The line below the output shows `Copying NAME 45%`. Holding **SELECT** stops
  the copy. A copy that fails or is stopped deletes what it made.
* Copies are dated 1 June 2025, like everything Aurora writes, because FatFs is
  built without a clock (`FF_FS_NORTC`).

```
cp notes.txt notes-backup.txt
cp -r Aurora/Music /Backup
cp *.png Pictures
```

### date

```
date
```

Prints the time from the console's clock in `date`'s format, without a time
zone:

```
Thu Sep  3 14:05:09 2026
```

### echo

```
echo [-n] [TEXT]...
```

Prints its arguments with one space between them. `-n`, as the first argument,
leaves out the newline. `echo *.txt` lists the matching names on one line.

### exit

```
exit
```

Closes the terminal, the same as **START**.

### help

```
help
help COMMAND
COMMAND --help
```

With no argument, lists every command with one line about it, then the keys.
With a command, prints its usage and what its options do.

### info

```
info FILE...
```

Aurora's own command: what Aurora can do with a file. It follows the same
rules as the File Explorer, the viewers and the app loader, so its answer
matches what opening the file would do. Each file prints a block:

| Line | Shows |
|------|-------|
| Path | where it is |
| Type | the kind of file, from its extension, or from its header for apps and FIRMs |
| Size | as `ls -h` and in bytes |
| Modified | date and time |
| Flags | `read-only`, `hidden`, `system`, `archive`, or `none` |
| Image | width x height (pictures) |
| Audio | rate, depth, channels and length (WAV and AAF) |
| ARM9, ARM11 | payload size, load address and entry (apps) |
| Icon | whether an app carries a Home Menu icon |
| Execute | whether Aurora can run it, and how, or why not |
| Open | what opens it, or why it cannot be opened |

What **Execute** and **Open** say:

| File | Execute | Open |
|------|---------|------|
| AUR1 or AOS1 container | yes, with the command to type; no if the header points past the end of the file | the File Explorer runs it |
| PNG, BMP, JPEG | no | the image viewer, unless: interlaced PNG; compressed BMP or a depth other than 8, 24 or 32 bits; JPEG other than baseline; over 6 MB; more pixels than the viewer holds; contents not a picture |
| WAV | no | the File Explorer plays uncompressed 8 and 16-bit PCM; other formats do not play |
| AAF | no | the Music app, from `/Aurora/Music`; not without an AAF1 header |
| MP3 | no | not decoded yet; hex editor only |
| TXT, LOG | no | the text viewer, and `cat` |
| FIRM | no: boot it from Luma's chainloader | hex editor only |
| other `.bin` | no: no AOS1 or AUR1 header | hex editor only |
| anything else | no | hex editor only |
| folder | no | `cd`, or the File Explorer; also counts its folders and files |

```
Nick@n3ds:/Aurora/Apps$ info Tetris.bin
Tetris.bin
  Path:     /Aurora/Apps/Tetris.bin
  Type:     Aurora app (AUR1)
  Size:     45.2K (46284 bytes)
  Modified: 2026-09-26 18:22
  Flags:    archive
  ARM9:     45.1K at 0x22000000, entry 0x22000000
  ARM11:    none
  Icon:     yes
  Execute:  yes: type Tetris, or pick it on the
            Home Menu
  Open:     yes: the File Explorer runs it
```

### ls

```
ls [-l] [-a] [-1] [-h] [PATH]...
```

| Option | Means |
|--------|-------|
| `-l` | long listing: flags, size, date and name, one per line |
| `-a` | include hidden and system entries, and names starting with a dot |
| `-1` | one name per line |
| `-h` | accepted; `-l` always gives sizes this way |

* With no PATH, lists the working folder. A PATH that is a file prints that
  file. Several PATHs each get a heading.
* Names are sorted ignoring letter case and laid out in columns, down then
  across, as `ls` does.
* In `-l`, the flags are four characters: `d` for a folder, `r`, `w` unless the
  file is read-only, and `x` for a folder or an Aurora app. Sizes read as
  `934`, `1.2K`, `34M`; folders show `-`.

```
Nick@n3ds:/$ ls -l
drwx     - 2026-09-30 12:34 Aurora
-rwx  233K 2026-09-30 12:34 AURORAOS.BIN
-rw-  2.9K 2026-09-30 12:34 notes.txt
```

### mkdir

```
mkdir [-p] DIR...
```

| Option | Means |
|--------|-------|
| `-p` | make missing parent folders too, and do not complain about folders already there |

Without `-p`: `mkdir: cannot create directory 'X': File exists` when the name
is taken, and `No such file or directory` when the parent is missing. A name
FAT does not allow gives `Invalid argument`.

```
mkdir Pictures
mkdir -p Backup/2026/September
```

### mv

```
mv [-n] SRC DEST
mv [-n] SRC... DIR
```

| Option | Means |
|--------|-------|
| `-n` | do not replace a file that is already there |
| `-f` | accepted; replacing is the default |

* With one SRC and a DEST that is not a folder, SRC is renamed to DEST, which
  can be in another folder. When DEST is an existing folder, or there are
  several SRCs, each moves into it under its own name.
* A move is a rename on the card, so it is instant whatever the size.
* A file already at the destination is replaced, unless `-n`. A folder there
  is not replaced: `mv: cannot move 'A' to 'B': File exists`.
* Changing only the letter case works: `mv readme.txt README.txt`.
* A folder cannot be moved into itself.
* Moving the working folder, or a folder it is in, takes the shell with it.

```
mv notes.txt old-notes.txt
mv *.png Pictures
```

### poweroff

```
poweroff
```

Prints `Powering off...` and turns the console off.

### pwd

```
pwd
```

Prints the working folder, for example `/Aurora/Apps`.

### reboot

```
reboot
```

Prints `Rebooting...` and restarts the console. It boots as it does from
power-on, so hold **START** to reach Luma's chainloader and Aurora again.

### rm

```
rm [-r] [-f] PATH...
```

| Option | Means |
|--------|-------|
| `-r`, `-R` | remove folders and everything in them |
| `-f` | say nothing about files that do not exist, and nothing when no PATH is given |

* A folder without `-r` is kept: `rm: cannot remove 'X': Is a directory`.
* `/`, `.` and `..` are refused.
* A read-only file is not removed: `Permission denied`.
* While a folder is being removed, the line below the output shows
  `Removing NAME`. Holding **SELECT** stops it (`rm: interrupted`) and leaves
  what it had not reached yet.
* If the working folder is removed, the shell moves to the nearest folder that
  is still there.

```
rm notes.txt
rm *.log
rm -r Backup
```

### shutdown

```
shutdown [-h | -P | -r] [now]
shutdown -c
```

| Option | Means |
|--------|-------|
| `-h`, `-P` | turn the console off (the default) |
| `-r` | restart instead |
| `-c` | cancel a scheduled shutdown; none can be scheduled, so it says so |

The time can be `now`, `+0`, or left out; all three act at once. Any other time
is refused, because shutdowns cannot be scheduled.

```
shutdown now
shutdown -r now
```

### systemctl

```
systemctl [list-units]
systemctl status [UNIT]...
systemctl is-active UNIT...
systemctl start|stop|restart|enable|disable UNIT...
systemctl poweroff
systemctl reboot
```

Units can be named with or without their suffix (`audio` or `audio.service`):

| Unit | Active when | Shows |
|------|-------------|-------|
| `core11.service` | the ARM11 core answers | its version, and this build's if they differ |
| `audio.service` | the core answers and is not parked | playing or idle |
| `touch.service` | the core answers | default or custom calibration |
| `gpu.service` | the GPU driver is up | jobs done |
| `wifi.service` | never, while Wi-Fi is paused | why |
| `sdcard.mount` | a card is mounted | free and total space |
| `power.service` | the MCU answers | battery level, and whether it is charging |

| Verb | Does |
|------|------|
| `list-units`, or none | one line per unit: name, `active` / `inactive` / `failed`, details |
| `status` | the console: `running`, or `degraded` if a unit failed, with the unit count, model and version |
| `status UNIT` | that unit's state and details |
| `is-active UNIT` | prints `active`, `inactive`, `failed` or `unknown` |
| `stop audio` | silences anything playing |
| `start`, `stop`, `restart`, `enable`, `disable` | anything else answers `Operation not supported.` |
| `poweroff`, `reboot` | as the commands of the same names |

```
Nick@n3ds:/$ systemctl
UNIT            ACTIVE   DESCRIPTION
core11.service  active   core v101
audio.service   active   idle
touch.service   active   custom calibration
gpu.service     active   ready, 1234 jobs done
wifi.service    inactive paused: stops at the HTC
                         connect
sdcard.mount    active   12.2 of 30.5 GB free
power.service   active   battery 87%, charging

7 units listed.
```

### touch

```
touch FILE...
```

Makes each FILE as an empty file. A file that already exists is left as it
is; its date cannot change, because FatFs is built without a clock.

### uname

```
uname [-a] [-s] [-n] [-r] [-m] [-o]
```

| Option | Prints |
|--------|--------|
| `-s` | `AuroraOS` (the default) |
| `-n` | the host, `n3ds` or `o3ds` |
| `-r` | the version, for example `Beta v0.1.2` |
| `-m` | `armv5tel`, the ARM9 |
| `-o` | `AuroraOS` |
| `-a` | all of the above, in that order |

### whoami

```
whoami
```

Prints the user name from setup.

## Limits

| What | Limit |
|------|-------|
| Command line | 255 characters |
| Words, after `*` and `?` are expanded | 64, and 8 KB of names from patterns |
| History | the last 32 commands, without repeats in a row |
| Scrollback | 320 lines |
| Path | 1,021 bytes |
| One folder in `ls`, a pattern or Tab | 1,024 entries or 48 KB of names; `ls` warns when there are more |
| Folder depth for `cp -r` and `rm -r` | 24 |
