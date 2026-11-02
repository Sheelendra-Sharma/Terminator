#include "task2_commands.h"
#include "task3_parser.h"
#include "task4_redirection.h"
#include "task5_pipes.h"
#include "task6_multiwatch.h"
#include "task8_jobs.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <unistd.h>

bool is_builtin(const string &name) {
  return name == "cd" || name == "exit" || name == "history" || name == "source"
      || name == "multiWatch" || name == "jobs" || name == "fg" || name == "bg"
      || name == "clear";
}

int run_builtin(Tab &tab, const Command &cmd, string &out, bool interactive) {
  string name = cmd.args[0];

  if (name == "cd") {
    string dir = cmd.args.size() > 1 ? cmd.args[1] : (getenv("HOME") ? getenv("HOME") : "/");

    filesystem::path target(dir);
    if (target.is_relative()) target = filesystem::path(tab.cwd) / target;
    target = target.lexically_normal();
    error_code ec;
    if (!filesystem::is_directory(target, ec)) {
      out = "terminator: cd: " + dir + ": No such file or directory\n";
      return 1;
    }
    tab.cwd = target.string();
    return 0;
  }

  if (name == "exit") {
    tab.exit_requested = true;
    return 0;
  }

  if (name == "history") {
    load_history();
    lock_guard<mutex> lock(g_history_mutex);

    size_t start = g_history.size() > 1000 ? g_history.size() - 1000 : 0;
    for (size_t i = start; i < g_history.size(); i++) {
      out += "  " + to_string(i + 1) + "  " + g_history[i] + "\n";
    }
    return 0;
  }

  if (name == "source") {
    if (cmd.args.size() < 2) {
      out = "terminator: source: filename required\n";
      return 1;
    }

    ifstream f(resolve_path(tab.cwd, cmd.args[1]));
    if (!f) {
      out = "terminator: source: " + cmd.args[1] + ": No such file or directory\n";
      return 1;
    }

    string line;
    int status = 0;

    while (getline(f, line)) {
      size_t first = line.find_first_not_of(" \t\r");

      if (first == string::npos || line[first] == '#') {
        continue;
      }

      status = run_line(tab, line, interactive, true);

      if (tab.exit_requested) {
        break;
      }
    }

    return status;
  }

  if (name == "multiWatch") {
    return run_multiwatch(tab, cmd.args, interactive);
  }

  if (name == "clear") {
    tab.lines.clear();
    tab.pending.clear();
    return 0;
  }

  if (name == "jobs") {
    out = jobs_list();
    return 0;
  }

  if (name == "fg") {
    int id = cmd.args.size() > 1 ? stoi(cmd.args[1].substr(cmd.args[1][0] == '%')) : 1;
    return jobs_fg(tab, id, interactive);
  }

  if (name == "bg") {
    int id = cmd.args.size() > 1 ? stoi(cmd.args[1].substr(cmd.args[1][0] == '%')) : 1;
    return jobs_bg(tab, id);
  }

  return 1;
}
