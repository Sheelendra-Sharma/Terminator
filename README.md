# Terminator

Terminator is a small Linux terminal emulator and shell written in C++17. It draws its interface with Xlib and runs commands through the POSIX process APIs.

## Requirements

- A C++17 compiler, `make`, and X11 development headers.

On Debian or Ubuntu:

```sh
sudo apt update
sudo apt install build-essential libx11-dev
```

## Build and run

Run these commands from the project directory:

```sh
make
./terminator
```

The default is thread mode: the GUI is in one process and each tab has a worker thread. To run each tab in a separate process, use:

```sh
./terminator useProcess=True
```

Both modes use the same X11 window and command interface. In process mode, tab workers communicate with the GUI over local socket pairs. Commands typed into the shell run as child processes in either mode.

## Features

- X11 terminal window with multiple tabs, scrollback, keyboard and mouse tab controls.
- Built-ins: `cd`, `exit`, `history`, `source`, `clear`, `multiWatch`, `jobs`, `fg`, and `bg`.
- External commands run with `fork()` and `execvp()`. Pipelines, `<`, `>`, `>>`, glob patterns, `~`, environment variables, quoting, and command substitution are supported.
- Multiline input for unfinished quotes, substitutions, and trailing backslashes.
- Line editing, command history navigation, Ctrl+R history search, and X11 clipboard copy/paste.
- Ctrl+C interrupts a foreground command; Ctrl+Z suspends it. Background jobs can be listed and resumed.
- `multiWatch ["cmd1", "cmd2"]` runs commands in parallel, reports output periodically, and stops with Ctrl+C.
- Up to 10,000 history entries are kept in `~/.terminator_history` and shared between tabs. `history` displays up to the latest 1,000 entries.

## Example commands

```sh
cd ~/Downloads
ls *.txt | wc -l
sort < input.txt > sorted.txt
echo "Home: $HOME"
echo "Files: $(ls | wc -l)"
sleep 30 &
jobs
history
```

## Limitations

- Task 10 filename completion and Task 11 bash script interpretation are not implemented. `source` executes nonempty lines in the current shell; it does not implement bash variables, arguments, conditionals, or script syntax checking.
- Hotkeys cannot currently be configured. Ctrl+Shift+C copies the current input line; selecting and copying terminal output is not supported.
- Built-ins are not executed as pipeline stages.
- Command output is added to the window after a command finishes, rather than streamed while it runs.
- Each tab has its own working directory and job table.

## Project layout

```text
.
├── main.cpp
├── tasks/
│   ├── common.cpp / common.h       # Shared types, text buffers, and history
│   ├── gui.cpp / gui.h             # X11 input and event loop
│   ├── render.cpp / render.h       # Window and tab drawing
│   ├── tab_runtime.cpp / .h        # Thread/process tab workers and IPC
│   ├── commands.cpp / .h           # Shell built-ins
│   ├── parser.cpp / .h             # Tokenizing and expansion
│   ├── redirection.cpp / .h        # Input and output files
│   ├── pipes.cpp / .h              # Commands, pipelines, and output capture
│   ├── multiwatch.cpp / .h          # Periodic parallel commands
│   ├── hotkeys.cpp / .h             # Input editing helpers
│   └── jobs.cpp / .h                # Background and stopped jobs
├── DESIGN.md
└── Makefile
```
