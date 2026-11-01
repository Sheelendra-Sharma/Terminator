#pragma once

#include "common.h"

// Run multiWatch ["cmd1", "cmd2", ...] — parallel, re-runs every 2 s until Ctrl+C.
int run_multiwatch(Tab &tab, const vector<string> &args);

