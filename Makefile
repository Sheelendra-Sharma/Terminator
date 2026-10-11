CC = gcc
CFLAGS = -Wall -Wextra -O2 -D_GNU_SOURCE -pthread -Itasks
LDLIBS = -lX11
SRC_DIR = tasks
SRC = $(SRC_DIR)/gui_tabs.c \
      $(SRC_DIR)/builtin_commands.c \
      $(SRC_DIR)/multiline_input.c \
      $(SRC_DIR)/io_redirection.c \
      $(SRC_DIR)/pipes.c
OBJ = $(SRC:.c=.o)

terminator: $(OBJ)
	$(CC) $(CFLAGS) -o $@ $(OBJ) $(LDLIBS)

$(SRC_DIR)/%.o: $(SRC_DIR)/%.c $(SRC_DIR)/terminator.h
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f terminator $(OBJ)

.PHONY: clean
