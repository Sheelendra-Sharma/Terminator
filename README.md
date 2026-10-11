# Terminator - Design Document (Tasks 1 to 5)

## Build and run
    make
    ./terminator                  # thread mode: each tab is a thread
    ./terminator useProcess=True  # process mode: each tab is a process

Needs gcc, make and libx11-dev.

## Files
| File | Task | Contents |
|---|---|---|
| `terminator.h` | all | shared structs and function declarations |
| `gui_tabs.c` | 1 | X11 window, tab bar, event loop, thread/process tab start |
| `builtin_commands.c` | 2 | shell loop, built-ins, `$?`, shared history, prompt |
| `multiline_input.c` | 3 | multi-line input, quoting, `$VAR`, `$(...)`, backticks, `*` globbing |
| `io_redirection.c` | 4 | `<`, `>`, `>>` |
| `pipes.c` | 5 | pipelines of any length |

The shell part (tasks 2 to 5) never touches X11. It only reads lines from one pipe and writes text to another.

## Task 1 - GUI and tabs
- The GUI (`main`) is the only code that uses Xlib. It draws into a pixmap and copies it to the window.
- Each tab has a text buffer (5000 lines), an input line with a cursor, and a scroll position. The window is redrawn from the buffer.
- `KeyPress` goes through `XLookupString`. Ctrl+T opens a tab, Ctrl+Q closes one, Ctrl+Tab and Alt+1-9 switch, and clicking a tab or `[ + ]` also works. The mouse wheel and PgUp/PgDn scroll.
- Each tab has two pipes: GUI to shell for typed lines, and shell to GUI for output.
- The loop blocks in `select()` on the X connection and on all tab output pipes. There is no busy-waiting. A tab keeps running while hidden because its output is still read and stored.
- The shell sends the tab title (current directory) in front of each prompt with a marker byte, and the GUI strips it out.
- **Thread mode:** `shell_run()` runs in a detached pthread.
- **Process mode:** the tab does `fork()`, calls `shell_run()` in the child and puts it in its own process group. Closing a tab kills that group, so one tab cannot affect the others.

## Task 2 - Commands
- `shell_run()` loops: prompt, read a command, run it, store the status.
- External commands: `fork()` + `execvp()`, and the shell waits with `waitpid()` on the specific pids. Wrong command: `terminator: x: command not found` (status 127), and the tab keeps working.
- Built-ins: `cd`, `exit [n]`, `history`. `$?` is the last exit status (a signal gives 128+n). `jobs`, `fg`, `bg`, `multiWatch` and `source` belong to later tasks.
- **Per-tab state** lives in a `Shell` struct (cwd, status, fds). Commands `chdir()` to the tab's cwd in the child, so `cd` in one tab never changes another tab, in either mode.
- **Shared history** is one `mmap(MAP_SHARED|MAP_ANONYMOUS)` ring buffer of 10,000 commands, created before any tab starts, so threads and forked processes see the same memory. A `sem_t` initialised with `pshared=1` protects it. `history` shows the last 1,000. The semaphore is released before writing output, so a stuck tab cannot hold it.

## Task 3 - Multi-line input and quoting
- `read_command()` reads one line at a time. `needs_more()` scans the text and says whether it is incomplete: a trailing `\`, an open `'` or `"`, an open `$(` or backtick, or a trailing `|`. If so, the shell prints `> ` and reads more. The lines are joined with `\n`.
- The scanner is a small recursive-descent: `skip_sq`, `skip_dq`, `skip_bt` and `skip_subst` call each other, so nesting and quotes inside `$(...)` work.
- `tokenize()` follows bash rules. Single quotes are literal. Double quotes expand `$VAR`, `${VAR}`, `$?`, `$(...)` and backticks. Unquoted expansion results are split into words. `~` and `*`, `?`, `[` (via `glob()`, relative to the tab's directory) are expanded. `#` starts a comment.
- **Command substitution:** `capture_output()` forks a sub-shell whose stdout is a pipe, runs the inner text through the same `execute_line()` (so inner substitutions run first), and the parent reads the pipe until EOF. Trailing newlines are removed.
- **Design choice:** inside double quotes, `\` followed by a newline is dropped but the newline stays, to match the example in the assignment (bash would join the lines). Outside quotes, `\` newline joins lines as in bash.

## Task 4 - Redirection
- The tokenizer produces `<`, `>` and `>>` tokens. They are attached to the command as `in_file` and `out_file`.
- Before anything is started, `redirect_check()` opens each input file once. If it is missing, an error is printed and nothing runs (status 1).
- In the child, `redirect_apply()` does `open()` then `dup2()` onto fd 0 or 1. Every child opens its own descriptor, so background jobs reading the same file have separate offsets and do not interfere.
- Built-ins in the shell itself (like `history > f`) write straight to the opened file.

## Task 5 - Pipes
- `run_tokens()` splits the tokens at `|` into N commands, creates N-1 pipes and forks N children. Child i gets pipe i-1 as stdin and pipe i as stdout. The first child reads from `/dev/null` and the last writes to the tab. Every child closes all pipe ends, and the parent closes them too, so EOF reaches the readers.
- Redirection is applied after the pipe ends, so `a < in | b > out` works.
- The parent waits for all N children. The status of the last one is `$?`.
- A built-in in a pipeline runs in a child, as bash does. A leading or doubled `|` is a syntax error.

## Known limitations (next tasks)
- Commands cannot read the keyboard (stdin is `/dev/null` unless redirected or piped).
- No Ctrl+C / Ctrl+Z, background jobs, `multiWatch`, `source`, hotkeys other than basic editing, history file, Ctrl+R, Tab completion or script support yet.
- Closing a tab while a command is running in thread mode lets the command finish first.
- Output is ASCII only.
