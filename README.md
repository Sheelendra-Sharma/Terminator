# Terminator

A custom terminal emulator and shell for Linux, written in C++17.
It draws its own X11 window, runs one shell per tab, and supports
external commands, built-ins, multiline input and quoting, I/O
redirection, and pipelines. This build follows the project
specification (Terminator, CS69201) up to and including Task 5.

## Implemented features

- Task 1: X11 GUI and tabs. One text buffer per tab, prompt, typed
  input, command output, scrolling with the mouse wheel and PageUp /
  PageDown. Click a tab to switch, click `[ + ]` to open a tab,
  Ctrl+T for a new tab, Ctrl+W to close a tab, Ctrl+PageUp / Ctrl+PageDown
  to switch tabs, Ctrl+L to clear the screen, Ctrl+C to cancel the
  current line. Typed commands and the prompt are drawn in bright
  green, command output in white, so a command is easy to tell apart
  from its output. The event loop uses `XNextEvent`, so it blocks instead
  of busy-waiting. Output is captured through pipes and drawn in the tab.
- Task 2: Built-in and external commands. External commands run with
  `fork` + `execvp` and the shell waits for them. Built-ins: `cd`,
  `exit`, `history`, `source`. An unknown command prints
  `terminator: <name>: command not found` and the tab stays usable.
  The last exit status is kept in `$?`. History is shared by all tabs
  (protected by a mutex) and saved to `~/.terminator_history`.
- Task 3: Multiline input and quoting. An incomplete command shows the
  `>` continuation prompt. Supported: trailing `\` line continuation,
  single and double quotes spanning lines, `$(...)` and backtick
  command substitution including nested and multiline forms (innermost
  runs first), and bash quoting rules (single quotes are literal,
  double quotes still expand `$VAR` and substitutions).
- Task 4: I/O redirection with `<` and `>` (and `>>` for append), using
  `dup2` in the child. A missing input file is reported instead of
  running the command. Each child opens its own file descriptors, so
  commands reading the same file do not interfere with each other.
- Task 5: Pipes of any length (`N` commands use `N-1` pipes and
  `N` child processes), combined with redirection. Unquoted `*`, `?`
  and `[...]` are expanded to matching files, and `~` expands to the
  home directory.

## Dependencies

A C++ compiler, make, and the X11 development headers.

```bash
sudo apt update
sudo apt install build-essential libx11-dev
```

For a headless machine, `xvfb` lets you run the GUI without a real
display:

```bash
sudo apt install xvfb
```

## Build and run

From the project root:

```bash
make
./terminator
```

`./terminator` runs in thread mode (the default in the specification).
`./terminator useProcess=True` is accepted, but process mode is not
implemented yet: the program prints a notice in the first tab and runs
in thread mode. See Known limitations.

Headless test run:

```bash
Xvfb :99 -screen 0 1024x768x24 &
DISPLAY=:99 ./terminator
```

## Example commands

```bash
cd ~/Downloads
gcc -o myprog myprog.c
./myprog
notacommand
echo 'literal: $HOME' "expanded: $HOME"
echo "Isn't process creation exciting?\
Fork, exec, repeat!"
echo "Files in $(basename $(pwd)): $(ls | wc -l)"
sort < somefile.txt
ls > abc
./a.out < infile.txt > outfile.txt
ls *.txt | wc -l
cat abc.c | sort | head -5
ls *.txt | xargs rm
history
echo $?
```

Multiline example, typed exactly as shown (the first line ends with `\`):

```text
user@terminator> echo "Isn't process creation exciting?\
> Fork, exec, repeat!"
Isn't process creation exciting?
Fork, exec, repeat!
```

## Folder structure

```text
terminator/
├── main.cpp
├── tasks/
│   ├── common.h
│   ├── common.cpp
│   ├── task1_gui.h
│   ├── task1_gui.cpp
│   ├── task2_commands.h
│   ├── task2_commands.cpp
│   ├── task3_parser.h
│   ├── task3_parser.cpp
│   ├── task4_redirection.h
│   ├── task4_redirection.cpp
│   ├── task5_pipes.h
│   └── task5_pipes.cpp
├── Makefile
└── README.md
```

- `main.cpp`: only parses the `useProcess=True` argument and starts
  the GUI.
- `tasks/common.h` / `tasks/common.cpp`: shared types (`Tab`,
  `Command`), shared history, the text buffer, the prompt, and the
  multiline-completeness checks.
- `tasks/task1_gui.cpp`: X11 window, tab bar, drawing, scrolling,
  keyboard and mouse handling, and the event loop.
- `tasks/task2_commands.cpp`: the built-ins `cd`, `exit`, `history`
  and `source`.
- `tasks/task3_parser.cpp`: tokenising, quoting, `$VAR` expansion,
  command substitution, glob expansion, and pipeline parsing.
- `tasks/task4_redirection.cpp`: opening input and output files for
  redirection and checking a missing input file.
- `tasks/task5_pipes.cpp`: running a line, running a pipeline with
  pipes, and capturing output for command substitution.

## Known limitations

- Task 6 (`multiWatch`) and later tasks are not implemented, as
  requested. That includes the `multiWatch` built-in, hotkeys from
  Task 7 (arrow-key editing, history browsing with Up/Down, Ctrl+R
  search, Tab completion, copy/paste), job control from Task 8 (`jobs`,
  `fg`, `bg`, `cmd &`, Ctrl+Z), the searchable history file behaviour
  from Task 9 beyond the shared `history` command, file-name
  auto-completion from Task 10, and bash script interpretation from
  Task 11 beyond the simple line-by-line `source` command.
- Process mode (`useProcess=True`) is not implemented; only thread
  mode runs. Closing a tab does not affect other tabs' buffers or
  working directories.
- Built-ins work only as a single command, not inside a pipeline
  (for example, `cd | cat` is not supported).
- Tabs do not run commands at the same time: a command in the active
  tab runs to completion before the GUI handles other input, so a
  long-running command blocks redrawing until it finishes. A tab's
  buffer, input and working directory are kept while another tab is
  shown.
- `source` runs a file line by line with the same shell; it does not
  implement script variables, arguments, `if` tests or `exit [n]`
  from Task 11.

## Troubleshooting

- `cannot open X display`: there is no X server. On a desktop, run
  from a terminal inside the graphical session. Headless, start
  `Xvfb` first (see above) and set `DISPLAY=:99`.
- `X11/Xlib.h: No such file or directory`: install `libx11-dev`
  (see Dependencies), then run `make clean && make`.
- `make: *** No rule to make target`: run `make` from the project
  root, the directory containing this README and the Makefile, not
  from inside `tasks/`.
- Window opens but typing does nothing: click inside the window once
  to give it keyboard focus, then type.
- A command prints `command not found`: the program is not on `PATH`
  or is not executable. For a program in the current directory, use
  `./myprog`, not `myprog`.
- The GUI looks blank after `Ctrl+L`: that only clears the screen
  buffer; the prompt is still there, just type a command.
