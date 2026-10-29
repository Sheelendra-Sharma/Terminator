#pragma once

#include <string>
#include <vector>
#include <mutex>

using namespace std;

const int LINE_OUTPUT = 0;
const int LINE_COMMAND = 1;

struct Line {
  string text;
  int kind = LINE_OUTPUT;
};

struct Command {
  vector<string> args;
  string in_file;
  string out_file;
  bool out_append = false;
};

struct Tab {
  string cwd;
  vector<Line> lines;
  string pending;
  int pending_kind = LINE_OUTPUT;
  string input;
  string multi;
  int scroll = 0;
  int last_status = 0;
  bool exit_requested = false;
};

extern vector<string> g_history;
extern mutex g_history_mutex;

void append_text(Tab &tab, const string &text, int kind = LINE_OUTPUT);
void append_line(Tab &tab, const string &line, int kind = LINE_OUTPUT);
void flush_pending(Tab &tab);
void add_history(const string &line);
void load_history();

string prompt_for(const Tab &tab);
string short_cwd(const string &cwd);

bool is_complete(const string &text);
bool ends_with_continuation(const string &text);
