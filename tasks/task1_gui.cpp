#include "task1_gui.h"
#include "common.h"
#include "task1_render.h"
#include "tab_runtime.h"
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <algorithm>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>

static Display *dpy = nullptr;
static Window win = 0;
static GC gc = 0;
static XFontStruct *font = nullptr;

static int win_w = 1000, win_h = 650;
static int char_w = 10, line_h = 20, tab_h = 30;

static vector<Tab> tabs;
static int cur = 0;
static int tab_scroll = 0;
static bool running = true;
static vector<XEvent> deferred_events;
static bool process_mode = false;
static TerminalRenderer renderer;
static Atom clipboard_atom, clipboard_property, clipboard_utf8;
static string copied_text;
static bool paste_pending = false, paste_fallback = false;

static void copy_text(const string &text) {
  copied_text = text;
  XSetSelectionOwner(dpy, clipboard_atom, win, CurrentTime);
  XSetSelectionOwner(dpy, XA_PRIMARY, win, CurrentTime);
  XFlush(dpy);
}

static void paste_text() {
  paste_pending = true;
  paste_fallback = false;
  XConvertSelection(dpy, clipboard_atom, clipboard_utf8, clipboard_property, win, CurrentTime);
  XFlush(dpy);
}

static void serve_selection(const XSelectionRequestEvent &request) {
  XSelectionEvent reply{};
  reply.type = SelectionNotify;
  reply.display = dpy;
  reply.requestor = request.requestor;
  reply.selection = request.selection;
  reply.target = request.target;
  reply.time = request.time;
  reply.property = request.property == None ? request.target : request.property;
  if (request.target == XA_STRING || request.target == clipboard_utf8)
    XChangeProperty(dpy, request.requestor, reply.property, request.target, 8,
                    PropModeReplace, (const unsigned char *)copied_text.data(), copied_text.size());
  else if (request.target == XInternAtom(dpy, "TARGETS", False)) {
    Atom targets[] = {XInternAtom(dpy, "TARGETS", False), clipboard_utf8, XA_STRING};
    XChangeProperty(dpy, request.requestor, reply.property, XA_ATOM, 32,
                    PropModeReplace, (unsigned char *)targets, 3);
  } else reply.property = None;
  XSendEvent(dpy, request.requestor, False, 0, (XEvent *)&reply);
}

static bool receive_paste(const XSelectionEvent &event, Tab &tab) {
  if (!paste_pending || event.selection != clipboard_atom) return false;
  if (event.property == None) {
    if (!paste_fallback) {
      paste_fallback = true;
      XConvertSelection(dpy, clipboard_atom, XA_STRING, clipboard_property, win, CurrentTime);
      XFlush(dpy);
    } else paste_pending = false;
    return false;
  }
  paste_pending = false;
  Atom type;
  int format;
  unsigned long count, remaining;
  unsigned char *data = nullptr;
  if (XGetWindowProperty(dpy, win, clipboard_property, 0, 4096, True, AnyPropertyType,
                         &type, &format, &count, &remaining, &data) != Success || !data || format != 8)
    return false;
  tab.cursor = max(0, min(tab.cursor, (int)tab.input.size()));
  for (unsigned long i = 0; i < count; i++)
    if (data[i] >= 32 && data[i] < 127) tab.input.insert(tab.cursor++, 1, (char)data[i]);
  XFree(data);
  return true;
}

static unsigned long alloc_hex(int screen, const char *hex) {
  XColor col;
  if (XParseColor(dpy, DefaultColormap(dpy, screen), hex, &col) &&
      XAllocColor(dpy, DefaultColormap(dpy, screen), &col))
    return col.pixel;
  return WhitePixel(dpy, screen);
}

static void redraw() { renderer.draw(tabs, cur, tab_scroll); }

static void switch_tab(int i) {
  if (i < 0 || i >= (int)tabs.size()) return;
  cur = i;
  renderer.keep_tab_visible(cur, tabs, tab_scroll);
}

static void new_tab() {
  Tab t;
  const char *home = getenv("HOME");
  t.cwd = home ? home : "/";
  tabs.push_back(t);
  tab_runtime_add(t);
  switch_tab((int)tabs.size() - 1);
}

static void close_tab() {
  if (tabs.size() <= 1) { running = false; return; }
  tab_runtime_remove(cur);
  tabs.erase(tabs.begin() + cur);
  if (cur >= (int)tabs.size()) cur = tabs.size() - 1;
}

static bool is_ctrl(bool ctrl, KeySym sym, char c) {
  if (!ctrl) return false;
  return sym == (KeySym)c || sym == (KeySym)(c - 32) || sym == (KeySym)(c - 'a' + 1);
}

static bool next_event(XEvent &event) {
  if (!deferred_events.empty()) {
    event = deferred_events.front();
    deferred_events.erase(deferred_events.begin());
    return true;
  }
  for (;;) {
    if (tab_runtime_collect(tabs)) redraw();
    if (tabs[cur].exit_requested) return true;
    if (XPending(dpy)) {
      XNextEvent(dpy, &event);
      return true;
    }
    vector<pollfd> fds;
    fds.push_back({ConnectionNumber(dpy), POLLIN, 0});
    for (int fd : tab_runtime_process_fds()) fds.push_back({fd, POLLIN, 0});
    poll(fds.data(), fds.size(), 100);
  }
}

int gui_event_fd() {
  int worker_fd = tab_worker_event_fd();
  return worker_fd >= 0 ? worker_fd : (dpy ? ConnectionNumber(dpy) : -1);
}

int gui_poll_job_control() {
  int signal = 0;
  if (tab_worker_poll_control(signal)) return signal;
  if (!dpy) return 0;

  while (XPending(dpy)) {
    XEvent event;
    if (!XCheckTypedWindowEvent(dpy, win, KeyPress, &event)) break;
    KeySym key = XLookupKeysym(&event.xkey, 0);
    bool ctrl = event.xkey.state & ControlMask;
    bool shift = event.xkey.state & ShiftMask;
    if (ctrl && !shift && (key == XK_c || key == XK_C)) return SIGINT;
    if (ctrl && !shift && (key == XK_z || key == XK_Z)) return SIGTSTP;
    deferred_events.push_back(event);
  }
  return 0;
}

void gui_close_worker_fds_in_child() {
  dpy = nullptr;
  tab_worker_close_fds_in_child();
}

static void handle_enter() {
  Tab &tab = tabs[cur];
  if (tab_runtime_busy(cur)) {
    append_line(tab, "terminator: wait for this tab's command to finish");
    redraw();
    return;
  }

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
  bool clear_command = candidate == "clear";
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
  if (clear_command) {
    tab.lines.clear();
    tab.pending.clear();
  }
  tab.scroll = 0;
  if (process_mode) add_history(candidate);
  tab_runtime_submit(cur, candidate);
  redraw();
}

int gui_run(bool process_mode) {
  ::process_mode = process_mode;
  load_history();

  Tab first;
  char buf[4096];
  first.cwd = getcwd(buf, sizeof(buf)) ? buf : "/";
  tabs.push_back(first);

  dpy = XOpenDisplay(nullptr);
  if (!dpy) {
    fprintf(stderr, "terminator: cannot open X display (set DISPLAY or run under Xvfb)\n");
    cleanup_shared_history();
    return 1;
  }

  int screen = DefaultScreen(dpy);
  int x_fd = ConnectionNumber(dpy);
  fcntl(x_fd, F_SETFD, fcntl(x_fd, F_GETFD) | FD_CLOEXEC);
  tab_runtime_init(process_mode, x_fd);
  win = XCreateSimpleWindow(dpy, RootWindow(dpy, screen),
                            0, 0, win_w, win_h, 0,
                            BlackPixel(dpy, screen), BlackPixel(dpy, screen));

  XStoreName(dpy, win, process_mode ? "Terminator - Process Mode"
                                   : "Terminator - Thread Mode");
  clipboard_atom = XInternAtom(dpy, "CLIPBOARD", False);
  clipboard_property = XInternAtom(dpy, "TERMINATOR_CLIPBOARD", False);
  clipboard_utf8 = XInternAtom(dpy, "UTF8_STRING", False);

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

  XSizeHints size_hints{};
  size_hints.flags = PMinSize;
  string minimum_prompt = prompt_for(tabs[0]);
  size_hints.min_width = max(240, (int)minimum_prompt.size() * char_w + 16);
  size_hints.min_height = tab_h + line_h * 2;
  XSetWMNormalHints(dpy, win, &size_hints);

  TerminalColors colors{
      WhitePixel(dpy, screen), alloc_hex(screen, "#000000"),
      alloc_hex(screen, "#FFFFFF"), alloc_hex(screen, "#000000"),
      alloc_hex(screen, "#FFFFFF"), alloc_hex(screen, "#000000"),
      alloc_hex(screen, "#000000")};

  renderer.configure(dpy, win, gc, font, win_w, win_h, char_w, line_h, tab_h,
                     colors);

  XMapWindow(dpy, win);
  tab_runtime_add(tabs[0]);

  XEvent ev;
  while (running) {
    next_event(ev);
    if (tabs[cur].exit_requested) {
      close_tab();
      if (running) redraw();
      continue;
    }

    if (ev.type == Expose) {
      redraw();
    } else if (ev.type == ConfigureNotify) {
      win_w = ev.xconfigure.width;
      win_h = ev.xconfigure.height;
      renderer.resize(win_w, win_h);
      redraw();
    } else if (ev.type == ClientMessage) {
      if ((Atom)ev.xclient.data.l[0] == wm_delete) running = false;
    } else if (ev.type == SelectionRequest) {
      serve_selection(ev.xselectionrequest);
    } else if (ev.type == SelectionNotify) {
      if (receive_paste(ev.xselection, tabs[cur])) redraw();

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
          renderer.clamp_tab_scroll(tabs, tab_scroll);
        } else {
          tab.scroll = max(0, tab.scroll - 3);
        }
        redraw();
        continue;
      }

      if (ev.xbutton.y < tab_h) {
        if (ev.xbutton.button == Button1) {
          int hit = renderer.tab_at_x(ev.xbutton.x, tabs, tab_scroll);
          if (hit >= 0) switch_tab(hit);
          else if (renderer.plus_at_x(ev.xbutton.x, tabs, tab_scroll)) new_tab();
          redraw();
        } else if (ev.xbutton.button == Button2) {
          int hit = renderer.tab_at_x(ev.xbutton.x, tabs, tab_scroll);
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

      if (ctrl && shift && (keysym == XK_c || keysym == XK_C)) {
        copy_text(tab.input);
        continue;
      }
      if (ctrl && shift && (keysym == XK_v || keysym == XK_V)) {
        paste_text();
        continue;
      }

      if (ctrl && !shift && (keysym == XK_c || keysym == XK_C ||
                             keysym == XK_z || keysym == XK_Z) && tab_runtime_busy(cur)) {
        int signal = (keysym == XK_c || keysym == XK_C) ? SIGINT : SIGTSTP;
        tab_runtime_signal(cur, signal);
        continue;
      }

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
      if (is_ctrl(ctrl, keysym, 'q')) {
        close_tab();
        redraw();
        continue;
      }

      if (keysym == XK_Up) {
        load_history();
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
          load_history();
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
      if (is_ctrl(ctrl, keysym, 'w')) {
        int start = tab.cursor;
        while (start > 0 && tab.input[start - 1] == ' ') start--;
        while (start > 0 && tab.input[start - 1] != ' ') start--;
        tab.input.erase(start, tab.cursor - start);
        tab.cursor = start;
        redraw();
        continue;
      }
      if (is_ctrl(ctrl, keysym, 'd')) {
        if (tab.input.empty()) close_tab();
        else if (tab.cursor < (int)tab.input.size()) tab.input.erase(tab.cursor, 1);
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

  tab_runtime_shutdown();
  XCloseDisplay(dpy);
  cleanup_shared_history();
  return 0;
}
