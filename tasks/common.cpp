#include "common.h"

#include <cstdlib>
#include <fstream>

vector<string> g_history;
mutex g_history_mutex;

void flush_pending(Tab &tab) {
  if (tab.pending.empty()) {
    return;
  }

  tab.lines.push_back({tab.pending, tab.pending_kind});
  tab.pending.clear();
}

void append_text(Tab &tab, const string &text, int kind) {
  if (text.empty()) {
    return;
  }

  if (!tab.pending.empty() && kind != tab.pending_kind) {
    flush_pending(tab);
  }

  if (tab.pending.empty()) {
    tab.pending_kind = kind;
  }

  tab.pending += text;

  size_t p;
  while ((p = tab.pending.find('\n')) != string::npos) {
    tab.lines.push_back({tab.pending.substr(0, p), tab.pending_kind});
    tab.pending.erase(0, p + 1);
  }
}

void append_line(Tab &tab, const string &line, int kind) {
  append_text(tab, line + "\n", kind);
}

void add_history(const string &line) {
  lock_guard<mutex> lock(g_history_mutex);

  if (!line.empty()) {
    g_history.push_back(line);
  }

  while (g_history.size() > 10000) {
    g_history.erase(g_history.begin());
  }

  const char *home = getenv("HOME");
  if (home) {
    ofstream f(string(home) + "/.terminator_history", ios::app);
    if (f) {
      f << line << "\n";
    }
  }
}

void load_history() {
  const char *home = getenv("HOME");
  if (!home) {
    return;
  }

  ifstream f(string(home) + "/.terminator_history");
  string line;

  while (getline(f, line) && g_history.size() < 10000) {
    g_history.push_back(line);
  }
}

string prompt_for(const Tab &) {
  return "user@terminator> ";
}

string short_cwd(const string &cwd) {
  const char *home = getenv("HOME");

  if (home && cwd == home) {
    return "~";
  }

  if (home && cwd.rfind(home, 0) == 0) {
    return "~" + cwd.substr(string(home).size());
  }

  return cwd;
}

bool ends_with_continuation(const string &text) {
  char quote = 0;

  for (size_t i = 0; i < text.size(); i++) {
    char c = text[i];

    if (quote == '\'') {
      if (c == '\'') {
        quote = 0;
      }
      continue;
    }

    if (quote == '"') {
      if (c == '"') {
        quote = 0;
      } else if (c == '\\' && i + 1 < text.size()) {
        i++;
      } else if (c == '\\') {
        return true;
      }
      continue;
    }

    if (c == '\'') {
      quote = c;
    } else if (c == '"') {
      quote = c;
    } else if (c == '\\' && i + 1 < text.size()) {
      i++;
    } else if (c == '\\') {
      return true;
    }
  }

  return false;
}

bool is_complete(const string &text) {
  int depth = 0;
  char quote = 0, backtick = 0;

  for (size_t i = 0; i < text.size(); i++) {
    char c = text[i];

    if (quote == '\'') {
      if (c == '\'') {
        quote = 0;
      }
      continue;
    }

    if (quote == '"') {
      if (c == '"') {
        quote = 0;
      } else if (c == '\\' && i + 1 < text.size()) {
        i++;
      }
      continue;
    }

    if (c == '\'') {
      quote = c;
    } else if (c == '"') {
      quote = c;
    } else if (c == '`') {
      backtick = !backtick;
    } else if (c == '$' && i + 1 < text.size() && text[i + 1] == '(') {
      depth++;
      i++;
    } else if (c == ')') {
      if (depth > 0) {
        depth--;
      }
    } else if (c == '\\' && i + 1 < text.size()) {
      i++;
    } else if (c == '\\' && i + 1 == text.size()) {
      return false;
    }
  }

  return quote == 0 && depth == 0 && !backtick;
}
