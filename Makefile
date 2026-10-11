CC = gcc
CFLAGS = -Wall -Wextra -O2 -D_GNU_SOURCE -pthread
LDLIBS = -lX11
SRC = gui_tabs.c builtin_commands.c multiline_input.c io_redirection.c pipes.c
OBJ = $(SRC:.c=.o)

terminator: $(OBJ)
	$(CC) $(CFLAGS) -o $@ $(OBJ) $(LDLIBS)

%.o: %.c terminator.h
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f terminator $(OBJ)

.PHONY: clean
