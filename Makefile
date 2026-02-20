CXX      := g++
CXXFLAGS := -std=c++17 -Wall -Wextra -O2 -D_FILE_OFFSET_BITS=64
LDFLAGS  := -lalpm -lssl -lcrypto

SRC_DIR := src
BUILD   := build
TARGET  := pmt

SRCS := $(wildcard $(SRC_DIR)/*.cpp)
OBJS := $(patsubst $(SRC_DIR)/%.cpp,$(BUILD)/%.o,$(SRCS))
DEPS := $(OBJS:.o=.d)

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS)

$(BUILD)/%.o: $(SRC_DIR)/%.cpp | $(BUILD)
	$(CXX) $(CXXFLAGS) -MMD -MP -c -o $@ $<

$(BUILD):
	mkdir -p $(BUILD)

install: $(TARGET)
	install -Dm755 $(TARGET) $(DESTDIR)$(PREFIX)/bin/$(TARGET)

clean:
	rm -rf $(BUILD) $(TARGET)

-include $(DEPS)

PREFIX ?= /usr
.PHONY: all clean install
