# Terminator — Architecture Reference

A minimal X11 terminal emulator written in C++17. The GUI runs in the main thread; each tab executes in its own thread by default or in a child process with `useProcess=True`. Commands run with `fork`/`execvp`. History is persisted in `~/.terminator_history` and shared between process-mode tabs through shared memory.

---

## File Map

| File | Role |
|---|---|
| `main.cpp` | Entry point — parses `useProcess=True` and calls `gui_run()` |
| `tasks/common.h` | Shared types: `Tab`, `Line`, `Command`; function declarations |
| `tasks/common.cpp` | Text buffering, history I/O, prompt/cwd helpers |
| `tasks/task1_gui.cpp` | X11 window, input, and event loop |
| `tasks/task1_render.cpp` | Tab bar, wrapped output, prompt, and cursor drawing |
| `tasks/tab_runtime.cpp` | Per-tab threads/processes, IPC, and command result updates |
| `tasks/task2_commands.cpp` | Built-in command dispatch (`cd`, `exit`, `history`, `source`, `multiWatch`, `jobs`, `fg`, `bg`, `clear`) |
| `tasks/task3_parser.cpp` | Tokenizer / expander → `vector<Command>` |
| `tasks/task4_redirection.cpp` | Opens `in_file` / `out_file` with `open(2)` before fork |
| `tasks/task5_pipes.cpp` | `run_line()` entry-point; `execute_pipeline()` forks N children |
| `tasks/task6_multiwatch.cpp` | `multiWatch` — re-runs commands every 2 s via `poll()` |
| `tasks/task7_hotkeys.cpp` | `EditState` line-editing helpers (cursor and history navigation) |
| `tasks/task8_jobs.cpp` | Background job table; `is_background()`, `run_background()`, `jobs_reap/fg/bg` |
| `Makefile` | `g++ -std=c++17 -Wall -Wextra -pthread -I.` |

---

## Data Flow

```
User keystroke (X11 KeyPress)
        │
        ▼
  task1_gui.cpp  ──  handle_enter()
        │
        ▼
   tab_runtime.cpp  ──  one worker per tab
        │
        └─► run_line()  [task5_pipes.cpp]
                        │
                        ├─► run_builtin()  [task2_commands.cpp]
                        │
                        ├─ parse_line()  [task3_parser.cpp]
                        │       tokenise, expand $VAR / $() / glob / ~
                        │       → vector<Command>
                        │
                        ├─ check_input_file() / open_output_for()  [task4_redirection.cpp]
                        │
                        └─ execute_pipeline()  [task5_pipes.cpp]
                                fork N children with N-1 pipes
                                child → execvp()
                                parent reads stdout/stderr pipe
                                        │
                                        ▼
                                  append_text()  [common.cpp]
                                        │
                                        ▼
                                   Tab.lines[]
                                        │
                                        ▼
                                    task1_render.cpp
                                  XDrawString → screen
```

---

## Central State — `Tab` struct (`common.h`)

| Field | Type | Purpose |
|---|---|---|
| `cwd` | `string` | Current working directory for this tab |
| `lines` | `vector<Line>` | All rendered lines (history + output) |
| `pending` | `string` | Partial line buffer (unflushed bytes) |
| `pending_kind` | `int` | `LINE_OUTPUT` or `LINE_COMMAND` |
| `input` | `string` | Current user-typed input |
| `multi` | `string` | Accumulated multi-line input |
| `scroll` | `int` | Lines scrolled up (0 = bottom) |
| `last_status` | `int` | Exit code of last command |
| `exit_requested` | `bool` | Set by `exit` built-in to close the tab |

---

## Module Summaries

### `common.cpp` / `common.h`
Shared types and utilities used by every other module.

- `append_text()` — buffers partial output into `tab.pending`, flushes complete lines into `tab.lines`.
- `append_line()` — pushes a complete `Line` directly.
- `flush_pending()` — forces any remaining partial bytes into `tab.lines`.
- `add_history()` / `load_history()` — thread-safe history append / load from `~/.terminator_history`.
- `prompt_for()` — returns `"user@host:~/cwd$ "` string.
- `short_cwd()` — replaces `$HOME` prefix with `~`.
- `is_complete()` — detects unclosed quotes/brackets for multi-line input.
- `ends_with_continuation()` — detects trailing `\` for line continuation.

---

### `task1_gui.cpp` — X11 GUI
Single X11 window with a tab bar at the top and a terminal body below.

The GUI file handles input and X11 events. `task1_render.cpp` draws tabs,
wrapped output, and the prompt. `tab_runtime.cpp` runs one worker per tab
and returns command results to the GUI.

**Drawing** (`redraw()`):
1. Fill body with `#0d1117`, tab bar with `#161b22`.
2. Draw filled pill-shaped tab buttons (active: `#1f6feb` blue, inactive: `#21262d`).
3. Compute visible row range from `tab.scroll` and window height.
4. Draw each `Line` with `XDrawString` in white.
5. Draw the prompt + input + cursor caret on the last row.

**Event loop** (`gui_run()`):
- `Expose` / `ConfigureNotify` → `redraw()`
- `ButtonPress` Button4/5 → `tab.scroll ±1`, then tab-bar hit-test for clicks
- `KeyPress` → ctrl-key dispatch → `handle_enter()` or character append

**Key bindings:**

| Key | Action |
|---|---|
| Enter | `handle_enter()` → run command |
| Up / Down | Browse command history (forward / backward) |
| Left / Right | Move cursor within input line |
| Home / End (Ctrl+A / Ctrl+E) | Jump cursor to start / end of line |
| Backspace / Delete | Delete character before / at cursor |
| Ctrl+U / Ctrl+K | Delete line to start / to end |
| Ctrl+T | New tab |
| Ctrl+Q / Ctrl+W | Close tab |
| Ctrl+L | Clear screen |
| Ctrl+C / Escape | Cancel current input line |
| Ctrl+PgUp/Down | Switch tab |
| PgUp/PgDn, Shift+Up/Down | Scroll terminal |
| Mouse wheel | Scroll terminal (works anywhere in window) |

---

### `task2_commands.cpp` — Built-ins

| Command | Behaviour |
|---|---|
| `cd [dir]` | `chdir()`, updates `tab.cwd` |
| `exit` | Sets `tab.exit_requested = true` |
| `history` | Dumps last 1000 entries from `g_history` |
| `source <file>` | Reads file line-by-line, calls `run_line()` for each |
| `multiWatch [cmds]` | Delegates to `run_multiwatch()` |
| `clear` | `tab.lines.clear(); tab.pending.clear()` |
| `jobs` | Prints background job table via `jobs_list()` |
| `fg [%n]` | Brings job to foreground via `jobs_fg()` |
| `bg [%n]` | Resumes stopped job in background via `jobs_bg()` |

---

### `task3_parser.cpp` — Tokenizer / Expander

`parse_line(line)` → `vector<Command>`

Expansions applied in order:
1. `~` → `$HOME` (at start of token)
2. `$VAR` / `${VAR}` → environment variable lookup
3. `$?` → last exit status; `$$` → shell PID
4. `$(...)` / `` `...` `` → command substitution via `capture_output()`
5. Glob patterns `*`, `?`, `[...]` → `glob(3)` expansion
6. Quoting: `'...'` literal, `"..."` with `$`/backtick expansion
7. `|` separates pipeline stages; `<` / `>` / `>>` set redirection

---

### `task4_redirection.cpp` — I/O Redirection

- `open_input_file()` — opens `cmd.in_file` with `O_RDONLY`; errors reported to tab.
- `open_output_for()` — opens `cmd.out_file` with `O_WRONLY|O_CREAT` (append or truncate).
- Called by `execute_pipeline()` in the child process before `execvp()`.

---

### `task5_pipes.cpp` — Pipeline Execution

`run_line(tab, line)`:
1. Adds non-empty line to history.
2. Calls `is_background()` — if trailing `&`, delegates to `run_background()`.
3. Otherwise calls `execute_pipeline()`, reaps finished jobs after.

`execute_pipeline(tab, cmds)`:
- Creates N-1 anonymous pipes for N commands.
- Forks one child per command.
- Child: dup2 pipe ends onto stdin/stdout, close extras, `execvp()`.
- Parent: reads the last stage's stdout/stderr via pipe into `append_text()`.
- Waits for all children; sets `tab.last_status`.

---

### `task6_multiwatch.cpp` — multiWatch

`run_multiwatch(tab, args)`:
- Parses `["cmd1","cmd2",...]` from args.
- Spawns each command via `popen()` or fork+pipe every 2 seconds using `poll()`.
- Prints a timestamped block of output per command cycle.
- Stops on SIGINT (Ctrl+C).

---

### `task7_hotkeys.cpp` — Line Editing (`EditState`)

Provides cursor movement, line editing, and history navigation helpers for an `EditState`.

---

### `task8_jobs.cpp` — Background Jobs

Each tab worker keeps a separate `g_jobs` table (`vector<Job>`); it is
thread-local in thread mode and process-local in process mode.

| Function | Description |
|---|---|
| `is_background(line, stripped)` | Returns true if line ends with `&`; strips it |
| `run_background(tab, line)` | Forks, runs pipeline in child, records PID in `g_jobs` |
| `jobs_reap(tab)` | Non-blocking `waitpid` sweep; removes finished jobs |
| `jobs_list()` | Returns formatted job-list string |
| `jobs_fg(tab, id)` | `SIGCONT` + blocking `waitpid` |
| `jobs_bg(tab, id)` | `SIGCONT` only (background continue) |

---

## Key Design Decisions

| Decision | Rationale |
|---|---|
| X11 stays on the GUI thread | Tab workers send command results to the GUI |
| One worker per tab | Tab command state and jobs stay isolated |
| `fork`/`execvp` per command | POSIX-native; gives natural process isolation |
| Per-tab `scroll` field | Each tab remembers its scroll position independently |
| No pixmap double-buffer | Direct `XFillRectangle` + `XDrawString`; fast enough at this scale |
| Shared history | A process-shared semaphore protects history in mapped memory; updates are also saved to disk |
| Process mode | The GUI stays in the parent; each tab gets a child process and a socket pair for commands and results |

