#include "common.h"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fstream>
#include <semaphore.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <pwd.h>
#include <unistd.h>

vector<string> g_history;
mutex g_history_mutex;

static const size_t HISTORY_LIMIT = 10000;
static const size_t HISTORY_LINE_SIZE = 4096;
struct SharedHistory {
  sem_t semaphore;
  uint32_t count;
  char entries[HISTORY_LIMIT][HISTORY_LINE_SIZE];
};
static SharedHistory *shared_history = nullptr;
static string shared_history_name;

static void copy_history_from_shared() {
  g_history.clear();
  for (uint32_t i = 0; i < shared_history->count; i++)
    g_history.emplace_back(shared_history->entries[i]);
}

static void copy_history_to_shared() {
  size_t count = min(g_history.size(), HISTORY_LIMIT);
  size_t start = g_history.size() - count;
  for (size_t i = 0; i < count; i++) {
    strncpy(shared_history->entries[i], g_history[start + i].c_str(), HISTORY_LINE_SIZE - 1);
    shared_history->entries[i][HISTORY_LINE_SIZE - 1] = '\0';
  }
  shared_history->count = (uint32_t)count;
}

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

  if (shared_history) {
    sem_wait(&shared_history->semaphore);
    copy_history_from_shared();
  }
  g_history.push_back(line);
  while (g_history.size() > HISTORY_LIMIT) {
    g_history.erase(g_history.begin());
  }
  if (shared_history) copy_history_to_shared();

  const char *home = getenv("HOME");
  if (home) {
    ofstream f(string(home) + "/.terminator_history", ios::trunc);
    for (const string &entry : g_history) f << entry << '\n';
  }
  if (shared_history) sem_post(&shared_history->semaphore);
}

void load_history() {
  if (shared_history) {
    lock_guard<mutex> lock(g_history_mutex);
    sem_wait(&shared_history->semaphore);
    copy_history_from_shared();
    sem_post(&shared_history->semaphore);
    return;
  }

  shared_history_name = "/terminator_history_" + to_string(getpid());
  int shm_fd = shm_open(shared_history_name.c_str(), O_CREAT | O_EXCL | O_RDWR, 0600);
  if (shm_fd >= 0) {
    if (ftruncate(shm_fd, sizeof(SharedHistory)) == 0) {
      void *memory = mmap(nullptr, sizeof(SharedHistory), PROT_READ | PROT_WRITE,
                          MAP_SHARED, shm_fd, 0);
      if (memory != MAP_FAILED) {
        shared_history = static_cast<SharedHistory *>(memory);
        shared_history->count = 0;
        sem_init(&shared_history->semaphore, 1, 1);
      }
    }
  }
  if (shm_fd >= 0) close(shm_fd);

  const char *home = getenv("HOME");
  deque<string> recent;
  if (home) {
    ifstream f(string(home) + "/.terminator_history");
    string line;
    while (getline(f, line)) {
      if (line.empty()) continue;
      recent.push_back(line);
      if (recent.size() > HISTORY_LIMIT) recent.pop_front();
    }
  }

  lock_guard<mutex> lock(g_history_mutex);
  g_history.assign(recent.begin(), recent.end());
  if (shared_history) {
    sem_wait(&shared_history->semaphore);
    copy_history_to_shared();
    sem_post(&shared_history->semaphore);
  }
  if (home) {
    ofstream trimmed(string(home) + "/.terminator_history", ios::trunc);
    for (const string &entry : g_history) trimmed << entry << '\n';
  }
}

void cleanup_shared_history() {
  if (shared_history) {
    sem_destroy(&shared_history->semaphore);
    munmap(shared_history, sizeof(SharedHistory));
    shared_history = nullptr;
  }
  if (!shared_history_name.empty()) shm_unlink(shared_history_name.c_str());
  shared_history_name.clear();
}

vector<string> search_history(const string &term) {
  load_history();
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
