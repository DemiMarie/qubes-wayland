project(
  'qubes-compositor',
  'c',
  version: '0.0.1',
  license: 'GPLv2+',
  default_options: [
    'c_std=c11',
    'warning_level=2',
    'werror=true',
  ],
)

add_project_arguments(
  [
    '-DWLR_USE_UNSTABLE',
    '-D_FORTIFY_SOURCE=2',
    '-Wno-unused-parameter',
    '-Wno-error=cpp',
    '-Wvla',
  ],
  language: 'c',
)

wlroots_proj = subproject(
  'wlroots',
  default_options: ['examples=false', 'session=disabled', 'backends=[]'],
  required: true,
  version: ['>=0.15.0', '<0.16.0'],
)
bin_compositor = executable(
  meson.project_name(),
  [
    'cbits/qubes_allocator.c',
    'cbits/qubes_backend.c',
    'cbits/qubes_output.c',
    'cbits/main.c',
  ],
  dependencies: [wlroots_proj.get_variable('wlroots')],
  include_directories: ['cbits', 'cbits/protocols'],
  install: true,
)
