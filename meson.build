project(
  'qubes-compositor',
  'c',
  version: '0.0.1',
  license: 'GPLv2+',
  default_options: [
    'c_std=c11',
    'buildtype=debugoptimized',
    'warning_level=2',
    'werror=true',
  ],
)

add_project_arguments(
  [
    '-DWLR_USE_UNSTABLE',
    '-D_FORTIFY_SOURCE=2',
    '-D_GNU_SOURCE=1',
    '-Wno-unused-parameter',
    '-Wno-error=cpp',
    # '-Wno-error=stringop-overflow',
    '-Wvla',
  ],
  language: 'c',
)
qubes_files = [
  'cbits/qubes_allocator.c',
  'cbits/qubes_backend.c',
  'cbits/qubes_output.c',
  'cbits/qubes_input.c',
  'cbits/qubes_clipboard.c',
  'cbits/qubes_xwayland.c',
  'cbits/qubes_data_source.c',
  'cbits/qubes_wayland.c',
  'cbits/qubes_window_position.c',
  'cbits/main.c',
]

cc = meson.get_compiler('c')
dl = cc.find_library('dl', required: false)
qubesdb = cc.find_library('qubesdb')

# Taken from wlroots, which is:
# Copyright (c) 2017, 2018 Drew DeVault
# Copyright (c) 2014 Jari Vetoniemi
#
# Permission is hereby granted, free of charge, to any person obtaining a copy of
# this software and associated documentation files (the "Software"), to deal in
# the Software without restriction, including without limitation the rights to
# use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies
# of the Software, and to permit persons to whom the Software is furnished to do
# so, subject to the following conditions:
#
# The above copyright notice and this permission notice shall be included in all
# copies or substantial portions of the Software.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
# IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
# FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
# AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
# LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
# OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
# SOFTWARE.

wayland_protos = dependency('wayland-protocols',
	version: '>=1.23',
	fallback: ['wayland-protocols', 'wayland_protocols'],
	default_options: ['tests=false'],
)
wl_protocol_dir = wayland_protos.get_variable('pkgdatadir')

wayland_scanner_dep = dependency('wayland-scanner', native: true)
wayland_scanner = find_program(
	wayland_scanner_dep.get_variable('wayland_scanner'),
	native: true,
)

protocols = {
	# Stable upstream protocols
	'xdg-shell': wl_protocol_dir / 'stable/xdg-shell/xdg-shell.xml',
}

protocols_code = {}
protocols_server_header = {}
foreach name, path : protocols
	code = custom_target(
		name.underscorify() + '_c',
		input: path,
		output: '@BASENAME@-protocol.c',
		command: [wayland_scanner, 'private-code', '@INPUT@', '@OUTPUT@'],
	)
	qubes_files += code

	server_header = custom_target(
		name.underscorify() + '_server_h',
		input: path,
		output: '@BASENAME@-protocol.h',
		command: [wayland_scanner, 'server-header', '@INPUT@', '@OUTPUT@'],
	)
	qubes_files += server_header
	protocols_code += { name: code }
	protocols_server_header += { name: server_header }
endforeach

xcb = dependency('xcb')
threads = dependency('threads')
vchan_xen = dependency('vchan-xen')
drm = dependency('libdrm')
wayland_server = dependency('wayland-server')
xkbcommon = dependency('xkbcommon')
pixman = dependency('pixman-1')
systemd = dependency('libsystemd', required: false)
pam = dependency('pam')

wlroots_options = [
  'examples=false',
  'xwayland=enabled',
  'xcb-errors=disabled',
  'renderers=[]',
  'backends=[]',
  'allocators=[]',
  'auto_features=disabled',
  'werror=false',
]
wlroots = dependency('wlroots',
                     version: ['>=0.17.1', '<0.18.0'],
                     default_options: wlroots_options,
                     required: get_option('use-system-wlroots'),
                     fallback: ['wlroots', 'wlroot'])

rust_parts = custom_target(
  'Build Rust code',
  depfile: 'libqubes_gui_rust.d',
  command: ['./build_rust.sh', '@DEPFILE@', '@OUTDIR@', '@PRIVATE_DIR@', '@SOURCE_ROOT@', 'release'],
  output: ['libqubes_gui_rust.a'],
  build_by_default: true,
)
conf_data = configuration_data()
conf_data.set('QUBES_HAS_SYSTEMD', systemd.found(), description: 'Is systemd found?')
conf_data.set('PREFIX', get_option('prefix'))
conf_h = configure_file(
  output: 'config.h',
  configuration: conf_data,
)
out_file = configure_file(
  input: 'qubes-gui-agent-wayland.service.in',
  output: 'qubes-gui-agent-wayland.service',
  configuration: conf_data,
)


bin_compositor = executable(
  meson.project_name(),
  qubes_files,
  dependencies: [wlroots, threads, vchan_xen, dl, systemd, qubesdb, drm, wayland_server, xkbcommon, pixman, xcb],
  include_directories: ['cbits'],
  link_with: [rust_parts[0]],
  install: true,
  gnu_symbol_visibility: 'hidden',
)

install_data(sources: '30_qubes-gui-agent-wayland.preset', install_dir: 'lib/systemd/system-preset')
install_data(sources: out_file, install_dir: 'lib/systemd/system')
install_data(sources: 'qubes-wayland-session', install_dir: 'bin', install_mode: 'rwxr-xr-x')
