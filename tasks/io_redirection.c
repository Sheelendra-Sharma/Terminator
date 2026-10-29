#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "terminator.h"

void redirect_set(Cmd *c, TokType t, char *file)
{
    if (t == T_IN) {
        c->in_file = file;
    } else {
        c->out_file = file;
        c->append = (t == T_APPEND);
    }
}

static void get_path(Shell *sh, const char *file, char *buf, size_t n)
{
    if (file[0] == '/')
        snprintf(buf, n, "%s", file);
    else
        snprintf(buf, n, "%s/%s", sh->cwd, file);
}

int redirect_check(Shell *sh, Cmd *c)
{
    if (!c->in_file)
        return 0;
    char path[PATH_MAX * 2];
    get_path(sh, c->in_file, path, sizeof(path));
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        say(sh->err_fd, "terminator: %s: %s\n", c->in_file, strerror(errno));
        return -1;
    }
    close(fd);
    return 0;
}

int redirect_open_out(Shell *sh, Cmd *c)
{
    char path[PATH_MAX * 2];
    get_path(sh, c->out_file, path, sizeof(path));
    int flags = O_WRONLY | O_CREAT | (c->append ? O_APPEND : O_TRUNC);
    int fd = open(path, flags, 0644);
    if (fd < 0)
        say(sh->err_fd, "terminator: %s: %s\n", c->out_file, strerror(errno));
    return fd;
}

void redirect_apply(Cmd *c)
{
    if (c->in_file) {
        int fd = open(c->in_file, O_RDONLY);
        if (fd < 0) {
            say(2, "terminator: %s: %s\n", c->in_file, strerror(errno));
            _exit(1);
        }
        dup2(fd, 0);
        close(fd);
    }
    if (c->out_file) {
        int flags = O_WRONLY | O_CREAT | (c->append ? O_APPEND : O_TRUNC);
        int fd = open(c->out_file, flags, 0644);
        if (fd < 0) {
            say(2, "terminator: %s: %s\n", c->out_file, strerror(errno));
            _exit(1);
        }
        dup2(fd, 1);
        close(fd);
    }
}
