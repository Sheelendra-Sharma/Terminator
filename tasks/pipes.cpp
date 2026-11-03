// Run shell commands and connect pipelines.

#include "pipes.h"
#include "commands.h"
#include "parser.h"
#include "redirection.h"
#include "jobs.h"
#include "gui.h"

#include <array>
#include <csignal>
#include <fcntl.h>
#include <poll.h>
#include <sys/wait.h>
#include <unistd.h>

// Dispatch one input line to a built-in or external command.
int run_line(Tab &tab, const string &line, bool interactive, bool record_history) {
  bool blank = true;

  for (char c : line) {
    if (c != ' ' && c != '\t' && c != '\r' && c != '\n') {
      blank = false;
    }
  }

  if (blank) {
    return tab.last_status;
  }

  if (record_history) add_history(line);

  // Check for background execution (trailing unquoted '&').
  string stripped;
  if (is_background(line, stripped)) {
    int status = run_background(tab, stripped);
    jobs_reap(tab);
    tab.last_status = status;
    return status;
  }

  vector<Command> cmds;
  string error;

  if (!parse_line(line, tab, cmds, error)) {
    append_line(tab, error);
    tab.last_status = 1;
    return 1;
  }

  int status = execute_pipeline_command(tab, cmds, line, interactive);
  tab.last_status = status;

  jobs_reap(tab);
  return status;
}

// Connect pipeline stages and collect their output.
static int execute_pipeline_impl(Tab &tab, const vector<Command> &cmds,
                                 string *capture, const string &command,
                                 bool interactive) {
  if (cmds.empty()) {
    return 0;
  }

  if (cmds.size() == 1 && cmds[0].args.empty()) {
    string error;

    if (!check_input_file(cmds[0], error, tab.cwd)) {
      append_line(tab, error);
      return 1;
    }

    if (!cmds[0].out_file.empty()) {
      int fd = open_output_for(cmds[0], tab.cwd);
      if (fd >= 0) {
        close(fd);
      }
    }

    return 0;
  }

  if (cmds.size() == 1 && is_builtin(cmds[0].args[0])) {
    string error, out;

    if (!check_input_file(cmds[0], error, tab.cwd)) {
      append_line(tab, error);
      return 1;
    }

    int status = run_builtin(tab, cmds[0], out, interactive);

    if (!cmds[0].out_file.empty()) {
      int fd = open_output_for(cmds[0], tab.cwd);
      if (fd >= 0) {
        if (!out.empty()) {
          write(fd, out.c_str(), out.size());
        }
        close(fd);
      }
    } else if (capture) {
      *capture += out;
    } else {
      append_text(tab, out);
    }

    flush_pending(tab);
    return status;
  }

  for (auto &cmd : cmds) {
    string error;
    if (!check_input_file(cmd, error, tab.cwd)) {
      append_line(tab, error);
      return 1;
    }
  }

  int n = cmds.size();
  vector<array<int, 2>> pipes;

  for (int i = 0; i + 1 < n; i++) {
    int p[2];
    if (pipe(p) != 0) {
      return 1;
    }
    pipes.push_back({p[0], p[1]});
  }

  int outpipe[2], errpipe[2];

  if (pipe(outpipe) != 0) {
    return 1;
  }

  bool use_err = capture != nullptr;
  if (use_err && pipe(errpipe) != 0) {
    return 1;
  }

  vector<pid_t> pids;
  pid_t pgid = 0;

  for (int i = 0; i < n; i++) {
    pid_t pid = fork();

    if (pid == 0) {
      if (interactive) setpgid(0, pgid ? pgid : 0);
      if (!tab.cwd.empty()) chdir(tab.cwd.c_str());
      if (i > 0) {
        dup2(pipes[i - 1][0], 0);
      } else if (!cmds[i].in_file.empty()) {
        int fd = open_input_file(cmds[i].in_file, tab.cwd);
        if (fd >= 0) {
          dup2(fd, 0);
          close(fd);
        }
      } else {
        int devnull = open("/dev/null", O_RDONLY);
        if (devnull >= 0) {
          dup2(devnull, 0);
          close(devnull);
        }
      }

      if (i + 1 < n) {
        dup2(pipes[i][1], 1);
      } else if (!cmds[i].out_file.empty()) {
        int fd = open_output_for(cmds[i], tab.cwd);
        if (fd >= 0) {
          dup2(fd, 1);
          close(fd);
        }
      } else {
        dup2(outpipe[1], 1);
      }

      if (use_err) {
        dup2(errpipe[1], 2);
      } else {
        dup2(outpipe[1], 2);
      }

      for (auto &p : pipes) {
        close(p[0]);
        close(p[1]);
      }

      close(outpipe[0]);
      close(outpipe[1]);

      if (use_err) {
        close(errpipe[0]);
        close(errpipe[1]);
      }

      vector<char *> argv;
      for (auto &a : cmds[i].args) {
        argv.push_back(const_cast<char *>(a.c_str()));
      }
      argv.push_back(nullptr);

      execvp(argv[0], argv.data());

      string msg = "terminator: " + cmds[i].args[0] + ": command not found\n";
      write(2, msg.c_str(), msg.size());
      _exit(127);
    }

    if (pid < 0) {
      return 1;
    }

    if (interactive) {
      if (!pgid) pgid = pid;
      setpgid(pid, pgid);
    }
    pids.push_back(pid);
  }

  for (auto &p : pipes) {
    close(p[0]);
    close(p[1]);
  }

  close(outpipe[1]);
  if (use_err) {
    close(errpipe[1]);
  }

  char buf[4096];
  bool out_open = true, err_open = use_err;
  int xfd = interactive && !capture ? gui_event_fd() : -1;
  bool stopped = false, interrupted = false;
  vector<bool> child_done(pids.size(), false);
  size_t children_left = pids.size();
  int status = 0;

  auto handle_control_key = [&]() {
    int signal = gui_poll_job_control();
    if (signal != SIGINT && signal != SIGTSTP) return;

    kill(-pgid, signal);
    if (signal == SIGINT) {
      append_line(tab, "^C");
      interrupted = true;
      return;
    }

    bool any_stopped = false;
    for (size_t i = 0; i < pids.size(); i++) {
      if (child_done[i]) continue;
      int child_status;
      pid_t result = waitpid(pids[i], &child_status, WUNTRACED);
      if (result != pids[i]) continue;
      if (WIFSTOPPED(child_status)) {
        any_stopped = true;
      } else {
        child_done[i] = true;
        children_left--;
        if (pids[i] == pids.back()) {
          if (WIFEXITED(child_status)) status = WEXITSTATUS(child_status);
          else if (WIFSIGNALED(child_status)) status = 128 + WTERMSIG(child_status);
        }
      }
    }
    stopped = any_stopped;
  };

  while (out_open || err_open || (xfd >= 0 && children_left > 0)) {
    if (xfd >= 0) handle_control_key();
    if (stopped) break;

    pollfd fds[3] = {
      {out_open ? outpipe[0] : -1, POLLIN, 0},
      {err_open ? errpipe[0] : -1, POLLIN, 0},
      {xfd, POLLIN, 0}
    };
    nfds_t count = xfd >= 0 ? 3 : 2;
    if (poll(fds, count, xfd >= 0 ? 100 : -1) < 0) continue;

    for (int k = 0; k < 2; k++) {
      if (fds[k].fd < 0 || !(fds[k].revents & (POLLIN | POLLHUP | POLLERR))) continue;

      ssize_t r = read(fds[k].fd, buf, sizeof(buf));
      if (r <= 0) {
        close(fds[k].fd);
        if (k == 0) out_open = false;
        else err_open = false;
      } else if (k == 0 && use_err) {
        *capture += string(buf, r);
      } else {
        append_text(tab, string(buf, r));
      }
    }

    if (xfd >= 0) {
      for (size_t i = 0; i < pids.size(); i++) {
        if (child_done[i]) continue;
        int child_status;
        pid_t result = waitpid(pids[i], &child_status, WNOHANG);
        if (result == pids[i]) {
          child_done[i] = true;
          children_left--;
          if (pids[i] == pids.back()) {
            if (WIFEXITED(child_status)) status = WEXITSTATUS(child_status);
            else if (WIFSIGNALED(child_status)) status = 128 + WTERMSIG(child_status);
          }
        }
      }
    }
  }

  if (stopped) {
    append_line(tab, "^Z");
    jobs_add_stopped(tab, pgid, command.empty() ? cmds[0].args[0] : command,
                     out_open ? outpipe[0] : -1);
    return 128 + SIGTSTP;
  }

  if (out_open) close(outpipe[0]);
  if (err_open) close(errpipe[0]);

  for (size_t i = 0; i < pids.size(); i++) {
    if (child_done[i]) continue;
    int w;
    waitpid(pids[i], &w, 0);

    if (pids[i] == pids.back()) {
      if (WIFEXITED(w)) {
        status = WEXITSTATUS(w);
      } else if (WIFSIGNALED(w)) {
        status = 128 + WTERMSIG(w);
      }
    }
  }

  flush_pending(tab);
  return interrupted ? 128 + SIGINT : status;
}

int execute_pipeline(Tab &tab, const vector<Command> &cmds, string *capture) {
  return execute_pipeline_impl(tab, cmds, capture, "", true);
}

int execute_pipeline_command(Tab &tab, const vector<Command> &cmds,
                             const string &command, bool interactive) {
  return execute_pipeline_impl(tab, cmds, nullptr, command, interactive);
}
