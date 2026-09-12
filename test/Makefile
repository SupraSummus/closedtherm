# Host-side tests. Run with `make -C test test` (or ../test.sh).
# Needs a C++17 compiler and, on first run, network access to fetch the pinned headers.

CXX      ?= g++
CXXFLAGS ?= -std=c++17 -Wall -Wextra -O0 -g
# override, so `make CXXFLAGS=-O2` still finds the headers
override CXXFLAGS += -Ifakes -I. -I..

DOCTEST_VERSION := 2.4.11
DOCTEST_SHA256  := 44faa038e9c3f9728efbda143748d01124ea0a27f4bf78f35a15d8fab2e039fb
DOCTEST_URL     := https://raw.githubusercontent.com/doctest/doctest/v$(DOCTEST_VERSION)/doctest/doctest.h

ARDUINOJSON_VERSION := 7.4.2
ARDUINOJSON_SHA256  := a05ac98f4481d2398c103ca5ffcce8e2fcd7fa2fa1d8d9f38d5757454f448d97
ARDUINOJSON_URL     := https://github.com/bblanchon/ArduinoJson/releases/download/v$(ARDUINOJSON_VERSION)/ArduinoJson-v$(ARDUINOJSON_VERSION).h

BUILD := build
SRCS  := main.cpp $(wildcard test_*.cpp)
DEPS  := doctest.h ArduinoJson.h ../ardu.ino ../pi.h ../pi_source.h ../thermometer.h $(wildcard fakes/*.h)
BIN   := $(BUILD)/run_tests

.PHONY: all test clean distclean

all: test

test: $(BIN)
	./$(BIN)

$(BIN): $(SRCS) $(DEPS) | $(BUILD)
	$(CXX) $(CXXFLAGS) $(SRCS) -o $@

# fetch <target> <url> <sha256>
define fetch
	curl -fsSL -o $(1).tmp $(2)
	echo "$(3)  $(1).tmp" | sha256sum -c --quiet
	mv $(1).tmp $(1)
endef

doctest.h:
	$(call fetch,$@,$(DOCTEST_URL),$(DOCTEST_SHA256))

ArduinoJson.h:
	$(call fetch,$@,$(ARDUINOJSON_URL),$(ARDUINOJSON_SHA256))

$(BUILD):
	mkdir -p $@

clean:
	rm -rf $(BUILD)

distclean: clean
	rm -f doctest.h ArduinoJson.h *.tmp
