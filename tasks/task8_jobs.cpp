#include "task8_jobs.h"

#include "task3_parser.h"
#include "task5_pipes.h"

#include <algorithm>
#include <csignal>
#include <sys/wait.h>
#include <unistd.h>

vector<Job> g_jobs;
mutex       g_jobs_mutex;

static int next_job_id() {
    int id = 1;
    for (auto &j : g_jobs)
        if (j.id >= id) id = j.id + 1;
    return id;
}

void jobs_add(pid_t pid, const string &cmd) {
    lock_guard<mutex> lock(g_jobs_mutex);
    g_jobs.push_back({next_job_id(), pid, cmd, JOB_RUNNING});
}

void jobs_reap(Tab &tab) {
    lock_guard<mutex> lock(g_jobs_mutex);

    for (auto &j : g_jobs) {
        if (j.state == JOB_DONE) continue;

        int status;
        pid_t r = waitpid(j.pid, &status, WNOHANG | WUNTRACED);

        if (r == j.pid) {
            if (WIFSTOPPED(status)) {
                j.state = JOB_STOPPED;
                append_line(tab, "[" + to_string(j.id) + "]+ Stopped\t" + j.cmd);
            } else {
                j.state = JOB_DONE;
                append_line(tab, "[" + to_string(j.id) + "]+ Done\t\t" + j.cmd);
            }
        }
    }

    g_jobs.erase(
        remove_if(g_jobs.begin(), g_jobs.end(),
                  [](const Job &j){ return j.state == JOB_DONE; }),
        g_jobs.end());
}

string jobs_list() {
    lock_guard<mutex> lock(g_jobs_mutex);
    string out;

    for (auto &j : g_jobs) {
        string state = (j.state == JOB_STOPPED) ? "Stopped" : "Running";
        out += "[" + to_string(j.id) + "]  " + state + "\t\t" + j.cmd + "\n";
    }

    return out;
}

int jobs_fg(Tab &tab, int job_id) {
    Job *found = nullptr;

    {
        lock_guard<mutex> lock(g_jobs_mutex);
        for (auto &j : g_jobs)
            if (j.id == job_id) { found = &j; break; }
    }

    if (!found) {
        append_line(tab, "terminator: fg: %" + to_string(job_id) + ": no such job");
        return 1;
    }

    append_line(tab, found->cmd);

    if (found->state == JOB_STOPPED)
        kill(found->pid, SIGCONT);

    found->state = JOB_RUNNING;

    int status;
    waitpid(found->pid, &status, WUNTRACED);

    {
        lock_guard<mutex> lock(g_jobs_mutex);
        g_jobs.erase(
            remove_if(g_jobs.begin(), g_jobs.end(),
                      [job_id](const Job &j){ return j.id == job_id; }),
            g_jobs.end());
    }

    if (WIFSTOPPED(status)) {
        jobs_add(found->pid, found->cmd);
        lock_guard<mutex> lock(g_jobs_mutex);
        g_jobs.back().state = JOB_STOPPED;
        append_line(tab, "[" + to_string(job_id) + "]+ Stopped\t" + found->cmd);
        return 128 + SIGTSTP;
    }

    return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
}

int jobs_bg(Tab &tab, int job_id) {
    lock_guard<mutex> lock(g_jobs_mutex);

    for (auto &j : g_jobs) {
        if (j.id == job_id) {
            kill(j.pid, SIGCONT);
            j.state = JOB_RUNNING;
            append_line(tab, "[" + to_string(j.id) + "]+ " + j.cmd + " &");
            return 0;
        }
    }

    append_line(tab, "terminator: bg: %" + to_string(job_id) + ": no such job");
    return 1;
}

bool is_background(const string &line, string &stripped) {
    char quote = 0;
    int  last_amp = -1;

    for (int i = 0; i < (int)line.size(); i++) {
        char c = line[i];

        if (quote == '\'') {
            if (c == '\'') quote = 0;
            continue;
        }
        if (quote == '"') {
            if (c == '"')  quote = 0;
            else if (c == '\\') i++;
            continue;
        }

        if (c == '\'' || c == '"') { quote = c; continue; }
        if (c == '\\') { i++; continue; }
        if (c == '&')  last_amp = i;
    }

    if (last_amp < 0) return false;

    for (int i = last_amp + 1; i < (int)line.size(); i++) {
        if (line[i] != ' ' && line[i] != '\t') return false;
    }

    stripped = line.substr(0, last_amp);
    while (!stripped.empty() && (stripped.back() == ' ' || stripped.back() == '\t'))
        stripped.pop_back();

    return true;
}

int run_background(Tab &tab, const string &raw_cmd) {
    vector<Command> cmds;
    string error;

    if (!parse_line(raw_cmd, tab, cmds, error)) {
        append_line(tab, error);
        return 1;
    }

    pid_t pid = fork();
    if (pid == 0) {
        int status = execute_pipeline(tab, cmds, nullptr);
        _exit(status);
    }
    if (pid < 0) return 1;

    jobs_add(pid, raw_cmd);

    {
        lock_guard<mutex> lock(g_jobs_mutex);
        int id = g_jobs.back().id;
        append_line(tab, "[" + to_string(id) + "] " + to_string(pid));
    }

    return 0;
}

