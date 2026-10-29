#pragma once

#include "common.h"

bool is_builtin(const string &name);
int run_builtin(Tab &tab, const Command &cmd, string &out);
