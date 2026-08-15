CXX = g++
CXXFLAGS = -Wall -Wextra -O2 -std=c++17 -Isrc
LDFLAGS = -lutil

TARGET = lite-tty-ime
SRC = src/vterm/virtual_shell.cpp src/util.cpp src/status_bar.cpp \
      src/pinyin_data.cpp src/pinyin_file_io.cpp src/pinyin_split.cpp src/pinyin_matcher.cpp \
      src/pinyin_composition.cpp src/word_matcher.cpp src/candidate_item.cpp \
      src/pinyin_virtual_cursor.cpp src/ime_controller.cpp

# 头文件依赖：任何 .h 变化都触发重新编译（header-only 的 ansi_parser/screen 等都在头里）
HDR = $(wildcard src/*.h src/vterm/*.h)

$(TARGET): $(SRC) $(HDR)
	$(CXX) $(CXXFLAGS) $(SRC) -o $(TARGET) $(LDFLAGS)

clean:
	rm -f $(TARGET)

.PHONY: clean
