#pragma once

#include "common.h"

bool check_input_file(const Command &cmd, string &error);
int open_input_file(const string &path);
int open_output_file(const string &path);
int open_output_file_append(const string &path);
int open_output_for(const Command &cmd);
