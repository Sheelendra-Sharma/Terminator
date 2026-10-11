#ifndef TERMINATOR_H
#define TERMINATOR_H

#include <limits.h>
#include <semaphore.h>
#include <sys/types.h>

#define PROMPT "user@terminator> "
#define CONT_PROMPT "> "
#define TITLE_MARK '\001'
#define HIST_MAX 10000
#define HIST_SHOW 1000
#define HIST_LEN 512
#define MAXARGS 256

typedef struct {
    sem_t lock;
    long total;
    char cmd[HIST_MAX][HIST_LEN];
} History;

typedef struct {
    int in_fd, out_fd, err_fd;
    char cwd[PATH_MAX];
    int status;
    int done;
} Shell;

typedef enum { T_WORD, T_PIPE, T_IN, T_OUT, T_APPEND } TokType;

typedef struct {
    TokType type;
    char *text;
} Token;

typedef struct {
    Token *v;
    int n, cap;
} TokList;

typedef struct {
    char *argv[MAXARGS];
    int argc;
    char *in_file;
    char *out_file;
    int append;
} Cmd;

extern History *hist;

void put(int fd, const char *s, size_t n);
void say(int fd, const char *fmt, ...);

void history_init(void);
void history_add(const char *line);
void history_print(int fd);

void shell_run(int in_fd, int out_fd);
int execute_line(Shell *sh, const char *line);
int is_builtin(const char *name);
int run_builtin(Shell *sh, int argc, char **argv, int out_fd);

int needs_more(const char *s);
int read_command(Shell *sh, char **out);
int tokenize(Shell *sh, const char *line, TokList *tl);
void toklist_free(TokList *tl);
char *capture_output(Shell *sh, const char *cmd);

void redirect_set(Cmd *c, TokType t, char *file);
int redirect_check(Shell *sh, Cmd *c);
int redirect_open_out(Shell *sh, Cmd *c);
void redirect_apply(Cmd *c);

int run_tokens(Shell *sh, TokList *tl);

#endif
