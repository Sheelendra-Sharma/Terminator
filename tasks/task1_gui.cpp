#include "task1_gui.h"

#include "common.h"
#include "task5_pipes.h"

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>

#include <cstdio>
#include <cstdlib>
#include <unistd.h>

static Display *dpy = nullptr;
static Window win = 0;
static GC gc = 0;
static XFontStruct *font = nullptr;

static int win_w = 900, win_h = 600, char_w = 8, line_h = 16, tab_h = 24;
static unsigned long output_px = 0, command_px = 0;

static vector<Tab> tabs;
static int cur = 0;
static bool running = true;

string tab_label(int i) {
  return "[" + to_string(i + 1) + ": " + short_cwd(tabs[i].cwd) + "]";
}

int tab_width(const string &label) {
  return (int)label.size() * char_w + 16;
}

void redraw() {
  if (!dpy) {
    return;
  }

  Tab &tab = tabs[cur];

  XClearWindow(dpy, win);
  XSetForeground(dpy, gc, output_px);

  int x = 0;
  for (size_t i = 0; i < tabs.size(); i++) {
    string label = tab_label(i);
    int w = tab_width(label);

    if ((int)i == cur) {
      XDrawRectangle(dpy, win, gc, x, 2, w - 2, tab_h - 4);
    }

    XDrawString(dpy, win, gc, x + 8, 17, label.c_str(), label.size());
    x += w;
  }

  string plus = "[ + ]";
  XDrawString(dpy, win, gc, x + 8, 17, plus.c_str(), plus.size());

  int visible = (win_h - tab_h) / line_h;
  if (visible < 1) {
    visible = 1;
  }

  int total = (int)tab.lines.size() + 1;

  if (tab.scroll > total - visible) {
    tab.scroll = total - visible;
  }
  if (tab.scroll < 0) {
    tab.scroll = 0;
  }

  int start = total - visible - tab.scroll;
  if (start < 0) {
    start = 0;
  }

  int max_chars = win_w / char_w;

  for (int row = 0; row < visible; row++) {
    int idx = start + row;
    int y = tab_h + (row + 1) * line_h - 4;

    if (idx < (int)tab.lines.size()) {
      Line &line = tab.lines[idx];
      string s = line.text;

      if ((int)s.size() > max_chars) {
        s = s.substr(0, max_chars);
      }

      if (line.kind == LINE_COMMAND) {
        XSetForeground(dpy, gc, command_px);
      } else {
        XSetForeground(dpy, gc, output_px);
      }

      XDrawString(dpy, win, gc, 4, y, s.c_str(), s.size());
    } else if (idx == (int)tab.lines.size()) {
      string prompt = tab.multi.empty() ? prompt_for(tab) : "> ";
      string text = prompt + tab.input;

      if ((int)text.size() > max_chars) {
        text = text.substr(0, max_chars);
      }

      XSetForeground(dpy, gc, command_px);
      XDrawString(dpy, win, gc, 4, y, text.c_str(), text.size());
      XSetForeground(dpy, gc, output_px);

      int cx = 4 + (int)(prompt.size() + tab.input.size()) * char_w;
      if (cx < win_w) {
        XDrawLine(dpy, win, gc, cx, y - line_h + 4, cx, y + 2);
      }
    }
  }

  XFlush(dpy);
}

void switch_tab(int i) {
  if (i < 0 || i >= (int)tabs.size()) {
    return;
  }

  cur = i;

  if (!tabs[cur].cwd.empty()) {
    chdir(tabs[cur].cwd.c_str());
  }
}

void new_tab() {
  Tab t;
  const char *home = getenv("HOME");
  t.cwd = home ? home : "/";

  tabs.push_back(t);
  switch_tab((int)tabs.size() - 1);
}

void close_tab() {
  if (tabs.size() <= 1) {
    running = false;
    return;
  }

  tabs.erase(tabs.begin() + cur);

  if (cur >= (int)tabs.size()) {
    cur = tabs.size() - 1;
  }

  if (!tabs[cur].cwd.empty()) {
    chdir(tabs[cur].cwd.c_str());
  }
}

bool ctrl_key(bool ctrl, KeySym keysym, int len, char *text, char key, int code) {
  return ctrl && (keysym == (KeySym)key ||
                  keysym == (KeySym)(key - 32) ||
                  (len == 1 && text[0] == code));
}

void handle_enter() {
  Tab &tab = tabs[cur];
  string disp = tab.multi.empty() ? prompt_for(tab) : "> ";

  append_line(tab, disp + tab.input, LINE_COMMAND);

  string candidate = tab.multi + tab.input;
  tab.input.clear();

  if (!is_complete(candidate)) {
    if (ends_with_continuation(candidate)) {
      tab.multi = candidate.substr(0, candidate.size() - 1) + "\n";
    } else {
      tab.multi = candidate + "\n";
    }

    tab.scroll = 0;
    redraw();
    return;
  }

  tab.multi.clear();
  run_line(tab, candidate);

  bool should_close = tab.exit_requested;
  tab.scroll = 0;

  if (should_close) {
    close_tab();
  } else {
    redraw();
  }
}

int gui_run(bool process_mode) {
  load_history();

  Tab first;
  char buf[4096];
  first.cwd = getcwd(buf, sizeof(buf)) ? buf : "/";
  tabs.push_back(first);

  if (process_mode) {
    append_line(tabs[0], "Process mode (useProcess=True) is not implemented yet; running in thread mode.");
  }

  dpy = XOpenDisplay(nullptr);
  if (!dpy) {
    fprintf(stderr, "terminator: cannot open X display (set DISPLAY or run under Xvfb)\n");
    return 1;
  }

  int screen = DefaultScreen(dpy);
  win = XCreateSimpleWindow(dpy, RootWindow(dpy, screen), 0, 0, win_w, win_h, 1,
                            BlackPixel(dpy, screen), BlackPixel(dpy, screen));

  XStoreName(dpy, win, "Terminator");

  Atom wm_delete = XInternAtom(dpy, "WM_DELETE_WINDOW", False);
  XSetWMProtocols(dpy, win, &wm_delete, 1);
  XSelectInput(dpy, win, ExposureMask | KeyPressMask | ButtonPressMask | StructureNotifyMask);

  gc = XCreateGC(dpy, win, 0, nullptr);

  font = XLoadQueryFont(dpy, "9x15");
  if (!font) {
    font = XLoadQueryFont(dpy, "fixed");
  }

  if (font) {
    XSetFont(dpy, gc, font->fid);
    char_w = font->max_bounds.width;
    line_h = font->ascent + font->descent + 2;
  }

  XSetForeground(dpy, gc, WhitePixel(dpy, screen));
  XSetBackground(dpy, gc, BlackPixel(dpy, screen));

  output_px = WhitePixel(dpy, screen);
  command_px = WhitePixel(dpy, screen);

  XColor exact, screen_def;
  if (XAllocNamedColor(dpy, DefaultColormap(dpy, screen), "lime", &screen_def, &exact)) {
    command_px = screen_def.pixel;
  }

  XMapWindow(dpy, win);

  XEvent ev;
  while (running) {
    XNextEvent(dpy, &ev);

    if (ev.type == Expose) {
      redraw();
    } else if (ev.type == ConfigureNotify) {
      win_w = ev.xconfigure.width;
      win_h = ev.xconfigure.height;
      redraw();
    } else if (ev.type == ClientMessage) {
      if ((Atom)ev.xclient.data.l[0] == wm_delete) {
        running = false;
      }
    } else if (ev.type == ButtonPress) {
      Tab &tab = tabs[cur];

      if (ev.xbutton.y < tab_h) {
        int x = 0, hit = -1;

        for (size_t i = 0; i < tabs.size(); i++) {
          int w = tab_width(tab_label(i));
          if (ev.xbutton.x >= x && ev.xbutton.x < x + w) {
            hit = i;
          }
          x += w;
        }

        if (hit >= 0) {
          switch_tab(hit);
        } else if (ev.xbutton.x >= x) {
          new_tab();
        }

        redraw();
      } else if (ev.xbutton.button == 4) {
        tab.scroll++;
        redraw();
      } else if (ev.xbutton.button == 5) {
        if (tab.scroll > 0) {
          tab.scroll--;
        }
        redraw();
      }
    } else if (ev.type == KeyPress) {
      Tab &tab = tabs[cur];

      char text[64];
      KeySym keysym = 0;
      int len = XLookupString(&ev.xkey, text, sizeof(text), &keysym, nullptr);
      bool ctrl = ev.xkey.state & ControlMask;

      if (keysym == XK_Return || keysym == XK_KP_Enter) {
        handle_enter();
        continue;
      }

      if (ctrl && keysym == XK_Page_Up) {
        switch_tab(cur - 1);
        redraw();
        continue;
      }

      if (ctrl && keysym == XK_Page_Down) {
        switch_tab(cur + 1);
        redraw();
        continue;
      }

      if (keysym == XK_Page_Up) {
        tab.scroll += (win_h - tab_h) / line_h - 1;
        redraw();
        continue;
      }

      if (keysym == XK_Page_Down) {
        if (tab.scroll > 0) {
          tab.scroll -= (win_h - tab_h) / line_h - 1;
        }
        redraw();
        continue;
      }

      if (ctrl_key(ctrl, keysym, len, text, 't', 20)) {
        new_tab();
        redraw();
        continue;
      }

      if (ctrl_key(ctrl, keysym, len, text, 'w', 23)) {
        close_tab();
        redraw();
        continue;
      }

      if (ctrl_key(ctrl, keysym, len, text, 'l', 12)) {
        tab.lines.clear();
        tab.pending.clear();
        redraw();
        continue;
      }

      if (ctrl_key(ctrl, keysym, len, text, 'c', 3)) {
        tab.input.clear();
        tab.multi.clear();
        redraw();
        continue;
      }

      if (keysym == XK_BackSpace) {
        if (!tab.input.empty()) {
          tab.input.pop_back();
        }
        redraw();
        continue;
      }

      if (keysym == XK_Escape) {
        tab.input.clear();
        tab.multi.clear();
        redraw();
        continue;
      }

      if (keysym == XK_Tab || keysym == XK_Left || keysym == XK_Right ||
          keysym == XK_Up || keysym == XK_Down) {
        continue;
      }

      for (int i = 0; i < len; i++) {
        if (text[i] >= 32 && text[i] < 127) {
          tab.input += text[i];
        }
      }

      tab.scroll = 0;
      redraw();
    }
  }

  XCloseDisplay(dpy);
  return 0;
}
