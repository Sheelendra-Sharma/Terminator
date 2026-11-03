// Built-in command interface.

#pragma once

#include "common.h"

// Check and run shell built-in commands.
bool is_builtin(const string &name);
int run_builtin(Tab &tab, const Command &cmd, string &out,
                bool interactive = true);
