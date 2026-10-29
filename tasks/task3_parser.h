#pragma once

#include "common.h"

bool parse_line(const string &line, Tab &tab, vector<Command> &cmds, string &error);
string capture_output(const string &line, Tab &tab);
