#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#include "terminator.h"

static int cmd_empty(Cmd *c)
{
    return c->argc == 0 && !c->in_file && !c->out_file;
}

static int decode(int st)
{
    if (WIFEXITED(st))
        return WEXITSTATUS(st);
    if (WIFSIGNALED(st))
        return 128 + WTERMSIG(st);
    return 1;
}

static void child_main(Shell *sh, Cmd *c, int in_fd, int out_fd, int *fds, int nfds)
{
    signal(SIGPIPE, SIG_DFL);
    if (chdir(sh->cwd) < 0)
        _exit(126);
    int z = open("/dev/null", O_RDONLY);
    if (z >= 0) {
        dup2(z, 0);
        close(z);
    }
    dup2(sh->out_fd, 1);
    dup2(sh->err_fd, 2);
    if (in_fd >= 0)
        dup2(in_fd, 0);
    if (out_fd >= 0)
        dup2(out_fd, 1);
    for (int i = 0; i < nfds; i++)
        close(fds[i]);
    redirect_apply(c);
    if (c->argc == 0)
        _exit(0);
    if (is_builtin(c->argv[0]))
        _exit(run_builtin(sh, c->argc, c->argv, 1));
    execvp(c->argv[0], c->argv);
    if (errno == ENOENT && !strchr(c->argv[0], '/')) {
        say(2, "terminator: %s: command not found\n", c->argv[0]);
        _exit(127);
    }
    say(2, "terminator: %s: %s\n", c->argv[0], strerror(errno));
    _exit(errno == ENOENT ? 127 : 126);
}

static int run_pipeline(Shell *sh, Cmd *cmds, int n)
{
    int nfds = 2 * (n - 1);
    int *fds = malloc((nfds > 0 ? nfds : 1) * sizeof(int));
    pid_t *pids = calloc(n, sizeof(pid_t));
    int status = 0;
    for (int i = 0; i < n - 1; i++) {
        if (pipe(fds + 2 * i) < 0) {
            say(sh->err_fd, "terminator: pipe failed\n");
            for (int k = 0; k < 2 * i; k++)
                close(fds[k]);
            free(fds);
            free(pids);
            return 1;
        }
    }
    for (int i = 0; i < n; i++) {
        int in_fd = i > 0 ? fds[2 * (i - 1)] : -1;
        int out_fd = i < n - 1 ? fds[2 * i + 1] : -1;
        pids[i] = fork();
        if (pids[i] == 0)
            child_main(sh, &cmds[i], in_fd, out_fd, fds, nfds);
        if (pids[i] < 0)
            say(sh->err_fd, "terminator: fork failed\n");
    }
    for (int k = 0; k < nfds; k++)
        close(fds[k]);
    for (int i = 0; i < n; i++) {
        int st = 0;
        if (pids[i] > 0) {
            while (waitpid(pids[i], &st, 0) < 0 && errno == EINTR)
                ;
            if (i == n - 1)
                status = decode(st);
        } else if (i == n - 1) {
            status = 1;
        }
    }
    free(fds);
    free(pids);
    return status;
}

int run_tokens(Shell *sh, TokList *tl)
{
    int n = 1, ci = 0, bad = 0, status;
    for (int i = 0; i < tl->n; i++)
        if (tl->v[i].type == T_PIPE)
            n++;
    Cmd *cmds = calloc(n, sizeof(Cmd));
    for (int i = 0; i < tl->n && !bad; i++) {
        Token *t = &tl->v[i];
        Cmd *c = &cmds[ci];
        if (t->type == T_WORD) {
            if (c->argc >= MAXARGS - 1)
                bad = 1;
            else
                c->argv[c->argc++] = t->text;
        } else if (t->type == T_PIPE) {
            if (cmd_empty(c))
                bad = 1;
            else
                ci++;
        } else if (i + 1 >= tl->n || tl->v[i + 1].type != T_WORD) {
            bad = 1;
        } else {
            redirect_set(c, t->type, tl->v[i + 1].text);
            i++;
        }
    }
    if (!bad && n > 1 && cmd_empty(&cmds[n - 1]))
        bad = 1;
    if (bad) {
        say(sh->err_fd, "terminator: syntax error\n");
        free(cmds);
        return 2;
    }
    if (n == 1 && cmds[0].argc == 0) {
        status = (cmds[0].in_file || cmds[0].out_file) ? 0 : sh->status;
        free(cmds);
        return status;
    }
    for (int i = 0; i < n; i++) {
        if (redirect_check(sh, &cmds[i]) < 0) {
            free(cmds);
            return 1;
        }
    }
    if (n == 1 && is_builtin(cmds[0].argv[0])) {
        int fd = sh->out_fd, opened = 0;
        if (cmds[0].out_file) {
            fd = redirect_open_out(sh, &cmds[0]);
            if (fd < 0) {
                free(cmds);
                return 1;
            }
            opened = 1;
        }
        status = run_builtin(sh, cmds[0].argc, cmds[0].argv, fd);
        if (opened)
            close(fd);
        free(cmds);
        return status;
    }
    status = run_pipeline(sh, cmds, n);
    free(cmds);
    return status;
}
