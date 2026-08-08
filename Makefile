# 1. Variables: Define compiler and optimization flags
CXX = g++
CXXFLAGS = -O2 -Wall -Wextra -std=c++17

# 2. Files list
SRCS = hashtable.cpp avl.cpp zset.cpp server.cpp
OBJS = $(SRCS:.cpp=.o)   # Converts file list to: hashtable.o avl.o zset.o server.o
TARGET = server

# 3. Default Rule: What happens when you type `make`
all: $(TARGET)

# 4. Linking Step: Combines .o files into the final executable binary
$(TARGET): $(OBJS)
	$(CXX) $(CXXFLAGS) -o $@ $(OBJS)

# 5. Compilation Step: Converts individual .cpp files into .o object files
%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

# 6. Clean Rule: Deletes generated binaries to reset the project state (`make clean`)
clean:
	rm -f $(OBJS) $(TARGET)

.PHONY: all clean