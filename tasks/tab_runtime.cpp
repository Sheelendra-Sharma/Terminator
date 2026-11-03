// Run each tab in a thread or child process.

#include "tab_runtime.h"
#include "gui.h"
#include "pipes.h"
#include "jobs.h"

#include <atomic>
#include <condition_variable>
#include <csignal>
#include <cstring>
#include <deque>
#include <fcntl.h>
#include <memory>
#include <mutex>
#include <poll.h>
#include <thread>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {
struct Snapshot {
  vector<Line> lines;
  string pending, cwd;
  int status = 0;
  bool exit_requested = false;
};

struct Worker {
  bool process = false, stopping = false, dead = false, error_reported = false;
  int fd = -1, control_fd = -1;
  pid_t pid = -1;
  atomic<bool> busy{false};
  mutex lock;
  condition_variable ready;
  deque<string> commands;
  deque<Snapshot> updates;
  Tab shell;
  vector<unsigned char> received;
  vector<Line> displayed_lines;
  string displayed_pending;
  thread worker_thread;
};

vector<shared_ptr<Worker>> workers;
bool use_processes = false;
int gui_connection = -1;
thread_local int control_read_fd = -1;
thread_local int control_write_fd = -1;
thread_local int command_fd = -1;
thread_local Worker *current_worker = nullptr;

// Transfer complete messages over the worker IPC channels.
bool read_exact(int fd, void *buffer, size_t size) {
  char *out = static_cast<char *>(buffer);
  while (size) {
    ssize_t n = read(fd, out, size);
    if (n <= 0) return false;
    out += n;
    size -= (size_t)n;
  }
  return true;
}

bool write_exact(int fd, const void *buffer, size_t size) {
  const char *in = static_cast<const char *>(buffer);
  while (size) {
    ssize_t n = write(fd, in, size);
    if (n <= 0) return false;
    in += n;
    size -= (size_t)n;
  }
  return true;
}

bool send_exact(int fd, const void *buffer, size_t size) {
  return write_exact(fd, buffer, size);
}

void set_cloexec(int fd) {
  int flags = fcntl(fd, F_GETFD);
  if (flags >= 0) fcntl(fd, F_SETFD, flags | FD_CLOEXEC);
}

void put_u32(vector<unsigned char> &data, uint32_t value) {
  const auto *bytes = reinterpret_cast<const unsigned char *>(&value);
  data.insert(data.end(), bytes, bytes + sizeof(value));
}

bool get_u32(const vector<unsigned char> &data, size_t &pos, uint32_t &value) {
  if (pos + sizeof(value) > data.size()) return false;
  memcpy(&value, data.data() + pos, sizeof(value));
  pos += sizeof(value);
  return true;
}

void put_string(vector<unsigned char> &data, const string &value) {
  put_u32(data, (uint32_t)value.size());
  data.insert(data.end(), value.begin(), value.end());
}

bool get_string(const vector<unsigned char> &data, size_t &pos, string &value) {
  uint32_t size;
  if (!get_u32(data, pos, size) || pos + size > data.size()) return false;
  value.assign(reinterpret_cast<const char *>(data.data() + pos), size);
  pos += size;
  return true;
}

vector<unsigned char> encode(const Tab &tab) {
  vector<unsigned char> data;
  put_u32(data, (uint32_t)tab.last_status);
  put_u32(data, tab.exit_requested ? 1 : 0);
  put_string(data, tab.cwd);
  put_u32(data, (uint32_t)tab.lines.size());
  for (const Line &line : tab.lines) {
    put_u32(data, (uint32_t)line.kind);
    put_string(data, line.text);
  }
  put_string(data, tab.pending);
  return data;
}

bool decode(const vector<unsigned char> &data, Snapshot &snapshot) {
  size_t pos = 0;
  uint32_t status, exit_requested, count;
  if (!get_u32(data, pos, status) || !get_u32(data, pos, exit_requested) ||
      !get_string(data, pos, snapshot.cwd) || !get_u32(data, pos, count)) return false;
  snapshot.status = (int)status;
  snapshot.exit_requested = exit_requested != 0;
  for (uint32_t i = 0; i < count; i++) {
    uint32_t kind;
    string text;
    if (!get_u32(data, pos, kind) || !get_string(data, pos, text)) return false;
    snapshot.lines.push_back({text, (int)kind});
  }
  return get_string(data, pos, snapshot.pending) && pos == data.size();
}

void send_snapshot(int fd, const Tab &tab) {
  vector<unsigned char> data = encode(tab);
  uint32_t length = (uint32_t)data.size();
  if (send_exact(fd, &length, sizeof(length))) send_exact(fd, data.data(), data.size());
}

void process_loop(int fd, int control_fd, Tab shell) {
  command_fd = fd;
  control_read_fd = control_fd;
  uint32_t length;
  while (read_exact(fd, &length, sizeof(length)) && length != UINT32_MAX) {
    if (length > 1024 * 1024) break;
    string command(length, '\0');
    if (!read_exact(fd, command.data(), length)) break;
    run_line(shell, command, true, false);
    send_snapshot(fd, shell);
    if (shell.exit_requested) break;
  }
  jobs_cleanup();
  close(control_fd);
  close(fd);
}

void thread_loop(const shared_ptr<Worker> &worker) {
  current_worker = worker.get();
  control_read_fd = worker->fd;
  control_write_fd = worker->control_fd;
  for (;;) {
    string command;
    {
      unique_lock<mutex> lock(worker->lock);
      worker->ready.wait(lock, [&] { return worker->stopping || !worker->commands.empty(); });
      if (worker->stopping) {
        jobs_cleanup();
        control_read_fd = control_write_fd = -1;
        current_worker = nullptr;
        if (worker->fd >= 0) close(worker->fd);
        worker->fd = -1;
        return;
      }
      command = move(worker->commands.front());
      worker->commands.pop_front();
    }
    worker->busy = true;
    run_line(worker->shell, command, true, true);
    worker->busy = false;
    Snapshot snapshot{worker->shell.lines, worker->shell.pending, worker->shell.cwd,
                      worker->shell.last_status, worker->shell.exit_requested};
    lock_guard<mutex> lock(worker->lock);
    worker->updates.push_back(move(snapshot));
  }
}

shared_ptr<Worker> create_worker(const Tab &tab) {
  auto worker = make_shared<Worker>();
  worker->process = use_processes;
  worker->shell = tab;
  if (!use_processes) {
    int control[2];
    if (pipe(control) != 0) {
      worker->dead = true;
      return worker;
    }
    set_cloexec(control[0]);
    set_cloexec(control[1]);
    worker->fd = control[0];
    worker->control_fd = control[1];
    worker->worker_thread = thread(thread_loop, worker);
    return worker;
  }

  int commands[2], controls[2];
  if (socketpair(AF_UNIX, SOCK_STREAM, 0, commands) != 0) {
    worker->dead = true;
    return worker;
  }
  if (socketpair(AF_UNIX, SOCK_STREAM, 0, controls) != 0) {
    close(commands[0]);
    close(commands[1]);
    worker->dead = true;
    return worker;
  }
  for (int fd : commands) set_cloexec(fd);
  for (int fd : controls) set_cloexec(fd);

  pid_t pid = fork();
  if (pid == 0) {
    close(commands[0]);
    close(controls[0]);
    gui_close_worker_fds_in_child();
    for (const auto &existing : workers) {
      if (existing->fd >= 0) close(existing->fd);
      if (existing->control_fd >= 0) close(existing->control_fd);
    }
    setpgid(0, 0);
    process_loop(commands[1], controls[1], tab);
    _exit(0);
  }
  close(commands[1]);
  close(controls[1]);
  if (pid < 0) {
    close(commands[0]);
    close(controls[0]);
    worker->dead = true;
    return worker;
  }
  worker->pid = pid;
  worker->fd = commands[0];
  int flags = fcntl(worker->fd, F_GETFL);
  if (flags >= 0) fcntl(worker->fd, F_SETFL, flags | O_NONBLOCK);
  worker->control_fd = controls[0];
  return worker;
}

// Stop a tab before releasing its thread or child process.
void stop_worker(const shared_ptr<Worker> &worker) {
  if (!worker) return;
  if (!worker->process) {
    if (worker->busy && worker->control_fd >= 0) {
      int signal = SIGINT;
      write_exact(worker->control_fd, &signal, sizeof(signal));
    }
    {
      lock_guard<mutex> lock(worker->lock);
      worker->stopping = true;
      worker->commands.clear();
      worker->ready.notify_one();
    }
    if (worker->worker_thread.joinable()) worker->worker_thread.join();
    if (worker->control_fd >= 0) close(worker->control_fd);
  } else {
    if (worker->busy && worker->control_fd >= 0) {
      int signal = SIGINT;
      send_exact(worker->control_fd, &signal, sizeof(signal));
    }
    if (worker->fd >= 0) close(worker->fd);
    if (worker->pid > 0) waitpid(worker->pid, nullptr, 0);
    if (worker->control_fd >= 0) close(worker->control_fd);
  }
}

bool same_line(const Line &a, const Line &b) {
  return a.kind == b.kind && a.text == b.text;
}

void read_process_updates(const shared_ptr<Worker> &worker) {
  unsigned char bytes[8192];
  bool eof = false;
  for (;;) {
    ssize_t n = read(worker->fd, bytes, sizeof(bytes));
    if (n == 0) { eof = true; break; }
    if (n < 0) break;
    worker->received.insert(worker->received.end(), bytes, bytes + n);
  }

  size_t consumed = 0;
  while (worker->received.size() - consumed >= sizeof(uint32_t)) {
    uint32_t length;
    memcpy(&length, worker->received.data() + consumed, sizeof(length));
    if (length > 64 * 1024 * 1024) {
      worker->received.clear();
      break;
    }
    if (worker->received.size() - consumed < sizeof(uint32_t) + length) break;
    vector<unsigned char> data(worker->received.begin() + consumed + sizeof(uint32_t),
                               worker->received.begin() + consumed + sizeof(uint32_t) + length);
    Snapshot snapshot;
    if (decode(data, snapshot)) {
      lock_guard<mutex> lock(worker->lock);
      worker->updates.push_back(move(snapshot));
    }
    consumed += sizeof(uint32_t) + length;
  }
  if (consumed) worker->received.erase(worker->received.begin(), worker->received.begin() + consumed);
  if (eof) {
    close(worker->fd);
    worker->fd = -1;
    worker->busy = false;
    worker->dead = true;
  }
}
}  // namespace

void tab_runtime_init(bool process_mode, int gui_fd) {
  use_processes = process_mode;
  gui_connection = gui_fd;
}

void tab_runtime_add(const Tab &tab) {
  workers.push_back(create_worker(tab));
}

void tab_runtime_remove(size_t index) {
  if (index >= workers.size()) return;
  stop_worker(workers[index]);
  workers.erase(workers.begin() + index);
}

void tab_runtime_submit(size_t index, const string &command) {
  if (index >= workers.size()) return;
  const auto &worker = workers[index];
  if (worker->dead) return;
  if (!worker->process) {
    lock_guard<mutex> lock(worker->lock);
    worker->commands.push_back(command);
    worker->busy = true;
    worker->ready.notify_one();
    return;
  }
  if (worker->fd < 0) return;
  worker->busy = true;
  uint32_t length = (uint32_t)command.size();
  if (!send_exact(worker->fd, &length, sizeof(length)) ||
      !send_exact(worker->fd, command.data(), command.size())) worker->busy = false;
}

bool tab_runtime_busy(size_t index) {
  return index < workers.size() && workers[index]->busy;
}

void tab_runtime_signal(size_t index, int signal) {
  if (index >= workers.size()) return;
  const auto &worker = workers[index];
  if (worker->control_fd < 0) return;
  if (worker->process) send_exact(worker->control_fd, &signal, sizeof(signal));
  else write_exact(worker->control_fd, &signal, sizeof(signal));
}

void tab_runtime_publish(const Tab &tab) {
  if (!current_worker) return;
  Snapshot snapshot{tab.lines, tab.pending, tab.cwd, tab.last_status, tab.exit_requested};
  lock_guard<mutex> lock(current_worker->lock);
  current_worker->updates.push_back(move(snapshot));
}

bool tab_runtime_collect(vector<Tab> &tabs) {
  bool changed = false;
  for (size_t i = 0; i < workers.size() && i < tabs.size(); i++) {
    const auto &worker = workers[i];
    if (worker->process && worker->fd >= 0) read_process_updates(worker);
    deque<Snapshot> updates;
    {
      lock_guard<mutex> lock(worker->lock);
      updates.swap(worker->updates);
    }
    for (const Snapshot &snapshot : updates) {
      Tab &tab = tabs[i];
      bool prefix = worker->displayed_lines.size() <= snapshot.lines.size();
      for (size_t j = 0; prefix && j < worker->displayed_lines.size(); j++)
        prefix = same_line(worker->displayed_lines[j], snapshot.lines[j]);
      if (!prefix) {
        tab.lines.clear();
        tab.pending.clear();
        tab.scroll = 0;
      }
      size_t start = prefix ? worker->displayed_lines.size() : 0;
      for (size_t j = start; j < snapshot.lines.size(); j++)
        append_line(tab, snapshot.lines[j].text, snapshot.lines[j].kind);
      if (snapshot.pending.rfind(worker->displayed_pending, 0) == 0)
        append_text(tab, snapshot.pending.substr(worker->displayed_pending.size()));
      else if (!snapshot.pending.empty()) {
        flush_pending(tab);
        append_text(tab, snapshot.pending);
      }
      worker->displayed_lines = snapshot.lines;
      worker->displayed_pending = snapshot.pending;
      tab.cwd = snapshot.cwd;
      tab.last_status = snapshot.status;
      tab.exit_requested = snapshot.exit_requested;
      if (worker->process) worker->busy = false;
      tab.scroll = 0;
      changed = true;
    }
    if (worker->dead && !worker->error_reported && !tabs[i].exit_requested) {
      append_line(tabs[i], "terminator: tab worker stopped unexpectedly");
      worker->error_reported = true;
      worker->busy = false;
      changed = true;
    }
  }
  return changed;
}

vector<int> tab_runtime_process_fds() {
  vector<int> fds;
  for (const auto &worker : workers)
    if (worker->process && worker->fd >= 0) fds.push_back(worker->fd);
  return fds;
}

void tab_runtime_shutdown() {
  for (const auto &worker : workers) stop_worker(worker);
  workers.clear();
}

int tab_worker_event_fd() {
  return control_read_fd;
}

bool tab_worker_poll_control(int &signal) {
  if (control_read_fd < 0) return false;
  pollfd fd{control_read_fd, POLLIN, 0};
  if (poll(&fd, 1, 0) <= 0 || !(fd.revents & (POLLIN | POLLHUP | POLLERR)))
    return false;
  signal = 0;
  ssize_t n = read(control_read_fd, &signal, sizeof(signal));
  return n == (ssize_t)sizeof(signal);
}

void tab_worker_close_fds_in_child() {
  if (control_read_fd >= 0) close(control_read_fd);
  if (control_write_fd >= 0) close(control_write_fd);
  if (command_fd >= 0) close(command_fd);
  if (gui_connection >= 0) close(gui_connection);
  control_read_fd = control_write_fd = command_fd = -1;
  gui_connection = -1;
}
