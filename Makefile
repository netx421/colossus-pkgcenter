# ---------------------------------------------
# COLOSSUS Package Center Makefile
# ---------------------------------------------

CXX      := g++
CXXFLAGS := -std=c++17 -O2 -Wall -Wextra `pkg-config --cflags gtk+-3.0`
LDFLAGS  := `pkg-config --libs gtk+-3.0`

TARGET   := colossus-pkgcenter
SRC      := colossus_pkgcenter.cpp
OBJ      := $(SRC:.cpp=.o)

.PHONY: all clean run

all: $(TARGET)

$(TARGET): $(OBJ)
	$(CXX) $(CXXFLAGS) $(OBJ) $(LDFLAGS) -o $(TARGET)

%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

run: $(TARGET)
	./$(TARGET)

clean:
	rm -f $(OBJ) $(TARGET)
