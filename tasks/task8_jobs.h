#pragma once

#include "common.h"

enum JobState { JOB_RUNNING, JOB_STOPPED, JOB_DONE };

struct Job {
    int id;
    pid_t pid;
    string cmd;
    JobState state;
};

extern vector<Job> g_jobs;
extern mutex g_jobs_mutex;

void jobs_add(pid_t pid, const string &cmd);
void jobs_reap(Tab &tab);
string jobs_list();
int jobs_fg(Tab &tab, int job_id);
int jobs_bg(Tab &tab, int job_id);
int run_background(Tab &tab, const string &raw_cmd);
bool is_background(const string &line, string &stripped);

