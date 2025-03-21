compiler = meson.get_compiler('c')

include_base_paths = [
	'/usr/include',
	'/usr/local/include',
#	'/opt/local/include',
]

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

if compiler.get_id() == 'gcc' or compiler.get_id() == 'clang'
	weighttp_cflags += [
		'-pipe',
		'-Wall',
		'-g',
		'-g2',
		'-O2',
		'-Wshadow',
		'-W',
		'-pedantic',
		'-pthread',
	]
endif

weighttp_flags = [ declare_dependency(
	compile_args: weighttp_cflags,
	link_args: [ '-lm' ],
) ]

executable('weighttp',
	sources: files( 'weighttp.c' ),
	dependencies: [ weighttp_flags ],
	install: true,
	install_dir: '/bin',
)
