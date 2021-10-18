MAKEFLAGS ::= -rR
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
	-Iprotocols

CC ::= gcc
override OUTDIR ::= build/
define newline :=


endef

.PHONY: all clean
.ONESHELL:
#all: protocols/xdg-shell-protocol.h
all: $(OUTDIR)wlroots_bindings.o $(OUTDIR)qubes.o qubes-compositor
clean:
	rm -rf $(OUTDIR)

protocols/%-protocol.h: /usr/share/wayland-protocols/stable/%/%.xml
	@mkdir -p -m 0700 protocols
	@wayland-scanner --include-core-only --strict -- server-header $< $@

qubes-compositor: $(OUTDIR)main.o $(OUTDIR)qubes_output.o $(OUTDIR)qubes_allocator.o
	@flags=$$(pkg-config --libs wlroots pixman-1 wayland-protocols wayland-server xkbcommon) &&
	case $$flags in (*\*\?\['
		') exit 1;; esac &&
	$(CC) -L/usr/local/lib64 $(CFLAGS) -o $@ $^ -Wl,-rpath,/usr/local/lib64 $${flags}

$(OUTDIR)%.o: %.c protocols/xdg-shell-protocol.h
	@mkdir -p -m 0700 $(OUTDIR)
	$(CC) $(CFLAGS) -c -o $@ $< -MD -MP -MF $@.dep
