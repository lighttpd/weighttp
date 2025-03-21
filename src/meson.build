compiler = meson.get_compiler('c')

warn_c_args = [
  '-Wshadow',
  '-W',
  '-pedantic',
]

add_project_arguments(
  compiler.get_supported_arguments(warn_c_args),
  language: 'c'
)

conf_data = configuration_data()
conf_data.set_quoted('PACKAGE_NAME', meson.project_name())
conf_data.set_quoted('PACKAGE_VERSION', meson.project_version())

configure_file(
  output : 'config.h',
  configuration : conf_data,
)

defs = [
  '-D_XOPEN_SOURCE=700',
  '-DHAVE_CONFIG_H',
]

weighttp_cflags = defs

weighttp_flags = [ declare_dependency(
  compile_args: weighttp_cflags,
) ]

thread_dep = dependency('threads')
m_dep = compiler.find_library('m', required : false)

executable('weighttp',
  sources: files( 'weighttp.c' ),
  dependencies: [ weighttp_flags, thread_dep, m_dep ],
  install: true,
)
