#include "task8_jobs.h"

#include "task3_parser.h"
#include "task1_gui.h"
#include "task5_pipes.h"

#include <algorithm>
#include <cerrno>
#include <csignal>
#include <poll.h>
#include <sys/wait.h>
#include <unistd.h>

thread_local vector<Job> g_jobs;
thread_local mutex g_jobs_mutex;

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

void jobs_add_stopped(Tab &tab, pid_t pgid, const string &cmd, int output_fd) {
    jobs_add(pgid, cmd);
    lock_guard<mutex> lock(g_jobs_mutex);
    Job &job = g_jobs.back();
    job.state = JOB_STOPPED;
    job.output_fd = output_fd;
    append_line(tab, "[" + to_string(job.id) + "] Stopped\t" + cmd);
}

void jobs_reap(Tab &tab) {
    lock_guard<mutex> lock(g_jobs_mutex);

    for (auto &j : g_jobs) {
        if (j.state == JOB_DONE) continue;

        int status;
        pid_t r;
        while ((r = waitpid(-j.pid, &status, WNOHANG | WUNTRACED)) > 0) {
            if (WIFSTOPPED(status)) {
                j.state = JOB_STOPPED;
                append_line(tab, "[" + to_string(j.id) + "]+ Stopped\t" + j.cmd);
            }
        }
        if (r < 0 && errno == ECHILD) {
            j.state = JOB_DONE;
            if (j.output_fd >= 0) close(j.output_fd);
            append_line(tab, "[" + to_string(j.id) + "]+ Done\t\t" + j.cmd);
        }
    }

    g_jobs.erase(
        remove_if(g_jobs.begin(), g_jobs.end(),
                  [](const Job &j){ return j.state == JOB_DONE; }),
        g_jobs.end());
}

void jobs_cleanup() {
    vector<pid_t> pids;
    {
        lock_guard<mutex> lock(g_jobs_mutex);
        for (const Job &job : g_jobs) {
            pids.push_back(job.pid);
            if (job.output_fd >= 0) close(job.output_fd);
        }
        g_jobs.clear();
    }
    for (pid_t pid : pids) {
        kill(-pid, SIGCONT);
        kill(-pid, SIGTERM);
    }
    for (pid_t pid : pids) {
        while (waitpid(-pid, nullptr, 0) > 0) {}
    }
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

int jobs_fg(Tab &tab, int job_id, bool interactive) {
    Job job{};
    bool found = false;

    {
        lock_guard<mutex> lock(g_jobs_mutex);
        for (auto &j : g_jobs)
            if (j.id == job_id) { job = j; found = true; break; }
    }

    if (!found) {
        append_line(tab, "terminator: fg: %" + to_string(job_id) + ": no such job");
        return 1;
    }

    append_line(tab, job.cmd);

    if (job.state == JOB_STOPPED) kill(-job.pid, SIGCONT);

    {
        lock_guard<mutex> lock(g_jobs_mutex);
        for (auto &entry : g_jobs)
            if (entry.id == job_id) entry.state = JOB_RUNNING;
    }

    int status = 0;
    bool interrupted = false;
    int xfd = interactive ? gui_event_fd() : -1;
    int output_fd = job.output_fd;
    while (true) {
        if (xfd >= 0 || output_fd >= 0) {
            pollfd fds[2] = {
                {output_fd, POLLIN, 0},
                {xfd, POLLIN, 0}
            };
            poll(fds, 2, xfd >= 0 ? 100 : -1);
            if (output_fd >= 0 && fds[0].revents & (POLLIN | POLLHUP)) {
                char buffer[4096];
                ssize_t count = read(output_fd, buffer, sizeof(buffer));
                if (count > 0) append_text(tab, string(buffer, count));
                else {
                    close(output_fd);
                    output_fd = -1;
                }
            }
            int signal = interactive ? gui_poll_job_control() : 0;
            if (signal == SIGINT || signal == SIGTSTP) {
                kill(-job.pid, signal);
                if (signal == SIGINT) append_line(tab, "^C");
                interrupted |= signal == SIGINT;
            }
        }

        pid_t waited = waitpid(-job.pid, &status,
                               (xfd >= 0 || output_fd >= 0 ? WNOHANG : 0) | WUNTRACED);
        if (waited > 0 && WIFSTOPPED(status)) {
            lock_guard<mutex> lock(g_jobs_mutex);
            for (auto &entry : g_jobs)
                if (entry.id == job_id) {
                    entry.state = JOB_STOPPED;
                    entry.output_fd = output_fd;
                }
            append_line(tab, "[" + to_string(job_id) + "]+ Stopped\t" + job.cmd);
            return 128 + SIGTSTP;
        }
        if (waited < 0 && errno == ECHILD) break;
        if (waited == 0 || waited > 0) continue;
    }

    {
        lock_guard<mutex> lock(g_jobs_mutex);
        if (output_fd >= 0) close(output_fd);
        g_jobs.erase(
            remove_if(g_jobs.begin(), g_jobs.end(),
                      [job_id](const Job &j){ return j.id == job_id; }),
            g_jobs.end());
    }

    if (interrupted) return 128 + SIGINT;
    return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
}

int jobs_bg(Tab &tab, int job_id) {
    lock_guard<mutex> lock(g_jobs_mutex);

    for (auto &j : g_jobs) {
        if (j.id == job_id) {
            kill(-j.pid, SIGCONT);
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
        gui_close_worker_fds_in_child();
        setpgid(0, 0);
        int status = execute_pipeline_command(tab, cmds, raw_cmd, false);
        _exit(status);
    }
    if (pid < 0) return 1;
    setpgid(pid, pid);

    jobs_add(pid, raw_cmd);

    {
        lock_guard<mutex> lock(g_jobs_mutex);
        int id = g_jobs.back().id;
        append_line(tab, "[" + to_string(id) + "] " + to_string(pid));
    }

    return 0;
}

