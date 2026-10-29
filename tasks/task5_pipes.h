#pragma once

#include "common.h"

int run_line(Tab &tab, const string &line);
int execute_pipeline(Tab &tab, const vector<Command> &cmds, string *capture);
