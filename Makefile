PLUGIN_NAME = waveview
OUT = $(PLUGIN_NAME).so
RUST_LIB = rust/target/release/libwaveview_brain.a

CXXFLAGS ?= -O2
CXXFLAGS += -shared -fPIC --no-gnu-unique -std=c++2b -Wall -g -DWLR_USE_UNSTABLE
PKG = pkg-config --cflags pixman-1 libdrm hyprland pangocairo libinput libudev wayland-server xkbcommon

all: $(OUT)

$(RUST_LIB):
	cd rust && cargo build --release --offline

$(OUT): src/main.cpp $(RUST_LIB)
	$(CXX) $(CXXFLAGS) `$(PKG)` src/main.cpp $(RUST_LIB) -lpthread -ldl -o $(OUT)

clean:
	rm -f $(OUT); cd rust && cargo clean

.PHONY: all clean
