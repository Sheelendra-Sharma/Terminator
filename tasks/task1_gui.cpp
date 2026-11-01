#include "task1_gui.h"

#include "common.h"
#include "task5_pipes.h"

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unistd.h>

static Display *dpy = nullptr;
static Window win = 0;
static GC gc = 0;
static XFontStruct *font = nullptr;

static int win_w = 1000, win_h = 650;
static int char_w = 10, line_h = 20, tab_h = 30;

static unsigned long px_text = 0;
static unsigned long px_tabtext_active = 0;
static unsigned long px_tabtext_inactive = 0;
static unsigned long px_tabbar = 0;
static unsigned long px_active = 0;
static unsigned long px_inactive = 0;
static unsigned long px_tabbg = 0;

static vector<Tab> tabs;
static int cur = 0;
static int tab_scroll = 0;
static bool running = true;

static unsigned long alloc_hex(int screen, const char *hex) {
  XColor col;
  if (XParseColor(dpy, DefaultColormap(dpy, screen), hex, &col) &&
      XAllocColor(dpy, DefaultColormap(dpy, screen), &col))
    return col.pixel;
  return WhitePixel(dpy, screen);
}

static string tab_label(int i) {
  return " " + to_string(i + 1) + ": " + short_cwd(tabs[i].cwd) + " ";
}

static int tab_width(const string &label) {
  return (int)label.size() * char_w + 12;
}

static int tab_at_x(int mouse_x) {
  int x = 6 - tab_scroll;
  for (size_t i = 0; i < tabs.size(); i++) {
    int width = tab_width(tab_label((int)i));
    if (mouse_x >= x && mouse_x < x + width) return (int)i;
    x += width + 4;
  }
  return -1;
}

static bool plus_at_x(int mouse_x) {
  int x = 6 - tab_scroll;
  for (size_t i = 0; i < tabs.size(); i++) {
    x += tab_width(tab_label((int)i)) + 4;
  }
  int width = 3 * char_w + 12;
  return mouse_x >= x && mouse_x < x + width;
}

static int tab_strip_width() {
  int width = 6;
  for (size_t i = 0; i < tabs.size(); i++) {
    width += tab_width(tab_label((int)i)) + 4;
  }
  width += (int)strlen(" + ") * char_w + 12;
  return width;
}

static int max_tab_scroll() {
  return max(0, tab_strip_width() - win_w);
}

static void clamp_tab_scroll() {
  tab_scroll = max(0, min(tab_scroll, max_tab_scroll()));
}

static void keep_tab_visible(int index) {
  int left = 6;
  for (int i = 0; i < index; i++) left += tab_width(tab_label(i)) + 4;
  int right = left + tab_width(tab_label(index));
  if (left < tab_scroll) tab_scroll = left;
  else if (right - tab_scroll > win_w) tab_scroll = right - win_w;
  clamp_tab_scroll();
}

static void redraw() {
  if (!dpy) return;

  Tab &tab = tabs[cur];
  clamp_tab_scroll();

  XSetForeground(dpy, gc, px_tabbg);
  XFillRectangle(dpy, win, gc, 0, tab_h, win_w, win_h - tab_h);

  XSetForeground(dpy, gc, px_tabbar);
  XFillRectangle(dpy, win, gc, 0, 0, win_w, tab_h);

  int ascent = font ? font->ascent : line_h - 4;
  int x = 6 - tab_scroll;
  for (size_t i = 0; i < tabs.size(); i++) {
    string label = tab_label(i);
    int w = tab_width(label);

    XSetForeground(dpy, gc, (int)i == cur ? px_active : px_inactive);
    XFillRectangle(dpy, win, gc, x, 4, w, tab_h - 8);
    XSetForeground(dpy, gc, (int)i == cur ? px_tabtext_active : px_tabtext_inactive);
    XDrawString(dpy, win, gc, x + 6, tab_h / 2 + ascent / 2, label.c_str(), label.size());

    x += w + 4;
  }

  const char *plus = " + ";
  int pw = (int)strlen(plus) * char_w + 12;
  XSetForeground(dpy, gc, px_inactive);
  XFillRectangle(dpy, win, gc, x, 4, pw, tab_h - 8);
  XSetForeground(dpy, gc, px_tabtext_inactive);
  XDrawString(dpy, win, gc, x + 6, tab_h / 2 + ascent / 2, plus, strlen(plus));

  int visible = (win_h - tab_h) / line_h;
  if (visible < 1) visible = 1;

  int total = (int)tab.lines.size() + 1;
  int max_scroll = total > visible ? total - visible : 0;
  if (tab.scroll > max_scroll) tab.scroll = max_scroll;
  if (tab.scroll < 0) tab.scroll = 0;

  int start = total - visible - tab.scroll;
  if (start < 0) start = 0;

  int max_chars = win_w / char_w;
  XSetForeground(dpy, gc, px_text);

  for (int row = 0; row < visible; row++) {
    int idx = start + row;
    int y = tab_h + (row + 1) * line_h - 4;

    if (idx < (int)tab.lines.size()) {
      string s = tab.lines[idx].text;
      if ((int)s.size() > max_chars) s = s.substr(0, max_chars);
      XDrawString(dpy, win, gc, 4, y, s.c_str(), s.size());
    } else if (idx == (int)tab.lines.size()) {
      string prompt = tab.searching_history ? "Enter search term: "
                      : tab.multi.empty() ? prompt_for(tab) : "> ";
      string text = prompt + tab.input;
      if ((int)text.size() > max_chars) text = text.substr(0, max_chars);
      XDrawString(dpy, win, gc, 4, y, text.c_str(), text.size());

      if (tab.cursor > (int)tab.input.size()) tab.cursor = (int)tab.input.size();
      if (tab.cursor < 0) tab.cursor = 0;

      int cx = 4 + (int)(prompt.size() + tab.cursor) * char_w;
      if (cx + char_w <= win_w) {
        int fa = font ? font->ascent : line_h - 4;
        int fd = font ? (font->ascent + font->descent) : line_h;
        XFillRectangle(dpy, win, gc, cx, y - fa, char_w, fd);
        if (tab.cursor < (int)tab.input.size()) {
          XSetForeground(dpy, gc, px_tabbg);
          char ch = tab.input[tab.cursor];
          XDrawString(dpy, win, gc, cx, y, &ch, 1);
          XSetForeground(dpy, gc, px_text);
        }
      }
    }
  }

  XFlush(dpy);
}

static void switch_tab(int i) {
  if (i < 0 || i >= (int)tabs.size()) return;
  cur = i;
  keep_tab_visible(cur);
  if (!tabs[cur].cwd.empty()) chdir(tabs[cur].cwd.c_str());
}

static void new_tab() {
  Tab t;
  const char *home = getenv("HOME");
  t.cwd = home ? home : "/";
  tabs.push_back(t);
  switch_tab((int)tabs.size() - 1);
}

static void close_tab() {
  if (tabs.size() <= 1) { running = false; return; }
  tabs.erase(tabs.begin() + cur);
  if (cur >= (int)tabs.size()) cur = tabs.size() - 1;
  if (!tabs[cur].cwd.empty()) chdir(tabs[cur].cwd.c_str());
}

static bool is_ctrl(bool ctrl, KeySym sym, char c) {
  if (!ctrl) return false;
  return sym == (KeySym)c || sym == (KeySym)(c - 32) || sym == (KeySym)(c - 'a' + 1);
}

static void handle_enter() {
  Tab &tab = tabs[cur];

  if (tab.searching_history) {
    string term = tab.input;
    append_line(tab, "Enter search term: " + term, LINE_COMMAND);
    vector<string> matches = search_history(term);
    if (matches.empty()) {
      append_line(tab, "No match for search term in history");
    } else {
      for (const string &match : matches) append_line(tab, match);
    }

    tab.input.clear();
    tab.cursor = 0;
    tab.searching_history = false;
    tab.saved_search_input.clear();
    tab.scroll = 0;
    redraw();
    return;
  }

  string disp = tab.multi.empty() ? prompt_for(tab) : "> ";
  append_line(tab, disp + tab.input, LINE_COMMAND);

  string candidate = tab.multi + tab.input;
  tab.input.clear();
  tab.cursor = 0;
  tab.hist_idx = -1;
  tab.saved_input.clear();

  if (!is_complete(candidate)) {
    if (ends_with_continuation(candidate))
      tab.multi = candidate.substr(0, candidate.size() - 1) + "\n";
    else
      tab.multi = candidate + "\n";
    tab.scroll = 0;
    redraw();
    return;
  }

  tab.multi.clear();
  run_line(tab, candidate);

  bool should_close = tab.exit_requested;
  tab.scroll = 0;

  if (should_close) close_tab();
  else              redraw();
}

int gui_run(bool process_mode) {
  load_history();

  Tab first;
  char buf[4096];
  first.cwd = getcwd(buf, sizeof(buf)) ? buf : "/";
  tabs.push_back(first);

  if (process_mode) {
    append_line(tabs[0], "Running in process-isolation mode: each command runs in its own forked process.");
  }

  dpy = XOpenDisplay(nullptr);
  if (!dpy) {
    fprintf(stderr, "terminator: cannot open X display (set DISPLAY or run under Xvfb)\n");
    return 1;
  }

  int screen = DefaultScreen(dpy);
  win = XCreateSimpleWindow(dpy, RootWindow(dpy, screen),
                            0, 0, win_w, win_h, 0,
                            BlackPixel(dpy, screen), BlackPixel(dpy, screen));

  XStoreName(dpy, win, "Terminator");

  Atom wm_delete = XInternAtom(dpy, "WM_DELETE_WINDOW", False);
  XSetWMProtocols(dpy, win, &wm_delete, 1);
  XSelectInput(dpy, win, ExposureMask | KeyPressMask | ButtonPressMask | StructureNotifyMask);

  gc = XCreateGC(dpy, win, 0, nullptr);

  const char *font_names[] = {
    "10x20", "12x24", "9x18", "9x15bold", "9x15", "fixed", nullptr
  };
  for (int fi = 0; font_names[fi]; fi++) {
    font = XLoadQueryFont(dpy, font_names[fi]);
    if (font) break;
  }
  if (font) {
    XSetFont(dpy, gc, font->fid);
    char_w = font->max_bounds.width;
    line_h = font->ascent + font->descent + 3;
  }

  px_text     = WhitePixel(dpy, screen);
  px_tabtext_active = alloc_hex(screen, "#000000");
  px_tabtext_inactive = alloc_hex(screen, "#FFFFFF");
  px_tabbg    = alloc_hex(screen, "#000000");
  px_tabbar   = alloc_hex(screen, "#000000");
  px_active   = alloc_hex(screen, "#FFFFFF");
  px_inactive = alloc_hex(screen, "#000000");

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
      if ((Atom)ev.xclient.data.l[0] == wm_delete) running = false;

    } else if (ev.type == ButtonPress) {
      Tab &tab = tabs[cur];

      if (ev.xbutton.button == Button4) {
        if (ev.xbutton.y < tab_h) {
          tab_scroll = max(0, tab_scroll - 3 * char_w);
        } else {
          tab.scroll += 3;
        }
        redraw();
        continue;
      }
      if (ev.xbutton.button == Button5) {
        if (ev.xbutton.y < tab_h) {
          tab_scroll += 3 * char_w;
          clamp_tab_scroll();
        } else {
          tab.scroll = max(0, tab.scroll - 3);
        }
        redraw();
        continue;
      }

      if (ev.xbutton.y < tab_h) {
        if (ev.xbutton.button == Button1) {
          int hit = tab_at_x(ev.xbutton.x);
          if (hit >= 0) switch_tab(hit);
          else if (plus_at_x(ev.xbutton.x)) new_tab();
          redraw();
        } else if (ev.xbutton.button == Button2) {
          int hit = tab_at_x(ev.xbutton.x);
          if (hit >= 0) {
            switch_tab(hit);
            close_tab();
            redraw();
          }
        }
        continue;
      }

    } else if (ev.type == KeyPress) {
      Tab &tab = tabs[cur];
      char text[64];
      KeySym keysym = 0;
      int len = XLookupString(&ev.xkey, text, sizeof(text), &keysym, nullptr);
      bool ctrl = ev.xkey.state & ControlMask;
      bool shift = ev.xkey.state & ShiftMask;

      if (keysym == XK_Return || keysym == XK_KP_Enter) {
        handle_enter();
        continue;
      }

      if (shift && keysym == XK_Up) {
        tab.scroll++;
        redraw();
        continue;
      }
      if (shift && keysym == XK_Down) {
        if (tab.scroll > 0) tab.scroll--;
        redraw();
        continue;
      }
      if (keysym == XK_Page_Up) {
        tab.scroll += (win_h - tab_h) / line_h - 1;
        redraw();
        continue;
      }
      if (keysym == XK_Page_Down) {
        tab.scroll = max(0, tab.scroll - (win_h - tab_h) / line_h + 1);
        redraw();
        continue;
      }

      if (ctrl && keysym == XK_Page_Up)   { switch_tab(cur - 1); redraw(); continue; }
      if (ctrl && keysym == XK_Page_Down) { switch_tab(cur + 1); redraw(); continue; }
      if (is_ctrl(ctrl, keysym, 't'))     { new_tab(); redraw(); continue; }
      if (is_ctrl(ctrl, keysym, 'r')) {
        if (!tab.searching_history) tab.saved_search_input = tab.input;
        tab.searching_history = true;
        tab.input.clear();
        tab.cursor = 0;
        tab.scroll = 0;
        redraw();
        continue;
      }
      if (keysym == XK_Escape && tab.searching_history) {
        tab.input = tab.saved_search_input;
        tab.cursor = (int)tab.input.size();
        tab.searching_history = false;
        tab.saved_search_input.clear();
        redraw();
        continue;
      }
      if (is_ctrl(ctrl, keysym, 'q') || is_ctrl(ctrl, keysym, 'w')) {
        close_tab();
        redraw();
        continue;
      }

      if (keysym == XK_Up) {
        lock_guard<mutex> lock(g_history_mutex);
        if (!g_history.empty()) {
          if (tab.hist_idx == -1) {
            tab.saved_input = tab.input;
            tab.hist_idx = (int)g_history.size() - 1;
          } else if (tab.hist_idx > 0) {
            tab.hist_idx--;
          }
          tab.input = g_history[tab.hist_idx];
          tab.cursor = (int)tab.input.size();
          tab.scroll = 0;
          redraw();
        }
        continue;
      }

      if (keysym == XK_Down) {
        if (tab.hist_idx != -1) {
          lock_guard<mutex> lock(g_history_mutex);
          tab.hist_idx++;
          if (tab.hist_idx >= (int)g_history.size()) {
            tab.hist_idx = -1;
            tab.input = tab.saved_input;
          } else {
            tab.input = g_history[tab.hist_idx];
          }
          tab.cursor = (int)tab.input.size();
          tab.scroll = 0;
          redraw();
        }
        continue;
      }

      if (keysym == XK_Left) {
        if (tab.cursor > 0) tab.cursor--;
        redraw();
        continue;
      }
      if (keysym == XK_Right) {
        if (tab.cursor < (int)tab.input.size()) tab.cursor++;
        redraw();
        continue;
      }
      if (keysym == XK_Home || is_ctrl(ctrl, keysym, 'a')) {
        tab.cursor = 0;
        redraw();
        continue;
      }
      if (keysym == XK_End || is_ctrl(ctrl, keysym, 'e')) {
        tab.cursor = (int)tab.input.size();
        redraw();
        continue;
      }

      if (is_ctrl(ctrl, keysym, 'l')) {
        tab.lines.clear();
        tab.pending.clear();
        redraw();
        continue;
      }
      if (keysym == XK_Escape || is_ctrl(ctrl, keysym, 'c')) {
        tab.input.clear();
        tab.multi.clear();
        tab.cursor = 0;
        tab.hist_idx = -1;
        tab.saved_input.clear();
        tab.searching_history = false;
        tab.saved_search_input.clear();
        tab.scroll = 0;
        redraw();
        continue;
      }

      if (keysym == XK_BackSpace) {
        if (tab.cursor > 0 && tab.cursor <= (int)tab.input.size()) {
          tab.input.erase(tab.cursor - 1, 1);
          tab.cursor--;
        }
        redraw();
        continue;
      }
      if (keysym == XK_Delete) {
        if (tab.cursor >= 0 && tab.cursor < (int)tab.input.size()) {
          tab.input.erase(tab.cursor, 1);
        }
        redraw();
        continue;
      }
      if (is_ctrl(ctrl, keysym, 'u')) {
        if (tab.cursor > 0) {
          tab.input.erase(0, tab.cursor);
          tab.cursor = 0;
        }
        redraw();
        continue;
      }
      if (is_ctrl(ctrl, keysym, 'k')) {
        if (tab.cursor < (int)tab.input.size()) {
          tab.input.erase(tab.cursor);
        }
        redraw();
        continue;
      }

      if (keysym == XK_Tab) continue;

      for (int i = 0; i < len; i++) {
        if ((unsigned char)text[i] >= 32 && (unsigned char)text[i] < 127) {
          if (tab.cursor > (int)tab.input.size()) tab.cursor = (int)tab.input.size();
          tab.input.insert(tab.cursor, 1, text[i]);
          tab.cursor++;
        }
      }
      tab.scroll = 0;
      redraw();
    }
  }

  XCloseDisplay(dpy);
  return 0;
}
