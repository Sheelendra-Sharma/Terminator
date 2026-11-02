#include "task4_redirection.h"

#include <filesystem>
#include <fcntl.h>
#include <unistd.h>

string resolve_path(const string &cwd, const string &path) {
  if (path.empty() || path[0] == '/' || cwd.empty()) return path;
  return (filesystem::path(cwd) / path).lexically_normal().string();
}

bool check_input_file(const Command &cmd, string &error, const string &cwd) {
  if (cmd.in_file.empty()) {
    return true;
  }

  string path = resolve_path(cwd, cmd.in_file);
  int fd = open(path.c_str(), O_RDONLY);
  if (fd < 0) {
    error = "terminator: " + cmd.in_file + ": No such file or directory";
    return false;
  }

  close(fd);
  return true;
}

int open_input_file(const string &path, const string &cwd) {
  string resolved = resolve_path(cwd, path);
  return open(resolved.c_str(), O_RDONLY);
}

int open_output_file(const string &path) {
  return open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
}

int open_output_file_append(const string &path) {
  return open(path.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644);
}

int open_output_for(const Command &cmd, const string &cwd) {
  string path = resolve_path(cwd, cmd.out_file);
  if (cmd.out_append) {
    return open_output_file_append(path);
  }
  return open_output_file(path);
}
