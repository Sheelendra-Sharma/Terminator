// Command line parser interface.

#pragma once

#include "common.h"

// Convert shell text into parsed commands.
bool parse_line(const string &line, Tab &tab, vector<Command> &cmds, string &error);
string capture_output(const string &line, Tab &tab);
