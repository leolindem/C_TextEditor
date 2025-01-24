# Makefile for Kilo editor in C++

# Compiler and flags
CXX       := g++
CXXFLAGS  := -std=c++17 -Wall -Wextra -pedantic

# Targets
SRC       := kilo.cpp
OBJ       := $(SRC:.cpp=.o)
EXEC      := kilo

# Default target
all: $(EXEC)

# Link the object file(s) into the final executable
$(EXEC): $(OBJ)
	$(CXX) $(CXXFLAGS) -o $@ $^

# Compile each .cpp file into a .o
%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

# Remove build artifacts
clean:
	rm -f $(OBJ) $(EXEC)

.PHONY: all clean
