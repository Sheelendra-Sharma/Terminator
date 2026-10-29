#include "task5_pipes.h"

#include "task2_commands.h"
#include "task3_parser.h"
#include "task4_redirection.h"

#include <array>
#include <fcntl.h>
#include <poll.h>
#include <sys/wait.h>
#include <unistd.h>

int run_line(Tab &tab, const string &line) {
  bool blank = true;

  for (char c : line) {
    if (c != ' ' && c != '\t' && c != '\r' && c != '\n') {
      blank = false;
    }
  }

  if (blank) {
    return tab.last_status;
  }

  add_history(line);

  if (!tab.cwd.empty()) {
    chdir(tab.cwd.c_str());
  }

  vector<Command> cmds;
  string error;

  if (!parse_line(line, tab, cmds, error)) {
    append_line(tab, error);
    tab.last_status = 1;
    return 1;
  }

  int status = execute_pipeline(tab, cmds, nullptr);
  tab.last_status = status;

  char buf[4096];
  if (getcwd(buf, sizeof(buf))) {
    tab.cwd = buf;
  }

  return status;
}

int execute_pipeline(Tab &tab, const vector<Command> &cmds, string *capture) {
  if (cmds.empty()) {
    return 0;
  }

  if (cmds.size() == 1 && cmds[0].args.empty()) {
    string error;

    if (!check_input_file(cmds[0], error)) {
      append_line(tab, error);
      return 1;
    }

    if (!cmds[0].out_file.empty()) {
      int fd = open_output_for(cmds[0]);
      if (fd >= 0) {
        close(fd);
      }
    }

    return 0;
  }

  if (cmds.size() == 1 && is_builtin(cmds[0].args[0])) {
    string error, out;

    if (!check_input_file(cmds[0], error)) {
      append_line(tab, error);
      return 1;
    }

    int status = run_builtin(tab, cmds[0], out);

    if (!cmds[0].out_file.empty()) {
      int fd = open_output_for(cmds[0]);
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
    if (!check_input_file(cmd, error)) {
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

  for (int i = 0; i < n; i++) {
    pid_t pid = fork();

    if (pid == 0) {
      if (i > 0) {
        dup2(pipes[i - 1][0], 0);
      } else if (!cmds[i].in_file.empty()) {
        int fd = open_input_file(cmds[i].in_file);
        if (fd >= 0) {
          dup2(fd, 0);
        }
      }

      if (i + 1 < n) {
        dup2(pipes[i][1], 1);
      } else if (!cmds[i].out_file.empty()) {
        int fd = open_output_for(cmds[i]);
        if (fd >= 0) {
          dup2(fd, 1);
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

  if (!use_err) {
    ssize_t r;
    while ((r = read(outpipe[0], buf, sizeof(buf))) > 0) {
      append_text(tab, string(buf, r));
    }
    close(outpipe[0]);
  } else {
    bool out_open = true, err_open = true;

    while (out_open || err_open) {
      pollfd fds[2];
      fds[0].fd = out_open ? outpipe[0] : -1;
      fds[0].events = POLLIN;
      fds[0].revents = 0;
      fds[1].fd = err_open ? errpipe[0] : -1;
      fds[1].events = POLLIN;
      fds[1].revents = 0;

      if (poll(fds, 2, -1) < 0) {
        break;
      }

      for (int k = 0; k < 2; k++) {
        if (fds[k].fd < 0 || !(fds[k].revents & (POLLIN | POLLHUP))) {
          continue;
        }

        ssize_t r = read(fds[k].fd, buf, sizeof(buf));
        if (r <= 0) {
          close(fds[k].fd);
          if (k == 0) {
            out_open = false;
          } else {
            err_open = false;
          }
        } else if (k == 0) {
          *capture += string(buf, r);
        } else {
          append_text(tab, string(buf, r));
        }
      }
    }
  }

  int status = 0;

  for (pid_t pid : pids) {
    int w;
    waitpid(pid, &w, 0);

    if (pid == pids.back()) {
      if (WIFEXITED(w)) {
        status = WEXITSTATUS(w);
      } else if (WIFSIGNALED(w)) {
        status = 128 + WTERMSIG(w);
      }
    }
  }

  flush_pending(tab);
  return status;
}
