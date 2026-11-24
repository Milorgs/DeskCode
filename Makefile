CXX ?= c++
CXXFLAGS ?= -O2 -g
WARN = -std=c++20 -Wall -Wextra -Wpedantic -Werror
ifeq ($(shell uname -s),Darwin)
BREW_PREFIX := $(shell brew --prefix 2>/dev/null)
ifneq ($(BREW_PREFIX),)
PKG_ENV = PKG_CONFIG_PATH="$(BREW_PREFIX)/opt/readline/lib/pkgconfig:$(BREW_PREFIX)/opt/openssl@3/lib/pkgconfig:$(BREW_PREFIX)/opt/json-c/lib/pkgconfig:$(BREW_PREFIX)/lib/pkgconfig$(if $(PKG_CONFIG_PATH),:$(PKG_CONFIG_PATH),)"
endif
else
PTYLIB = -lutil
endif
PKGS = libcurl json-c openssl readline
ATSPI := $(shell $(PKG_ENV) pkg-config --exists atspi-2 gobject-2.0 && echo atspi-2 gobject-2.0)
PKG_CFLAGS := $(patsubst -I%,-isystem %,$(shell $(PKG_ENV) pkg-config --cflags $(PKGS) $(ATSPI)))
PKG_LIBS := $(shell $(PKG_ENV) pkg-config --libs $(PKGS) $(ATSPI))
CPPFLAGS += $(PKG_CFLAGS) -Isrc
LDLIBS += $(PKG_LIBS)
ifneq ($(ATSPI),)
CPPFLAGS += -DWITH_ATSPI
endif
CORE = src/history.cpp src/mcp.cpp src/common.cpp src/platform.cpp src/ui.cpp src/tasks_ui.cpp src/util.cpp src/config.cpp src/api.cpp src/files.cpp src/workspace.cpp src/tools_ext.cpp src/process.cpp src/desktop.cpp src/agent.cpp src/tasks.cpp src/i18n.cpp src/lsp.cpp src/plugins.cpp src/acp.cpp src/agents.cpp
CORE_OBJ = $(CORE:%.cpp=build/obj/%.o)
APP_OBJ = build/obj/src/main.o
TEST_OBJ = build/obj/tests/test.o
DEPFILES = $(CORE_OBJ:.o=.d) $(APP_OBJ:.o=.d) $(TEST_OBJ:.o=.d)
.DELETE_ON_ERROR:
.PHONY: all clean test test-mcp-http sanitize install
all: build/desk
build:
	mkdir -p build
build/obj/%.o: %.cpp
	mkdir -p $(dir $@)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(WARN) -MMD -MP -c $< -o $@
build/desk: $(CORE_OBJ) $(APP_OBJ) | build
	$(CXX) $(CXXFLAGS) $(WARN) $^ $(LDFLAGS) $(LDLIBS) $(PTYLIB) -o $@
build/test: $(CORE_OBJ) $(TEST_OBJ) | build
	$(CXX) $(CXXFLAGS) $(WARN) $^ $(LDFLAGS) $(LDLIBS) $(PTYLIB) -o $@
test: build/desk build/test
	DESK_NO_ARROW=1 ./build/test < /dev/null
test-mcp-http: build/desk
	python3 tests/mcp_http.py < /dev/null
sanitize:
	$(MAKE) clean
	$(MAKE) test CXXFLAGS='-O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer' LDFLAGS='-fsanitize=address,undefined'
install: build/desk
	./install.sh
clean:
	rm -rf build

-include $(DEPFILES)
