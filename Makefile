CXX = g++
CXXFLAGS = -std=c++17 -Wall -Wextra -pthread
CPPFLAGS = -I.
LDFLAGS = -pthread
LDLIBS = -lX11
SRCS = main.cpp tasks/common.cpp tasks/gui.cpp tasks/render.cpp tasks/tab_runtime.cpp tasks/commands.cpp tasks/parser.cpp tasks/redirection.cpp tasks/pipes.cpp tasks/multiwatch.cpp tasks/hotkeys.cpp tasks/jobs.cpp
OBJS = $(SRCS:.cpp=.o)
all: terminator
terminator: $(OBJS)
	$(CXX) $(CXXFLAGS) $(LDFLAGS) -o $@ $(OBJS) $(LDLIBS)
%.o: %.cpp
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) -c $< -o $@
clean:
	rm -f $(OBJS) terminator
.PHONY: all clean
