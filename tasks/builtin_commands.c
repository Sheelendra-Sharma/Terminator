#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include "terminator.h"

History *hist;

void put(int fd, const char *s, size_t n)
{
    while (n > 0) {
        ssize_t w = write(fd, s, n);
        if (w < 0) {
            if (errno == EINTR)
                continue;
            return;
        }
        s += w;
        n -= w;
    }
}

void say(int fd, const char *fmt, ...)
{
    char buf[PATH_MAX * 2 + 256];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (n < 0)
        return;
    if ((size_t)n >= sizeof buf)
        n = sizeof buf - 1;
    put(fd, buf, n);
}

void history_init(void)
{
    hist = mmap(NULL, sizeof(History), PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (hist == MAP_FAILED) {
        perror("terminator: mmap");
        exit(1);
    }
    sem_init(&hist->lock, 1, 1);
    hist->total = 0;
}

void history_add(const char *line)
{
    char tmp[HIST_LEN];
    size_t n = 0;
    for (; line[n] && n < HIST_LEN - 1; n++)
        tmp[n] = line[n] == '\n' ? ' ' : line[n];
    tmp[n] = 0;
    sem_wait(&hist->lock);
    memcpy(hist->cmd[hist->total % HIST_MAX], tmp, n + 1);
    hist->total++;
    sem_post(&hist->lock);
}

void history_print(int fd)
{
    size_t cap = (size_t)HIST_SHOW * (HIST_LEN + 16);
    char *out = malloc(cap);
    size_t len = 0;
    if (!out)
        return;
    sem_wait(&hist->lock);
    long start = hist->total > HIST_SHOW ? hist->total - HIST_SHOW : 0;
    for (long i = start; i < hist->total; i++) {
        int n = snprintf(out + len, cap - len, "%5ld  %s\n", i + 1, hist->cmd[i % HIST_MAX]);
        if (n < 0 || (size_t)n >= cap - len)
            break;
        len += n;
    }
    sem_post(&hist->lock);
    put(fd, out, len);
    free(out);
}

static void send_prompt(Shell *sh)
{
    char title[PATH_MAX + 8];
    const char *home = getenv("HOME");
    size_t hl = home ? strlen(home) : 0;
    if (hl > 0 && strncmp(sh->cwd, home, hl) == 0 && (sh->cwd[hl] == '/' || sh->cwd[hl] == 0))
        snprintf(title, sizeof title, "~%s", sh->cwd + hl);
    else
        snprintf(title, sizeof title, "%s", sh->cwd);
    say(sh->out_fd, "%c%s\n%s", TITLE_MARK, title, PROMPT);
}

static int do_cd(Shell *sh, int argc, char **argv)
{
    const char *home = getenv("HOME");
    const char *arg = argc > 1 ? argv[1] : home;
    char cand[PATH_MAX * 2 + 2], real[PATH_MAX];
    struct stat st;
    if (!arg) {
        say(sh->err_fd, "terminator: cd: HOME not set\n");
        return 1;
    }
    if (arg[0] == '/')
        snprintf(cand, sizeof cand, "%s", arg);
    else
        snprintf(cand, sizeof cand, "%s/%s", sh->cwd, arg);
    if (!realpath(cand, real) || stat(real, &st) < 0) {
        say(sh->err_fd, "terminator: cd: %s: %s\n", arg, strerror(errno));
        return 1;
    }
    if (!S_ISDIR(st.st_mode)) {
        say(sh->err_fd, "terminator: cd: %s: %s\n", arg, strerror(ENOTDIR));
        return 1;
    }
    snprintf(sh->cwd, sizeof sh->cwd, "%s", real);
    return 0;
}

int is_builtin(const char *name)
{
    return strcmp(name, "cd") == 0 || strcmp(name, "exit") == 0 || strcmp(name, "history") == 0;
}

int run_builtin(Shell *sh, int argc, char **argv, int out_fd)
{
    if (strcmp(argv[0], "cd") == 0)
        return do_cd(sh, argc, argv);
    if (strcmp(argv[0], "exit") == 0) {
        sh->done = 1;
        return argc > 1 ? atoi(argv[1]) : sh->status;
    }
    history_print(out_fd);
    return 0;
}

int execute_line(Shell *sh, const char *line)
{
    TokList tl = {0};
    if (tokenize(sh, line, &tl) < 0) {
        toklist_free(&tl);
        return 2;
    }
    int st = run_tokens(sh, &tl);
    toklist_free(&tl);
    return st;
}

static int blank(const char *s)
{
    for (; *s; s++)
        if (*s != ' ' && *s != '\t' && *s != '\n')
            return 0;
    return 1;
}

void shell_run(int in_fd, int out_fd)
{
    Shell sh;
    memset(&sh, 0, sizeof sh);
    sh.in_fd = in_fd;
    sh.out_fd = out_fd;
    sh.err_fd = out_fd;
    if (!getcwd(sh.cwd, sizeof sh.cwd))
        strcpy(sh.cwd, "/");
    while (!sh.done) {
        char *line = NULL;
        send_prompt(&sh);
        if (read_command(&sh, &line) < 0)
            break;
        if (!blank(line)) {
            history_add(line);
            sh.status = execute_line(&sh, line);
        }
        free(line);
    }
    close(in_fd);
    close(out_fd);
}
