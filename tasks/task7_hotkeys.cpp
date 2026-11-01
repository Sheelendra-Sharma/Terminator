#include "task7_hotkeys.h"

bool hotkey_left(EditState &e) {
    if (e.cursor > 0) e.cursor--;
    return true;
}

bool hotkey_right(EditState &e) {
    if (e.cursor < (int)e.line.size()) e.cursor++;
    return true;
}

bool hotkey_home(EditState &e) {
    e.cursor = 0;
    return true;
}

bool hotkey_end(EditState &e) {
    e.cursor = (int)e.line.size();
    return true;
}

bool hotkey_up(EditState &e) {
    lock_guard<mutex> lock(g_history_mutex);
    if (g_history.empty()) return true;

    if (e.hist_idx == -1) {
        e.saved_line = e.line;
        e.hist_idx = (int)g_history.size() - 1;
    } else if (e.hist_idx > 0) {
        e.hist_idx--;
    }

    e.line = g_history[e.hist_idx];
    e.cursor = (int)e.line.size();
    return true;
}

bool hotkey_down(EditState &e) {
    if (e.hist_idx == -1) return true;

    lock_guard<mutex> lock(g_history_mutex);
    e.hist_idx++;

    if (e.hist_idx >= (int)g_history.size()) {
        e.hist_idx = -1;
        e.line = e.saved_line;
    } else {
        e.line = g_history[e.hist_idx];
    }

    e.cursor = (int)e.line.size();
    return true;
}

bool hotkey_kill_bol(EditState &e) {
    e.line.erase(0, e.cursor);
    e.cursor = 0;
    return true;
}

bool hotkey_kill_eol(EditState &e) {
    e.line.erase(e.cursor);
    return true;
}

bool hotkey_kill_word(EditState &e) {
    int i = e.cursor;
    while (i > 0 && e.line[i - 1] == ' ') i--;
    while (i > 0 && e.line[i - 1] != ' ') i--;

    e.line.erase(i, e.cursor - i);
    e.cursor = i;
    return true;
}

void hotkey_clear(Tab &tab) {
    tab.lines.clear();
    tab.pending.clear();
    tab.scroll = 0;
}

bool hotkey_insert(EditState &e, char c) {
    e.line.insert(e.cursor, 1, c);
    e.cursor++;
    return true;
}

bool hotkey_backspace(EditState &e) {
    if (e.cursor <= 0) return true;
    e.line.erase(e.cursor - 1, 1);
    e.cursor--;
    return true;
}
