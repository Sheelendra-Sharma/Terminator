#pragma once

#include "common.h"

#include <X11/Xlib.h>
#include <X11/Xutil.h>

struct TerminalColors {
  unsigned long text, active_text, inactive_text;
  unsigned long tab_bar, active_tab, inactive_tab, background;
};

class TerminalRenderer {
 public:
  void configure(Display *display, Window window, GC gc, XFontStruct *font,
                 int width, int height, int char_width, int line_height,
                 int tab_height, const TerminalColors &colors);
  void resize(int width, int height);
  void draw(vector<Tab> &tabs, int active_tab, int &tab_scroll);

  int tab_at_x(int mouse_x, const vector<Tab> &tabs, int tab_scroll) const;
  bool plus_at_x(int mouse_x, const vector<Tab> &tabs, int tab_scroll) const;
  void clamp_tab_scroll(const vector<Tab> &tabs, int &tab_scroll) const;
  void keep_tab_visible(int index, const vector<Tab> &tabs, int &tab_scroll) const;

 private:
  Display *display = nullptr;
  Window window = 0;
  GC gc = 0;
  XFontStruct *font = nullptr;
  int width = 1000, height = 650;
  int char_width = 10, line_height = 20, tab_height = 30;
  TerminalColors colors{};

  string tab_label(int index) const;
  int tab_width(const string &label) const;
  int max_tab_scroll(const vector<Tab> &tabs) const;
};
