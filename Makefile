# lagtrace: `make && make install` (per user, no root), `make uninstall`.
# If your Vulkan headers predate VK_EXT_present_timing (Vulkan 1.4.330), run `make deps` first
# (or point VULKAN_INCLUDE at a Vulkan-Headers checkout).
CC       ?= cc
CFLAGS   ?= -O2 -g
PREFIX   ?= $(HOME)/.local
BINDIR   ?= $(PREFIX)/bin
LIBDIR   ?= $(PREFIX)/lib/lagtrace
LAYERDIR ?= $(PREFIX)/share/vulkan/implicit_layer.d
PYDIR    ?= $(LIBDIR)/python
VULKAN_HEADERS_TAG ?= v1.4.354
VULKAN_INCLUDE ?= $(if $(wildcard deps/Vulkan-Headers/include),deps/Vulkan-Headers/include)

INC      = $(if $(VULKAN_INCLUDE),-I$(VULKAN_INCLUDE))
WARN     = -std=gnu11 -Wall -Wextra
LAYER    = build/libVkLayer_lagtrace.so
BENCH    = build/lagtrace-bench
MANIFEST = VkLayer_lagtrace.x86_64.json

# NO_BENCH=1 builds and installs only the layer and CLI (no SDL3 needed; `lagtrace bench` won't work).
BENCH_TARGET = $(if $(filter 1,$(NO_BENCH)),,$(BENCH))

all: $(LAYER) $(BENCH_TARGET) build/$(MANIFEST)

$(LAYER): layer/lagtrace_layer.c | build
	$(CC) $(CFLAGS) $(INC) $(WARN) -shared -fPIC -fvisibility=hidden -pthread -o $@ $<

$(BENCH): bench/lagtrace_bench.c | build
	$(CC) $(CFLAGS) $(INC) $(WARN) $(shell pkg-config --cflags sdl3) -o $@ $< $(shell pkg-config --libs sdl3) -lvulkan

# A manifest that points at build/, so `lagtrace` can run from a checkout without installing.
build/$(MANIFEST): layer/VkLayer_lagtrace.json.in | build
	sed 's#@LIB@#$(abspath $(LAYER))#' $< > $@

build:
	mkdir -p build

deps:
	test -d deps/Vulkan-Headers || git clone --depth 1 --branch $(VULKAN_HEADERS_TAG) https://github.com/KhronosGroup/Vulkan-Headers.git deps/Vulkan-Headers

install: $(LAYER) $(BENCH_TARGET)
	install -Dm755 $(LAYER) $(DESTDIR)$(LIBDIR)/libVkLayer_lagtrace.so
	mkdir -p $(DESTDIR)$(LAYERDIR)
	sed 's#@LIB@#$(LIBDIR)/libVkLayer_lagtrace.so#' layer/VkLayer_lagtrace.json.in > $(DESTDIR)$(LAYERDIR)/$(MANIFEST)
	$(if $(BENCH_TARGET),install -Dm755 $(BENCH) $(DESTDIR)$(BINDIR)/lagtrace-bench)
	rm -rf $(DESTDIR)$(PYDIR)/lagtrace
	mkdir -p $(DESTDIR)$(PYDIR)/lagtrace
	install -m644 lagtrace/*.py $(DESTDIR)$(PYDIR)/lagtrace/
	sed 's#@PYDIR@#$(PYDIR)#' bin/lagtrace.in > $(DESTDIR)$(BINDIR)/lagtrace
	chmod 755 $(DESTDIR)$(BINDIR)/lagtrace
	@id -nG | grep -qw input || echo "note: lagtrace reads click times from /dev/input; add yourself to the input group: sudo usermod -aG input $$USER (then log in again)"

uninstall:
	rm -f $(DESTDIR)$(LIBDIR)/libVkLayer_lagtrace.so $(DESTDIR)$(LAYERDIR)/$(MANIFEST)
	rm -f $(DESTDIR)$(BINDIR)/lagtrace $(DESTDIR)$(BINDIR)/lagtrace-bench
	rm -rf $(DESTDIR)$(PYDIR)

test:
	python3 -m unittest discover -s tests -v

clean:
	rm -rf build

.PHONY: all deps install uninstall test clean
