CXX = g++
CXXFLAGS = -std=c++17 -Wall -Wextra -pthread
CPPFLAGS = -I.
LDFLAGS = -pthread
LDLIBS = -lX11
SRCS = main.cpp tasks/common.cpp tasks/task1_gui.cpp tasks/task2_commands.cpp tasks/task3_parser.cpp tasks/task4_redirection.cpp tasks/task5_pipes.cpp
OBJS = $(SRCS:.cpp=.o)
all: terminator
terminator: $(OBJS)
	$(CXX) $(CXXFLAGS) $(LDFLAGS) -o $@ $(OBJS) $(LDLIBS)
%.o: %.cpp
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) -c $< -o $@
clean:
	rm -f $(OBJS) terminator
.PHONY: all clean
