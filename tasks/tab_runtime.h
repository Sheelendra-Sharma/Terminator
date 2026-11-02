#pragma once

#include "common.h"

void tab_runtime_init(bool process_mode, int gui_fd);
void tab_runtime_add(const Tab &tab);
void tab_runtime_remove(size_t index);
void tab_runtime_submit(size_t index, const string &command);
bool tab_runtime_busy(size_t index);
void tab_runtime_signal(size_t index, int signal);
void tab_runtime_publish(const Tab &tab);
bool tab_runtime_collect(vector<Tab> &tabs);
vector<int> tab_runtime_process_fds();
void tab_runtime_shutdown();

int tab_worker_event_fd();
bool tab_worker_poll_control(int &signal);
void tab_worker_close_fds_in_child();
