// Entry point for Terminator terminal emulator.

#include "tasks/gui.h"

#include <string>

int main(int argc, char **argv) {
  // Check for process isolation mode flag
  bool process_mode = false;

  for (int i = 1; i < argc; i++) {
    if (string(argv[i]) == "useProcess=True") {
      process_mode = true;
    }
  }

  // Launch the main GUI event loop
  return gui_run(process_mode);
}
