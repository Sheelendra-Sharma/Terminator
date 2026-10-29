#include "tasks/task1_gui.h"
#include <string>

int main(int argc, char **argv) {
  bool process_mode = false;

  for (int i = 1; i < argc; i++) {
    if (string(argv[i]) == "useProcess=True") {
      process_mode = true;
    }
  }

  return gui_run(process_mode);
}
