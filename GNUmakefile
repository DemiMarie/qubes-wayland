MAKEFLAGS ::= -rR
BUILD_RUST ?= yes
ifeq ($(BUILD_RUST),yes)
override flags ::= '-DBUILD_RUST=_Pragma("GCC error \"should not be referenced\"")'
else ifeq ($(BUILD_RUST),no)
override flags ::= -UBUILD_RUST
else
$(error BUILD_RUST must be “yes” or “no”)
endif

CFLAGS ::= -O2 -g3 \
	-fno-strict-aliasing \
	-fno-strict-overflow \
	-fno-delete-null-pointer-checks \
	-grecord-gcc-switches \
	-fstack-clash-protection \
	-fcf-protection \
	-fPIC \
	-Werror=format \
	-Werror=implicit-function-declaration \
	-Werror=maybe-uninitialized \
	-pedantic-errors \
	-fasynchronous-unwind-tables \
	-fexceptions \
	-Wall -Wextra -Werror \
	$(shell pkg-config --cflags pixman-1 wayland-protocols) \
	-Wp,-DWLR_USE_UNSTABLE \
	-Wp,-D_FORTIFY_SOURCE=2 \
	-Wp,-D_GLIBCXX_ASSERTIONS \
	-Wp,-D_POSIX_C_SOURCE=200112l \
	-Wp,-D_GNU_SOURCE \
	-Wp,-UNDEBUG \
	-Icbits/protocols \
	-fsanitize=address,undefined \
	-pthread $(flags)

CC ::= gcc
override OUTDIR ::= cbits/build/
define newline :=


endef

.PHONY: all clean
.ONESHELL:
#all: protocols/xdg-shell-protocol.h
all: $(OUTDIR)qubes.o qubes-compositor
clean:
	rm -rf $(OUTDIR) && cargo clean

cbits/protocols/%-protocol.h: /usr/share/wayland-protocols/stable/%/%.xml
	@mkdir -p -m 0700 protocols
	@wayland-scanner --include-core-only --strict -- server-header $< $@

qubes-compositor: $(OUTDIR)main.o $(OUTDIR)qubes_output.o $(OUTDIR)qubes_allocator.o $(OUTDIR)qubes_backend.o$(and $(filter-out no,$(BUILD_RUST)), $(PWD)/target/release/libqubes_gui_rust.a)
	@flags=$$(pkg-config --libs wlroots pixman-1 wayland-protocols wayland-server xkbcommon) &&
	case $$flags in (*\*\?\['
		') exit 1;; esac &&
	$(CC) -L/usr/local/lib64 -Ltarget/release $(CFLAGS) -o $@ $^ -Wl,-rpath,/usr/local/lib64 $${flags}$(and $(filter-out no,$(BUILD_RUST)), -lqubes_gui_rust) -lm -lvchan-xen -pthread -ldl

$(OUTDIR)%.o: cbits/%.c cbits/protocols/xdg-shell-protocol.h GNUmakefile
	@mkdir -p -m 0700 $(OUTDIR)
	$(CC) $(CFLAGS) -c -o '$(subst ','\'',$@)' $< -MD -MP -MF '$(subst ','\'',$@).dep'
-include $(OUTDIR)*.dep
-include $(PWD)/target/release/libqubes_gui_rust.d

$(PWD)/target/release/libqubes_gui_rust.a: GNUmakefile Cargo.toml
	cargo build --release && touch -- '$(subst ','\'',$@)'
