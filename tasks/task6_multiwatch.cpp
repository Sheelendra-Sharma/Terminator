#include "task6_multiwatch.h"

#include <csignal>
#include <ctime>
#include <poll.h>
#include <sys/wait.h>
#include <unistd.h>

static vector<string> parse_watch_cmds(const vector<string> &args) {
    vector<string> out;
    string joined;

    for (size_t i = 1; i < args.size(); i++) {
        if (i > 1) joined += ' ';
        joined += args[i];
    }

    size_t s = joined.find('['), e = joined.rfind(']');
    if (s == string::npos || e == string::npos) return out;
    joined = joined.substr(s + 1, e - s - 1);

    size_t i = 0;
    while (i < joined.size()) {
        if (joined[i] == '"') {
            size_t j = joined.find('"', i + 1);
            if (j == string::npos) break;
            out.push_back(joined.substr(i + 1, j - i - 1));
            i = j + 1;
        } else {
            i++;
        }
    }

    return out;
}

struct WatchJob {
    string cmd;
    pid_t  pid  = -1;
    int    rfd  = -1;
    string buf;
};

static volatile sig_atomic_t g_mw_stop = 0;
static void mw_sigint(int) { g_mw_stop = 1; }

static pid_t spawn_cmd(const string &cmd, int &rfd) {
    int p[2];
    if (pipe(p) != 0) return -1;

    pid_t pid = fork();
    if (pid == 0) {
        close(p[0]);
        dup2(p[1], 1);
        dup2(p[1], 2);
        close(p[1]);
        execl("/bin/sh", "sh", "-c", cmd.c_str(), nullptr);
        _exit(127);
    }

    close(p[1]);
    rfd = p[0];
    return pid;
}

int run_multiwatch(Tab &tab, const vector<string> &args) {
    vector<string> cmds = parse_watch_cmds(args);

    if (cmds.empty()) {
        append_line(tab, "terminator: multiWatch: no commands given");
        return 1;
    }

    struct sigaction old_sa, sa{};
    sa.sa_handler = mw_sigint;
    sigaction(SIGINT, &sa, &old_sa);
    g_mw_stop = 0;

    int n = (int)cmds.size();
    vector<WatchJob> jobs(n);
    for (int i = 0; i < n; i++) jobs[i].cmd = cmds[i];

    vector<time_t> started(n, 0);

    while (!g_mw_stop) {
        time_t now = time(nullptr);

        for (int i = 0; i < n; i++) {
            if (jobs[i].pid == -1 && now - started[i] >= 2) {
                jobs[i].buf.clear();
                jobs[i].pid = spawn_cmd(jobs[i].cmd, jobs[i].rfd);
                started[i]  = now;
            }
        }

        vector<pollfd> pfds;
        vector<int>    idx;
        for (int i = 0; i < n; i++) {
            if (jobs[i].rfd >= 0) {
                pfds.push_back({jobs[i].rfd, POLLIN, 0});
                idx.push_back(i);
            }
        }

        if (pfds.empty()) {
            usleep(50000);
            continue;
        }

        int ret = poll(pfds.data(), pfds.size(), 200);
        if (ret < 0) break;

        for (size_t k = 0; k < pfds.size(); k++) {
            if (!(pfds[k].revents & (POLLIN | POLLHUP))) continue;

            int i   = idx[k];
            char buf[4096];
            ssize_t r = read(jobs[i].rfd, buf, sizeof(buf));

            if (r > 0) {
                jobs[i].buf += string(buf, r);
            } else {
                close(jobs[i].rfd);
                jobs[i].rfd = -1;
                int status = 0;
                if (jobs[i].pid > 0) {
                    waitpid(jobs[i].pid, &status, 0);
                    jobs[i].pid = -1;
                }

                string sep(52, '-');
                string header = "\"" + jobs[i].cmd + "\", " +
                                to_string((long)time(nullptr)) + ":";
                append_line(tab, header);
                append_line(tab, sep);
                append_text(tab, jobs[i].buf);
                if (jobs[i].buf.empty() || jobs[i].buf.back() != '\n')
                    append_text(tab, "\n");
                append_line(tab, sep);
                flush_pending(tab);
            }
        }
    }

    for (auto &j : jobs) {
        if (j.pid > 0) {
            kill(j.pid, SIGTERM);
            waitpid(j.pid, nullptr, 0);
        }
        if (j.rfd >= 0) close(j.rfd);
    }

    sigaction(SIGINT, &old_sa, nullptr);
    g_mw_stop = 0;
    return 0;
}

