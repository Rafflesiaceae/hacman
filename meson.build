project('hacman', 'c',
  version: '0.1.0',
  license: 'MIT',
  meson_version: '>=0.60.0',
  default_options: [
    'c_std=c99',
    'warning_level=3',
    'default_library=static',
    'b_ndebug=if-release',
  ],
)

cc = meson.get_compiler('c')

# hacman is a POSIX/XSI program: it needs fork/execvp, clock/time, mkstemp and
# realpath's allocation form for canonical input paths.
add_project_arguments(
  '-D_POSIX_C_SOURCE=200809L',
  '-D_XOPEN_SOURCE=700',
  language: 'c',
)

# Startup cost is a feature here (see README.md), so the binary is trimmed
# aggressively: unused sections are dropped and no build-id note is emitted.
size_cflags = cc.get_supported_arguments(
  '-ffunction-sections',
  '-fdata-sections',
  '-fno-asynchronous-unwind-tables',
  '-fno-unwind-tables',
)
size_ldflags = cc.get_supported_link_arguments(
  '-Wl,--gc-sections',
  '-Wl,--build-id=none',
  '-Wl,-z,norelro',
)
add_project_arguments(size_cflags, language: 'c')
add_project_link_arguments(size_ldflags, language: 'c')

link_args = []
if get_option('static')
  # A fully static binary has no dynamic loader to run before main(), which is
  # the single largest startup win. build.sh selects a musl toolchain for this.
  if cc.has_link_argument('-static')
    link_args += '-static'
  else
    warning('compiler does not support -static; building a dynamic binary')
  endif
endif

# Keep third-party implementation files in separate static libraries so the
# project's warning policy remains focused on maintained sources.
xxhash_inc = include_directories('vendor/xxhash')
xxhash_lib = static_library('xxhash', 'vendor/xxhash/xxhash.c',
  include_directories: xxhash_inc,
  install: false,
)
xxhash_dep = declare_dependency(
  include_directories: xxhash_inc,
  link_with: xxhash_lib,
)

# The vendored SIML parser is implemented directly by siml.h, which config.c
# includes as its single translation unit.
siml_inc = include_directories('vendor/siml')
siml_dep = declare_dependency(
  include_directories: siml_inc,
)

conf = configuration_data()
conf.set_quoted('HM_VERSION', meson.project_version())
conf.set_quoted('HM_CURL', get_option('curl'))
conf.set_quoted('HM_SHELL', get_option('shell'))
configure_file(output: 'config.h', configuration: conf)

hacman_sources = files(
  'src/main.c',
  'src/util.c',
  'src/config.c',
  'src/plan.c',
  'src/cache.c',
  'src/check.c',
  'src/install.c',
  'src/sandbox.c',
)

hacman = executable('hacman', hacman_sources,
  dependencies: [siml_dep, xxhash_dep],
  include_directories: include_directories('src'),
  link_args: link_args,
  install: true,
)

test('smoke', find_program('tests.py'),
  args: [hacman.full_path()],
  workdir: meson.current_source_dir(),
)
