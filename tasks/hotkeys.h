// Input editing helper interface.

#pragma once

#include "common.h"

// Input text, cursor position, and history selection.
struct EditState {
  string line;
  int cursor = 0;
  int hist_idx = -1;
  string saved_line;
};

bool hotkey_left(EditState &e);
bool hotkey_right(EditState &e);
bool hotkey_home(EditState &e);
bool hotkey_end(EditState &e);
bool hotkey_up(EditState &e);
bool hotkey_down(EditState &e);
bool hotkey_kill_bol(EditState &e);
bool hotkey_kill_eol(EditState &e);
bool hotkey_kill_word(EditState &e);
void hotkey_clear(Tab &tab);
bool hotkey_insert(EditState &e, char c);
bool hotkey_backspace(EditState &e);
