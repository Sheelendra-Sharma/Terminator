#include "task2_commands.h"

#include "task3_parser.h"
#include "task5_pipes.h"

#include <cstdlib>
#include <fstream>
#include <unistd.h>

bool is_builtin(const string &name) {
  return name == "cd" || name == "exit" || name == "history" || name == "source";
}

int run_builtin(Tab &tab, const Command &cmd, string &out) {
  string name = cmd.args[0];

  if (name == "cd") {
    string dir = cmd.args.size() > 1 ? cmd.args[1] : (getenv("HOME") ? getenv("HOME") : "/");

    if (chdir(dir.c_str()) != 0) {
      out = "terminator: cd: " + dir + ": No such file or directory\n";
      return 1;
    }

    char buf[4096];
    if (getcwd(buf, sizeof(buf))) {
      tab.cwd = buf;
    }
    return 0;
  }

  if (name == "exit") {
    tab.exit_requested = true;
    return 0;
  }

  if (name == "history") {
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

    ifstream f(cmd.args[1]);
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

      status = run_line(tab, line);

      if (tab.exit_requested) {
        break;
      }
    }

    return status;
  }

  return 1;
}
