#pragma once

#include <string>

using namespace std;

int gui_run(bool process_mode);
int gui_event_fd();
int gui_poll_job_control();
void gui_close_worker_fds_in_child();
