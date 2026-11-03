// Draw tabs, terminal text, and the input cursor.

#include "render.h"

#include <algorithm>
#include <cstring>

void TerminalRenderer::configure(Display *display, Window window, GC gc,
                                 XFontStruct *font, int width, int height,
                                 int char_width, int line_height, int tab_height,
                                 const TerminalColors &colors) {
  this->display = display;
  this->window = window;
  this->gc = gc;
  this->font = font;
  this->width = width;
  this->height = height;
  this->char_width = char_width;
  this->line_height = line_height;
  this->tab_height = tab_height;
  this->colors = colors;
}

void TerminalRenderer::resize(int width, int height) {
  this->width = width;
  this->height = height;
}

string TerminalRenderer::tab_label(int index) const {
  return " Tab " + to_string(index + 1) + " ";
}

int TerminalRenderer::tab_width(const string &label) const {
  return (int)label.size() * char_width + 12;
}

// Convert a click position into a tab index.
int TerminalRenderer::tab_at_x(int mouse_x, const vector<Tab> &tabs,
                               int tab_scroll) const {
  int x = 6 - tab_scroll;
  for (size_t i = 0; i < tabs.size(); ++i) {
    int width = tab_width(tab_label((int)i));
    if (mouse_x >= x && mouse_x < x + width)
      return (int)i;
    x += width + 4;
  }
  return -1;
}

bool TerminalRenderer::plus_at_x(int mouse_x, const vector<Tab> &tabs,
                                 int tab_scroll) const {
  int x = 6 - tab_scroll;
  for (size_t i = 0; i < tabs.size(); ++i)
    x += tab_width(tab_label((int)i)) + 4;
  int plus_width = 3 * char_width + 12;
  return mouse_x >= x && mouse_x < x + plus_width;
}

int TerminalRenderer::max_tab_scroll(const vector<Tab> &tabs) const {
  int strip_width = 6 + 3 * char_width + 12;
  for (size_t i = 0; i < tabs.size(); ++i)
    strip_width += tab_width(tab_label((int)i)) + 4;
  return max(0, strip_width - width);
}

void TerminalRenderer::clamp_tab_scroll(const vector<Tab> &tabs,
                                        int &tab_scroll) const {
  tab_scroll = max(0, min(tab_scroll, max_tab_scroll(tabs)));
}

void TerminalRenderer::keep_tab_visible(int index, const vector<Tab> &tabs,
                                        int &tab_scroll) const {
  if (index < 0 || index >= (int)tabs.size())
    return;

  int left = 6;
  for (int i = 0; i < index; ++i)
    left += tab_width(tab_label(i)) + 4;

  int right = left + tab_width(tab_label(index));
  if (left < tab_scroll)
    tab_scroll = left;
  else if (right - tab_scroll > width)
    tab_scroll = right - width;
  clamp_tab_scroll(tabs, tab_scroll);
}

// Draw tabs, wrapped output, the prompt, and the cursor.
void TerminalRenderer::draw(vector<Tab> &tabs, int active_tab,
                            int &tab_scroll) {
  if (!display || tabs.empty())
    return;

  clamp_tab_scroll(tabs, tab_scroll);
  Tab &tab = tabs[active_tab];

  XSetForeground(display, gc, colors.background);
  XFillRectangle(display, window, gc, 0, tab_height, width, height - tab_height);
  XSetForeground(display, gc, colors.tab_bar);
  XFillRectangle(display, window, gc, 0, 0, width, tab_height);

  int ascent = font ? font->ascent : line_height - 4;
  int x = 6 - tab_scroll;
  for (size_t i = 0; i < tabs.size(); ++i) {
    string label = tab_label((int)i);
    int width = tab_width(label);
    bool active = (int)i == active_tab;

    XSetForeground(display, gc, active ? colors.active_tab : colors.inactive_tab);
    XFillRectangle(display, window, gc, x, 4, width, tab_height - 8);
    XSetForeground(display, gc, active ? colors.active_text : colors.inactive_text);
    XDrawString(display, window, gc, x + 6, tab_height / 2 + ascent / 2,
                label.c_str(), label.size());
    x += width + 4;
  }

  const char *plus = " + ";
  int plus_width = 3 * char_width + 12;
  XSetForeground(display, gc, colors.inactive_tab);
  XFillRectangle(display, window, gc, x, 4, plus_width, tab_height - 8);
  XSetForeground(display, gc, colors.inactive_text);
  XDrawString(display, window, gc, x + 6, tab_height / 2 + ascent / 2,
              plus, strlen(plus));

  int visible_rows = max(1, (height - tab_height) / line_height);
  int max_chars = max(1, (width - 8) / char_width);
  vector<string> rows;

  for (const Line &line : tab.lines) {
    if (line.text.empty())
      rows.push_back("");

    for (size_t start = 0; start < line.text.size();) {
      size_t end = min(line.text.size(), start + (size_t)max_chars);
      if (end < line.text.size()) {
        size_t space = line.text.find_last_of(" \t", end - 1);
        if (space != string::npos && space > start)
          end = space;
      }
      rows.push_back(line.text.substr(start, end - start));
      start = end;
      while (start < line.text.size() &&
             (line.text[start] == ' ' || line.text[start] == '\t'))
        ++start;
    }
  }

  int prompt_row = (int)rows.size();
  string prompt = tab.searching_history ? "Enter search term: "
                  : tab.multi.empty()   ? prompt_for(tab)
                                        : "> ";
  tab.cursor = max(0, min(tab.cursor, (int)tab.input.size()));

  int prompt_width = (int)prompt.size();
  int first_input_chars = max(0, max_chars - prompt_width);
  int cursor_row = 0;
  int cursor_col = prompt_width + tab.cursor;
  string first_row = prompt + tab.input.substr(0, first_input_chars);
  rows.push_back(first_row.substr(0, max_chars));

  for (int pos = first_input_chars; pos < (int)tab.input.size(); pos += max_chars)
    rows.push_back(tab.input.substr(pos, max_chars));

  if (tab.cursor == first_input_chars && first_input_chars > 0) {
    cursor_row = 1;
    cursor_col = 0;
  }
  else if (tab.cursor > first_input_chars) {
    int after_first = tab.cursor - first_input_chars;
    cursor_row = 1 + (after_first - 1) / max_chars;
    cursor_col = (after_first - 1) % max_chars + 1;
  }

  int total_rows = (int)rows.size();
  int max_scroll = max(0, total_rows - visible_rows);
  tab.scroll = max(0, min(tab.scroll, max_scroll));
  int first_visible = max(0, total_rows - visible_rows - tab.scroll);
  XSetForeground(display, gc, colors.text);

  for (int row = 0; row < visible_rows; ++row) {
    int index = first_visible + row;
    if (index >= total_rows)
      continue;

    int y = tab_height + (row + 1) * line_height - 4;
    XDrawString(display, window, gc, 4, y, rows[index].c_str(), rows[index].size());
    if (index < prompt_row || index - prompt_row != cursor_row)
      continue;

    int cursor_x = 4 + cursor_col * char_width;
    if (cursor_x + char_width > width)
      continue;

    int font_ascent = font ? font->ascent : line_height - 4;
    int font_height = font ? font->ascent + font->descent : line_height;
    XFillRectangle(display, window, gc, cursor_x, y - font_ascent,
                   char_width, font_height);
    if (tab.cursor < (int)tab.input.size()) {
      XSetForeground(display, gc, colors.background);
      char ch = tab.input[tab.cursor];
      XDrawString(display, window, gc, cursor_x, y, &ch, 1);
      XSetForeground(display, gc, colors.text);
    }
  }

  XFlush(display);
}
