// I/O redirection interface.

#pragma once

#include "common.h"

// Resolve paths relative to the tab directory.
string resolve_path(const string &cwd, const string &path);
bool check_input_file(const Command &cmd, string &error, const string &cwd = "");
int open_input_file(const string &path, const string &cwd = "");
int open_output_file(const string &path);
int open_output_file_append(const string &path);
int open_output_for(const Command &cmd, const string &cwd = "");
