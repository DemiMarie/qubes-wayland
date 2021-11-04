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
    '-DBUILD_RUST',
    '-Wno-unused-parameter',
    '-Wno-error=cpp',
    '-Wvla',
  ],
  language: 'c',
)
qubes_files = [
  'cbits/qubes_allocator.c',
  'cbits/qubes_backend.c',
  'cbits/qubes_output.c',
  'cbits/qubes_clipboard.c',
  'cbits/qubes_data_source.c',
  'cbits/main.c',
]

cc = meson.get_compiler('c')
dl = cc.find_library('dl', required: false)

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

wlroots_proj = subproject(
  'wlroots',
  default_options: ['examples=false', 'session=disabled', 'backends=[]'],
  required: true,
  version: ['>=0.15.0', '<0.16.0'],
)

rust_parts = custom_target(
  'Build Rust code',
  depfile: 'libqubes_gui_rust.d',
  command: ['./build_rust.sh', '@DEPFILE@', '@OUTDIR@', '@PRIVATE_DIR@', 'release'],
  output: ['libqubes_gui_rust.a'],
  build_by_default: true,
)

threads = dependency('threads')
vchan_xen = dependency('vchan-xen')

bin_compositor = executable(
  meson.project_name(),
  qubes_files,
  dependencies: [wlroots_proj.get_variable('wlroots'), threads, vchan_xen, dl],
  include_directories: ['cbits', include_directories('.')],
  link_with: [rust_parts[0]],
  install: true,
)
