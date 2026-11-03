# Terminator Design

This document describes the current implementation of the C++17 X11 terminal emulator. The project uses Xlib for the window and POSIX processes, pipes, and threads for command execution.

## Program structure

| File | Responsibility |
|---|---|
| `main.cpp` | Reads `useProcess=True` and starts the GUI. |
| `tasks/common.h`, `tasks/common.cpp` | `Tab`, `Line`, and `Command` data; text buffering; prompt; persistent and shared history. |
| `tasks/gui.cpp` | X11 window, keyboard/mouse events, clipboard, tab management, and event loop. |
| `tasks/render.cpp` | Draws the tab bar, terminal text, prompt, and cursor. |
| `tasks/tab_runtime.cpp` | Starts tab workers in thread or process mode and transfers input, output, and control events. |
| `tasks/commands.cpp` | Implements built-in commands. |
| `tasks/parser.cpp` | Parses command lines, quoting, expansions, and pipeline stages. |
| `tasks/redirection.cpp` | Resolves paths and opens redirected input/output. |
| `tasks/pipes.cpp` | Runs commands and pipelines, captures output, and handles foreground signals. |
| `tasks/jobs.cpp` | Tracks background and suspended process groups; implements `jobs`, `fg`, and `bg`. |
| `tasks/multiwatch.cpp` | Runs watched commands concurrently and collects output with `poll()`. |
| `tasks/hotkeys.cpp` | Edits the input line and navigates command history. |

## GUI and event loop

The GUI process owns the X11 display connection. The event loop handles X11 events and polls the display connection together with process-mode worker descriptors. This keeps Xlib calls on the GUI side and lets worker output trigger a redraw without a busy loop.

Each `Tab` stores its own working directory, scrollback lines, partial output, input and cursor, multiline command, scroll offset, last status, and exit state. `Line` records the text and whether it is command input or output. `TerminalRenderer` draws the active tab and tab bar from this state. Tab workers continue while another tab is selected.

## Thread and process modes

- **Thread mode (`./terminator`)**: one application process; each tab has a worker thread. Tabs have separate shell state. Shared history uses an in-process mutex.
- **Process mode (`./terminator useProcess=True`)**: the GUI stays in the parent; each tab worker runs in a child process. Local Unix socket pairs carry requests and control events between GUI and worker. The process-shared history is in a `mmap()` region and guarded by a process-shared semaphore.

Both modes run each external command in a child process with `fork()` and `execvp()`. Closing a tab asks its worker to stop and cleans up that tab's jobs.

## Commands, pipes, and redirection

`parse_line()` converts input into a sequence of `Command` values. Each command stores its argument vector and optional input/output paths. The parser handles quoting, environment variables, `$?`, `$$`, command substitution, glob patterns, and home-directory expansion.

`run_line()` dispatches built-ins directly in the tab worker or sends external commands to `execute_pipeline()`. For a pipeline of N commands, the executor creates N−1 pipes and forks one child per stage. Children connect the appropriate pipe ends with `dup2()`, apply file redirection, and call `execvp()`. The parent collects output and waits for the children. Redirection paths are resolved against the tab's working directory; `>` truncates and `>>` appends.

Built-ins are `cd`, `exit`, `history`, `source`, `clear`, `multiWatch`, `jobs`, `fg`, and `bg`. `source` reads and runs nonempty lines in the current shell; it is not a bash script interpreter.

## History and completion

History is stored in a vector in memory and persisted to `~/.terminator_history`, capped at 10,000 entries. In thread mode, a mutex protects the vector. In process mode, the vector is refreshed from shared memory while a process-shared semaphore is held. `history` displays at most the latest 1,000 entries. Ctrl+R searches backward for an exact match; when none exists, it returns commands with the longest common substring longer than two characters.

Filename auto-completion from Task 10 is not implemented, so there is no completion data structure or completion key behavior yet.

## Signals and jobs

The GUI handles Ctrl+C and Ctrl+Z as X11 key events while a foreground command is running. It forwards SIGINT or SIGTSTP to the worker, which signals the command's process group. The terminal shell itself remains running. Background pipelines are tracked as process groups in a per-tab job vector. `jobs` lists them, `fg` continues and waits for one, and `bg` resumes a stopped job in the background. Finished jobs are reaped with nonblocking `waitpid()` calls.

`multiWatch` starts one child for each command and uses `poll()` to collect output and check for Ctrl+C. Each command is rerun after its previous run completes, with a two-second interval.

## Current scope

Task 10 filename completion and Task 11 bash script interpretation are outside the current implementation. Output is buffered until command completion for ordinary foreground commands. Built-ins do not run as pipeline stages, and key bindings are fixed in the code.
