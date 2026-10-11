#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <glob.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#include "terminator.h"

typedef struct {
    char *s;
    int len, cap, started, glob;
} Word;

static int skip_subst(const char *s, int i);

static int skip_sq(const char *s, int i)
{
    while (s[i] && s[i] != '\'')
        i++;
    return s[i] ? i : -1;
}

static int skip_bt(const char *s, int i)
{
    while (s[i]) {
        if (s[i] == '\\') {
            if (!s[i + 1])
                return -1;
            i += 2;
            continue;
        }
        if (s[i] == '`')
            return i;
        i++;
    }
    return -1;
}

static int skip_dq(const char *s, int i)
{
    int j;
    while (s[i]) {
        if (s[i] == '\\') {
            if (!s[i + 1])
                return -1;
            i += 2;
        } else if (s[i] == '"') {
            return i;
        } else if (s[i] == '`') {
            j = skip_bt(s, i + 1);
            if (j < 0)
                return -1;
            i = j + 1;
        } else if (s[i] == '$' && s[i + 1] == '(') {
            j = skip_subst(s, i + 2);
            if (j < 0)
                return -1;
            i = j + 1;
        } else {
            i++;
        }
    }
    return -1;
}

static int skip_subst(const char *s, int i)
{
    int depth = 0, j;
    while (s[i]) {
        char c = s[i];
        if (c == '\\') {
            if (!s[i + 1])
                return -1;
            i += 2;
        } else if (c == '\'') {
            j = skip_sq(s, i + 1);
            if (j < 0)
                return -1;
            i = j + 1;
        } else if (c == '"') {
            j = skip_dq(s, i + 1);
            if (j < 0)
                return -1;
            i = j + 1;
        } else if (c == '`') {
            j = skip_bt(s, i + 1);
            if (j < 0)
                return -1;
            i = j + 1;
        } else if (c == '$' && s[i + 1] == '(') {
            j = skip_subst(s, i + 2);
            if (j < 0)
                return -1;
            i = j + 1;
        } else if (c == '(') {
            depth++;
            i++;
        } else if (c == ')') {
            if (depth == 0)
                return i;
            depth--;
            i++;
        } else {
            i++;
        }
    }
    return -1;
}

int needs_more(const char *s)
{
    int i = 0, j;
    while (s[i]) {
        char c = s[i];
        if (c == '\\') {
            if (!s[i + 1])
                return 1;
            i += 2;
        } else if (c == '\'') {
            j = skip_sq(s, i + 1);
            if (j < 0)
                return 1;
            i = j + 1;
        } else if (c == '"') {
            j = skip_dq(s, i + 1);
            if (j < 0)
                return 1;
            i = j + 1;
        } else if (c == '`') {
            j = skip_bt(s, i + 1);
            if (j < 0)
                return 1;
            i = j + 1;
        } else if (c == '$' && s[i + 1] == '(') {
            j = skip_subst(s, i + 2);
            if (j < 0)
                return 1;
            i = j + 1;
        } else if (c == '#' && (i == 0 || isspace((unsigned char)s[i - 1]))) {
            while (s[i] && s[i] != '\n')
                i++;
        } else {
            i++;
        }
    }
    int k = (int)strlen(s) - 1;
    while (k >= 0 && isspace((unsigned char)s[k]))
        k--;
    return k >= 0 && s[k] == '|';
}

static char *read_line(int fd)
{
    char *b = NULL;
    size_t len = 0, cap = 0;
    for (;;) {
        char c;
        ssize_t r = read(fd, &c, 1);
        if (r < 0 && errno == EINTR)
            continue;
        if (r <= 0) {
            free(b);
            return NULL;
        }
        if (c == '\n')
            break;
        if (len + 2 > cap) {
            cap = cap ? cap * 2 : 128;
            b = realloc(b, cap);
        }
        b[len++] = c;
    }
    if (!b)
        return strdup("");
    b[len] = 0;
    return b;
}

int read_command(Shell *sh, char **out)
{
    char *acc = NULL;
    for (;;) {
        char *ln = read_line(sh->in_fd);
        if (!ln) {
            free(acc);
            return -1;
        }
        if (!acc) {
            acc = ln;
        } else {
            size_t a = strlen(acc), b = strlen(ln);
            acc = realloc(acc, a + b + 2);
            acc[a] = '\n';
            memcpy(acc + a + 1, ln, b + 1);
            free(ln);
        }
        if (!needs_more(acc))
            break;
        put(sh->out_fd, CONT_PROMPT, strlen(CONT_PROMPT));
    }
    *out = acc;
    return 0;
}

static void toklist_add(TokList *tl, TokType type, const char *text)
{
    if (tl->n == tl->cap) {
        tl->cap = tl->cap ? tl->cap * 2 : 16;
        tl->v = realloc(tl->v, tl->cap * sizeof(Token));
    }
    tl->v[tl->n].type = type;
    tl->v[tl->n].text = text ? strdup(text) : NULL;
    tl->n++;
}

void toklist_free(TokList *tl)
{
    for (int i = 0; i < tl->n; i++)
        free(tl->v[i].text);
    free(tl->v);
    tl->v = NULL;
    tl->n = tl->cap = 0;
}

static void w_open(Word *w)
{
    if (!w->s) {
        w->cap = 32;
        w->s = calloc(1, w->cap);
    }
    w->started = 1;
}

static void w_add(Word *w, char c)
{
    w_open(w);
    if (w->len + 2 > w->cap) {
        w->cap *= 2;
        w->s = realloc(w->s, w->cap);
    }
    w->s[w->len++] = c;
    w->s[w->len] = 0;
}

static void flush(Shell *sh, Word *w, TokList *tl)
{
    if (!w->started)
        return;
    int done = 0;
    if (w->glob) {
        char pat[PATH_MAX * 2 + 2];
        size_t pre = 0;
        glob_t g;
        if (w->s[0] == '/') {
            snprintf(pat, sizeof pat, "%s", w->s);
        } else {
            snprintf(pat, sizeof pat, "%s/%s", sh->cwd, w->s);
            pre = strlen(sh->cwd) + 1;
        }
        if (glob(pat, 0, NULL, &g) == 0) {
            for (size_t k = 0; k < g.gl_pathc; k++)
                toklist_add(tl, T_WORD, g.gl_pathv[k] + pre);
            globfree(&g);
            done = 1;
        }
    }
    if (!done)
        toklist_add(tl, T_WORD, w->s);
    w->len = 0;
    w->s[0] = 0;
    w->started = 0;
    w->glob = 0;
}

static void add_text(Shell *sh, Word *w, TokList *tl, const char *t, int quoted)
{
    for (; *t; t++) {
        if (!quoted && (*t == ' ' || *t == '\t' || *t == '\n'))
            flush(sh, w, tl);
        else
            w_add(w, *t);
    }
}

char *capture_output(Shell *sh, const char *cmd)
{
    int p[2];
    if (pipe2(p, O_CLOEXEC) < 0)
        return strdup("");
    pid_t pid = fork();
    if (pid < 0) {
        close(p[0]);
        close(p[1]);
        return strdup("");
    }
    if (pid == 0) {
        Shell sub = *sh;
        sub.out_fd = p[1];
        int st = execute_line(&sub, cmd);
        _exit(st);
    }
    close(p[1]);
    char *buf = NULL, tmp[4096];
    size_t len = 0, cap = 0;
    ssize_t r;
    while ((r = read(p[0], tmp, sizeof tmp)) != 0) {
        if (r < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        if (len + r + 1 > cap) {
            cap = (len + r + 1) * 2;
            buf = realloc(buf, cap);
        }
        memcpy(buf + len, tmp, r);
        len += r;
    }
    close(p[0]);
    while (waitpid(pid, NULL, 0) < 0 && errno == EINTR)
        ;
    if (!buf)
        return strdup("");
    buf[len] = 0;
    while (len > 0 && buf[len - 1] == '\n')
        buf[--len] = 0;
    return buf;
}

static int expand_dollar(Shell *sh, const char *s, int *pi, Word *w, TokList *tl, int quoted)
{
    int i = *pi;
    char c = s[i + 1];
    if (c == '(') {
        int j = skip_subst(s, i + 2);
        if (j < 0)
            return -1;
        char *cmd = strndup(s + i + 2, j - i - 2);
        char *out = capture_output(sh, cmd);
        add_text(sh, w, tl, out, quoted);
        free(cmd);
        free(out);
        *pi = j + 1;
    } else if (c == '?') {
        char b[16];
        snprintf(b, sizeof b, "%d", sh->status);
        add_text(sh, w, tl, b, quoted);
        *pi = i + 2;
    } else if (c == '{') {
        const char *e = strchr(s + i + 2, '}');
        if (!e)
            return -1;
        char *name = strndup(s + i + 2, e - (s + i + 2));
        const char *v = getenv(name);
        add_text(sh, w, tl, v ? v : "", quoted);
        free(name);
        *pi = (int)(e - s) + 1;
    } else if (isalpha((unsigned char)c) || c == '_') {
        int j = i + 1;
        while (isalnum((unsigned char)s[j]) || s[j] == '_')
            j++;
        char *name = strndup(s + i + 1, j - i - 1);
        const char *v = getenv(name);
        add_text(sh, w, tl, v ? v : "", quoted);
        free(name);
        *pi = j;
    } else {
        w_add(w, '$');
        *pi = i + 1;
    }
    return 0;
}

static int expand_backtick(Shell *sh, const char *s, int *pi, Word *w, TokList *tl, int quoted)
{
    int j = skip_bt(s, *pi + 1);
    if (j < 0)
        return -1;
    char *raw = strndup(s + *pi + 1, j - *pi - 1);
    char *cmd = malloc(strlen(raw) + 1);
    int k = 0;
    for (const char *p = raw; *p; p++) {
        if (*p == '\\' && (p[1] == '`' || p[1] == '\\' || p[1] == '$'))
            p++;
        cmd[k++] = *p;
    }
    cmd[k] = 0;
    char *out = capture_output(sh, cmd);
    add_text(sh, w, tl, out, quoted);
    free(raw);
    free(cmd);
    free(out);
    *pi = j + 1;
    return 0;
}

static int bad_quote(Shell *sh)
{
    say(sh->err_fd, "terminator: syntax error: unterminated quote or substitution\n");
    return -1;
}

int tokenize(Shell *sh, const char *line, TokList *tl)
{
    Word w = {0};
    int i = 0, n = (int)strlen(line);
    while (i < n) {
        char c = line[i];
        if (c == ' ' || c == '\t' || c == '\n') {
            flush(sh, &w, tl);
            i++;
        } else if (c == '#' && !w.started) {
            while (i < n && line[i] != '\n')
                i++;
        } else if (c == '|') {
            flush(sh, &w, tl);
            toklist_add(tl, T_PIPE, NULL);
            i++;
        } else if (c == '<') {
            flush(sh, &w, tl);
            toklist_add(tl, T_IN, NULL);
            i++;
        } else if (c == '>') {
            flush(sh, &w, tl);
            if (line[i + 1] == '>') {
                toklist_add(tl, T_APPEND, NULL);
                i += 2;
            } else {
                toklist_add(tl, T_OUT, NULL);
                i++;
            }
        } else if (c == '\\') {
            if (i + 1 < n && line[i + 1] != '\n')
                w_add(&w, line[i + 1]);
            i += 2;
        } else if (c == '\'') {
            int j = skip_sq(line, i + 1);
            if (j < 0)
                return bad_quote(sh);
            w_open(&w);
            for (int k = i + 1; k < j; k++)
                w_add(&w, line[k]);
            i = j + 1;
        } else if (c == '"') {
            w_open(&w);
            i++;
            while (i < n && line[i] != '"') {
                if (line[i] == '\\' && i + 1 < n) {
                    char d = line[i + 1];
                    if (d == '$' || d == '"' || d == '\\' || d == '`' || d == '\n') {
                        w_add(&w, d);
                        i += 2;
                    } else {
                        w_add(&w, '\\');
                        i++;
                    }
                } else if (line[i] == '$') {
                    if (expand_dollar(sh, line, &i, &w, tl, 1) < 0)
                        return bad_quote(sh);
                } else if (line[i] == '`') {
                    if (expand_backtick(sh, line, &i, &w, tl, 1) < 0)
                        return bad_quote(sh);
                } else {
                    w_add(&w, line[i++]);
                }
            }
            if (i >= n)
                return bad_quote(sh);
            i++;
        } else if (c == '$') {
            if (expand_dollar(sh, line, &i, &w, tl, 0) < 0)
                return bad_quote(sh);
        } else if (c == '`') {
            if (expand_backtick(sh, line, &i, &w, tl, 0) < 0)
                return bad_quote(sh);
        } else if (c == '~' && !w.started && (i + 1 == n || strchr("/ \t\n|<>", line[i + 1]))) {
            const char *home = getenv("HOME");
            w_open(&w);
            for (const char *p = home ? home : "~"; *p; p++)
                w_add(&w, *p);
            i++;
        } else {
            if (c == '*' || c == '?' || c == '[')
                w.glob = 1;
            w_add(&w, c);
            i++;
        }
    }
    flush(sh, &w, tl);
    free(w.s);
    return 0;
}
