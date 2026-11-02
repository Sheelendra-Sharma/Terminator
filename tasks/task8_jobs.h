#pragma once

#include "common.h"

enum JobState { JOB_RUNNING, JOB_STOPPED, JOB_DONE };

struct Job {
    int id;
    pid_t pid;
    string cmd;
    JobState state;
    int output_fd = -1;
};

extern thread_local vector<Job> g_jobs;
extern thread_local mutex g_jobs_mutex;

void jobs_add(pid_t pid, const string &cmd);
void jobs_add_stopped(Tab &tab, pid_t pgid, const string &cmd, int output_fd);
void jobs_reap(Tab &tab);
void jobs_cleanup();
string jobs_list();
int jobs_fg(Tab &tab, int job_id, bool interactive = true);
int jobs_bg(Tab &tab, int job_id);
int run_background(Tab &tab, const string &raw_cmd);
bool is_background(const string &line, string &stripped);

