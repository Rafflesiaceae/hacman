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

# hacman is a POSIX program: it needs fork/execvp, clock/time and mkstemp.
add_project_arguments('-D_POSIX_C_SOURCE=200809L', language: 'c')

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

# The vendored SIML parser is header-only; siml.c is the single translation
# unit that instantiates it.
siml_inc = include_directories('vendor/siml')
siml_lib = static_library('siml', 'vendor/siml/siml.c',
  include_directories: siml_inc,
  c_args: ['-w'],  # vendored code, built as-is
  override_options: ['c_std=c89'],
)
siml_dep = declare_dependency(
  link_with: siml_lib,
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
  'src/state.c',
  'src/check.c',
  'src/install.c',
)

hacman = executable('hacman', hacman_sources,
  dependencies: siml_dep,
  include_directories: include_directories('src'),
  link_args: link_args,
  install: true,
)

test('smoke', find_program('tests.sh'),
  args: [hacman.full_path()],
  workdir: meson.current_source_dir(),
)
