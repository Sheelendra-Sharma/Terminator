#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/wait.h>
#include <unistd.h>
#include "terminator.h"

#define MAXTABS 16
#define MAXLINES 5000
#define INPUT_MAX 1024

typedef struct {
    char *s;
    int len, cap;
} Line;

typedef struct {
    int in_fd, out_fd;
    pid_t pid;
    Line *lines;
    int nlines;
    char title[256];
    char tbuf[256];
    int tlen, in_title;
    char input[INPUT_MAX];
    int ilen, icur, scroll;
} Tab;

static Display *dpy;
static int scr;
static Window win;
static GC gc;
static XFontStruct *font;
static Pixmap pix;
static Atom wm_delete;
static int cw, ch, asc, barh;
static int winw = 900, winh = 600;
static unsigned long col_bg, col_fg, col_bar, col_sel;

static Tab *tabs[MAXTABS];
static int ntabs, cur;
static int use_process, dirty = 1, quit;
static int tab_x0[MAXTABS + 1], tab_x1[MAXTABS + 1];

static void *thread_worker(void *arg)
{
    int *fds = (int *)arg;
    int in = fds[0], out = fds[1];
    free(fds);
    shell_run(in, out);
    return NULL;
}

static void line_append(Line *l, char c)
{
    if (l->len + 2 > l->cap) {
        l->cap = l->cap ? l->cap * 2 : 64;
        l->s = realloc(l->s, l->cap);
    }
    l->s[l->len++] = c;
    l->s[l->len] = '\0';
}

static void new_line(Tab *t)
{
    if (t->nlines == MAXLINES) {
        free(t->lines[0].s);
        memmove(t->lines, t->lines + 1, (MAXLINES - 1) * sizeof(Line));
        t->nlines--;
    }
    Line *l = &t->lines[t->nlines++];
    l->len = 0;
    l->cap = 64;
    l->s = calloc(1, 64);
    if (t->scroll > 0 && t->nlines > 1)
        t->scroll++;
}

static void tab_putc(Tab *t, unsigned char c)
{
    if (t->in_title) {
        if (c == '\n') {
            snprintf(t->title, sizeof(t->title), "%s", t->tbuf);
            t->in_title = 0;
        } else if (t->tlen < 255) {
            t->tbuf[t->tlen++] = c;
        }
        return;
    }
    if (c == TITLE_MARK) {
        t->in_title = 1;
        t->tlen = 0;
        return;
    }
    if (c == '\n') {
        new_line(t);
        return;
    }
    Line *l = &t->lines[t->nlines - 1];
    if (c == '\t') {
        do { line_append(l, ' '); } while (l->len % 8 != 0);
        return;
    }
    if (c >= 32 && c < 127)
        line_append(l, c);
}

static int tab_open(void)
{
    if (ntabs >= MAXTABS)
        return -1;
    int ip[2], op[2];
    if (pipe(ip) < 0 || pipe(op) < 0)
        return -1;

    Tab *t = calloc(1, sizeof(Tab));
    t->lines = calloc(MAXLINES, sizeof(Line));
    new_line(t);

    if (use_process) {
        pid_t p = fork();
        if (p < 0) {
            close(ip[0]); close(ip[1]); close(op[0]); close(op[1]);
            free(t->lines); free(t);
            return -1;
        }
        if (p == 0) {
            close(ConnectionNumber(dpy));
            for (int i = 0; i < ntabs; i++) {
                close(tabs[i]->in_fd);
                close(tabs[i]->out_fd);
            }
            close(ip[1]);
            close(op[0]);
            setpgid(0, 0);
            shell_run(ip[0], op[1]);
            _exit(0);
        }
        setpgid(p, p);
        close(ip[0]);
        close(op[1]);
        t->pid = p;
    } else {
        int *fds = malloc(2 * sizeof(int));
        fds[0] = ip[0];
        fds[1] = op[1];
        pthread_t th;
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
        pthread_create(&th, &attr, thread_worker, fds);
        pthread_attr_destroy(&attr);
    }

    t->in_fd = ip[1];
    t->out_fd = op[0];
    tabs[ntabs] = t;
    cur = ntabs++;
    dirty = 1;
    return 0;
}

static void tab_close(int i)
{
    Tab *t = tabs[i];
    close(t->in_fd);
    close(t->out_fd);
    if (t->pid > 0) {
        kill(-t->pid, SIGKILL);
        kill(t->pid, SIGKILL);
        waitpid(t->pid, NULL, 0);
    }
    for (int k = 0; k < t->nlines; k++)
        free(t->lines[k].s);
    free(t->lines);
    free(t);

    memmove(&tabs[i], &tabs[i + 1], (ntabs - i - 1) * sizeof(Tab *));
    ntabs--;
    if (cur >= ntabs)
        cur = ntabs - 1;
    if (cur < 0)
        cur = 0;
    dirty = 1;
}

static unsigned long alloc_col(const char *hex)
{
    XColor col;
    Colormap cm = DefaultColormap(dpy, scr);
    if (XParseColor(dpy, cm, hex, &col) && XAllocColor(dpy, cm, &col))
        return col.pixel;
    return BlackPixel(dpy, scr);
}

static int get_rows(void) { int r = (winh - barh) / ch; return r < 1 ? 1 : r; }
static int get_cols(void) { int c = winw / cw; return c < 1 ? 1 : c; }

static void draw_bar(void)
{
    XSetForeground(dpy, gc, col_bar);
    XFillRectangle(dpy, pix, gc, 0, 0, winw, barh);
    int x = 4;
    for (int i = 0; i <= ntabs; i++) {
        char label[300];
        if (i < ntabs)
            snprintf(label, sizeof(label), "[ %d: %.20s ]", i + 1, tabs[i]->title);
        else
            strcpy(label, "[ + ]");
        int len = strlen(label);
        int w = XTextWidth(font, label, len);
        tab_x0[i] = x;
        tab_x1[i] = x + w;
        if (i == cur && i < ntabs) {
            XSetForeground(dpy, gc, col_sel);
            XFillRectangle(dpy, pix, gc, x, 2, w, barh - 4);
        }
        XSetForeground(dpy, gc, col_fg);
        XDrawString(dpy, pix, gc, x, 3 + asc, label, len);
        x += w + cw;
    }
}

static void draw_text(void)
{
    if (ntabs == 0)
        return;
    Tab *t = tabs[cur];
    int cols = get_cols(), rows = get_rows();
    int n = t->nlines;
    int lastlen = t->lines[n - 1].len;
    int full_len = lastlen + t->ilen;

    char *last = malloc(full_len + 1);
    memcpy(last, t->lines[n - 1].s, lastlen);
    memcpy(last + lastlen, t->input, t->ilen);
    last[full_len] = '\0';

    if (t->scroll > n - 1) t->scroll = n - 1;
    if (t->scroll < 0) t->scroll = 0;

    int end = n - t->scroll;
    int row = rows;
    for (int i = end - 1; i >= 0 && row > 0; i--) {
        const char *s = (i == n - 1) ? last : t->lines[i].s;
        int len = (i == n - 1) ? full_len : t->lines[i].len;
        int nr = (i == n - 1) ? (len + cols) / cols : (len == 0 ? 1 : (len + cols - 1) / cols);
        int start = row - nr;

        XSetForeground(dpy, gc, col_fg);
        for (int k = 0; k < nr; k++) {
            int r = start + k;
            int off = k * cols;
            int cnt = len - off;
            if (cnt > cols) cnt = cols;
            if (r >= 0 && r < rows && cnt > 0)
                XDrawString(dpy, pix, gc, 0, barh + r * ch + asc, s + off, cnt);
        }

        /* Draw cursor block on active line */
        if (i == n - 1) {
            int idx = lastlen + t->icur;
            int cr = start + idx / cols;
            int cc = idx % cols;
            if (cr >= 0 && cr < rows) {
                XSetForeground(dpy, gc, col_fg);
                XFillRectangle(dpy, pix, gc, cc * cw, barh + cr * ch, cw, ch);
                if (idx < full_len) {
                    XSetForeground(dpy, gc, col_bg);
                    XDrawString(dpy, pix, gc, cc * cw, barh + cr * ch + asc, &last[idx], 1);
                }
            }
        }
        row = start;
    }
    free(last);
}

static void redraw(void)
{
    XSetForeground(dpy, gc, col_bg);
    XFillRectangle(dpy, pix, gc, 0, 0, winw, winh);
    draw_bar();
    draw_text();
    XCopyArea(dpy, pix, win, gc, 0, 0, winw, winh, 0, 0);
    XFlush(dpy);
}

static void send_line(Tab *t)
{
    Line *l = &t->lines[t->nlines - 1];
    for (int i = 0; i < t->ilen; i++)
        line_append(l, t->input[i]);
    new_line(t);
    put(t->in_fd, t->input, t->ilen);
    put(t->in_fd, "\n", 1);
    t->ilen = t->icur = 0;
    t->scroll = 0;
}

static void handle_key(XKeyEvent *e)
{
    if (ntabs == 0) return;
    Tab *t = tabs[cur];
    char buf[32];
    KeySym ks;
    int n = XLookupString(e, buf, sizeof(buf), &ks, NULL);
    dirty = 1;

    if (e->state & ControlMask) {
        if (ks == XK_t || ks == XK_T) { tab_open(); return; }
        if (ks == XK_q || ks == XK_Q) { tab_close(cur); return; }
        if (ks == XK_Page_Up) { cur = (cur + ntabs - 1) % ntabs; return; }
        if (ks == XK_Page_Down || ks == XK_Tab) { cur = (cur + 1) % ntabs; return; }
    }
    if ((e->state & Mod1Mask) && ks >= XK_1 && ks <= XK_9) {
        int idx = ks - XK_1;
        if (idx < ntabs) cur = idx;
        return;
    }

    switch (ks) {
    case XK_Return:
    case XK_KP_Enter:
        send_line(t);
        return;
    case XK_BackSpace:
        if (t->icur > 0) {
            memmove(t->input + t->icur - 1, t->input + t->icur, t->ilen - t->icur);
            t->icur--; t->ilen--;
        }
        return;
    case XK_Delete:
        if (t->icur < t->ilen) {
            memmove(t->input + t->icur, t->input + t->icur + 1, t->ilen - t->icur - 1);
            t->ilen--;
        }
        return;
    case XK_Left:
        if (t->icur > 0) t->icur--;
        return;
    case XK_Right:
        if (t->icur < t->ilen) t->icur++;
        return;
    case XK_Home: t->icur = 0; return;
    case XK_End: t->icur = t->ilen; return;
    case XK_Page_Up: t->scroll += get_rows() / 2 + 1; return;
    case XK_Page_Down:
        t->scroll -= get_rows() / 2 + 1;
        if (t->scroll < 0) t->scroll = 0;
        return;
    }

    if (n > 0 && buf[0] >= 32 && buf[0] < 127 && t->ilen < INPUT_MAX - 1) {
        memmove(t->input + t->icur + 1, t->input + t->icur, t->ilen - t->icur);
        t->input[t->icur++] = buf[0];
        t->ilen++;
        t->scroll = 0;
    }
}

static void handle_button(XButtonEvent *e)
{
    if (ntabs == 0) return;
    dirty = 1;
    if (e->button == Button4) {
        tabs[cur]->scroll += 3;
    } else if (e->button == Button5) {
        tabs[cur]->scroll -= 3;
        if (tabs[cur]->scroll < 0) tabs[cur]->scroll = 0;
    } else if (e->y < barh) {
        for (int i = 0; i <= ntabs; i++) {
            if (e->x >= tab_x0[i] && e->x < tab_x1[i]) {
                if (i == ntabs) tab_open();
                else if (e->button == Button1) cur = i;
                else if (e->button == Button2) tab_close(i);
                break;
            }
        }
    }
}

int main(int argc, char **argv)
{
    for (int i = 1; i < argc; i++)
        if (strcmp(argv[i], "useProcess=True") == 0)
            use_process = 1;

    signal(SIGPIPE, SIG_IGN);
    history_init();

    dpy = XOpenDisplay(NULL);
    if (!dpy) {
        fprintf(stderr, "terminator: cannot open display\n");
        return 1;
    }
    scr = DefaultScreen(dpy);
    font = XLoadQueryFont(dpy, "9x15");
    if (!font) font = XLoadQueryFont(dpy, "fixed");
    if (!font) {
        fprintf(stderr, "terminator: cannot load font\n");
        return 1;
    }

    cw = font->max_bounds.width;
    asc = font->ascent;
    ch = asc + font->descent;
    barh = ch + 6;

    col_bg  = alloc_col("#101418");
    col_fg  = alloc_col("#d8dee9");
    col_bar = alloc_col("#2a2f3a");
    col_sel = alloc_col("#4c566a");

    win = XCreateSimpleWindow(dpy, RootWindow(dpy, scr), 0, 0, winw, winh, 0, col_fg, col_bg);
    XStoreName(dpy, win, "Terminator");
    wm_delete = XInternAtom(dpy, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(dpy, win, &wm_delete, 1);
    XSelectInput(dpy, win, KeyPressMask | ButtonPressMask | ExposureMask | StructureNotifyMask);

    gc = XCreateGC(dpy, win, 0, NULL);
    XSetFont(dpy, gc, font->fid);
    pix = XCreatePixmap(dpy, win, winw, winh, DefaultDepth(dpy, scr));
    XMapWindow(dpy, win);

    if (tab_open() < 0) {
        fprintf(stderr, "terminator: cannot start tab\n");
        return 1;
    }

    int xfd = ConnectionNumber(dpy);
    while (!quit && ntabs > 0) {
        while (XPending(dpy)) {
            XEvent e;
            XNextEvent(dpy, &e);
            if (e.type == Expose) dirty = 1;
            else if (e.type == ConfigureNotify) {
                if (e.xconfigure.width != winw || e.xconfigure.height != winh) {
                    winw = e.xconfigure.width;
                    winh = e.xconfigure.height;
                    XFreePixmap(dpy, pix);
                    pix = XCreatePixmap(dpy, win, winw, winh, DefaultDepth(dpy, scr));
                }
                dirty = 1;
            } else if (e.type == KeyPress) handle_key(&e.xkey);
            else if (e.type == ButtonPress) handle_button(&e.xbutton);
            else if (e.type == ClientMessage && (Atom)e.xclient.data.l[0] == wm_delete) quit = 1;
        }

        if (quit || ntabs == 0) break;
        if (dirty) { redraw(); dirty = 0; }

        fd_set rs;
        FD_ZERO(&rs);
        FD_SET(xfd, &rs);
        int mx = xfd;
        for (int i = 0; i < ntabs; i++) {
            FD_SET(tabs[i]->out_fd, &rs);
            if (tabs[i]->out_fd > mx) mx = tabs[i]->out_fd;
        }

        if (select(mx + 1, &rs, NULL, NULL, NULL) < 0) {
            if (errno == EINTR) continue;
            break;
        }

        for (int i = ntabs - 1; i >= 0; i--) {
            Tab *t = tabs[i];
            if (!FD_ISSET(t->out_fd, &rs)) continue;
            char buf[4096];
            ssize_t r = read(t->out_fd, buf, sizeof(buf));
            if (r > 0) {
                for (ssize_t k = 0; k < r; k++) tab_putc(t, buf[k]);
                dirty = 1;
            } else if (r == 0 || (errno != EINTR && errno != EAGAIN)) {
                tab_close(i);
            }
        }
    }

    while (ntabs > 0) tab_close(ntabs - 1);
    XCloseDisplay(dpy);
    return 0;
}
