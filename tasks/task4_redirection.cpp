#include "task4_redirection.h"

#include <fcntl.h>
#include <unistd.h>

bool check_input_file(const Command &cmd, string &error) {
  if (cmd.in_file.empty()) {
    return true;
  }

  int fd = open(cmd.in_file.c_str(), O_RDONLY);
  if (fd < 0) {
    error = "terminator: " + cmd.in_file + ": No such file or directory";
    return false;
  }

  close(fd);
  return true;
}

int open_input_file(const string &path) {
  return open(path.c_str(), O_RDONLY);
}

int open_output_file(const string &path) {
  return open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
}

int open_output_file_append(const string &path) {
  return open(path.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644);
}

int open_output_for(const Command &cmd) {
  if (cmd.out_append) {
    return open_output_file_append(cmd.out_file);
  }
  return open_output_file(cmd.out_file);
}
