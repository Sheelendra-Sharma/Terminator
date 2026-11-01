#include "common.h"

#include <algorithm>
#include <cstdlib>
#include <deque>
#include <fstream>
#include <pwd.h>
#include <unistd.h>

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

  if (line.empty()) return;

  g_history.push_back(line);
  while (g_history.size() > 10000) {
    g_history.erase(g_history.begin());
  }

  const char *home = getenv("HOME");
  if (!home) return;

  ofstream f(string(home) + "/.terminator_history", ios::trunc);
  for (const string &entry : g_history) f << entry << '\n';
}

void load_history() {
  const char *home = getenv("HOME");
  if (!home) {
    return;
  }

  ifstream f(string(home) + "/.terminator_history");
  bool loaded = f.good();
  deque<string> recent;
  string line;

  while (getline(f, line)) {
    if (line.empty()) continue;
    recent.push_back(line);
    if (recent.size() > 10000) recent.pop_front();
  }

  lock_guard<mutex> lock(g_history_mutex);
  g_history.assign(recent.begin(), recent.end());

  if (loaded) {
    ofstream trimmed(string(home) + "/.terminator_history", ios::trunc);
    for (const string &entry : g_history) trimmed << entry << '\n';
  }
}

vector<string> search_history(const string &term) {
  lock_guard<mutex> lock(g_history_mutex);
  if (term.empty()) return {};

  for (auto it = g_history.rbegin(); it != g_history.rend(); ++it) {
    if (*it == term) return {*it};
  }

  vector<size_t> best_by_command(g_history.size(), 0);
  size_t best = 0;
  vector<size_t> previous(term.size() + 1), current(term.size() + 1);

  for (size_t i = 0; i < g_history.size(); ++i) {
    fill(previous.begin(), previous.end(), 0);
    for (char c : g_history[i]) {
      fill(current.begin(), current.end(), 0);
      for (size_t j = 1; j <= term.size(); ++j) {
        if (c == term[j - 1]) current[j] = previous[j - 1] + 1;
        best_by_command[i] = max(best_by_command[i], current[j]);
      }
      previous.swap(current);
    }
    best = max(best, best_by_command[i]);
  }

  vector<string> matches;
  if (best <= 2) return matches;

  for (size_t i = g_history.size(); i > 0; --i) {
    if (best_by_command[i - 1] == best) matches.push_back(g_history[i - 1]);
  }
  return matches;
}

string prompt_for(const Tab &) {
  const char *username = getenv("USER");
  if (!username || !*username) {
    const passwd *account = getpwuid(getuid());
    username = account ? account->pw_name : nullptr;
  }

  string name = username && *username ? username : "user";
  return name + "@" + name + "> ";
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
