// Public entry points for the terminal GUI.

#pragma once

#include <string>

using namespace std;

// Start the GUI in thread or process mode.
int gui_run(bool process_mode);
int gui_event_fd();
int gui_poll_job_control();
void gui_close_worker_fds_in_child();
