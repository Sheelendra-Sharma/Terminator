#pragma once

#include "common.h"

int run_line(Tab &tab, const string &line, bool interactive = true,
             bool record_history = true);
int execute_pipeline(Tab &tab, const vector<Command> &cmds, string *capture);
int execute_pipeline_command(Tab &tab, const vector<Command> &cmds,
                             const string &command, bool interactive = true);
