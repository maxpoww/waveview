PLUGIN_NAME = waveview
OUT = $(PLUGIN_NAME).so
RUST_LIB = rust/target/release/libwaveview_brain.a

CXXFLAGS ?= -O2
CXXFLAGS += -shared -fPIC --no-gnu-unique -std=c++2b -Wall -g -DWLR_USE_UNSTABLE
PKG = pkg-config --cflags pixman-1 libdrm hyprland pangocairo libinput libudev wayland-server xkbcommon

all: $(OUT)

$(RUST_LIB):
	cd rust && cargo build --release --offline

# Golem's titlebars live in src/hyprbars/ (a stripped fork — see the
# PROVENANCE.md there). Built into this .so rather than shipped as a second
# plugin, so there is one thing in ABI lockstep with Hyprland.
SRC = src/main.cpp $(wildcard src/hyprbars/*.cpp)

$(OUT): $(SRC) $(RUST_LIB)
	$(CXX) $(CXXFLAGS) `$(PKG)` $(SRC) $(RUST_LIB) -lpthread -ldl -o $(OUT)

clean:
	rm -f $(OUT); cd rust && cargo clean

.PHONY: all clean
