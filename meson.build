project('eegview', 'c',
        version : '1.0',
        default_options : [
            'warning_level=3',
        ],
        meson_version: '>= 0.49'
)

cc = meson.get_compiler('c')
configuration_inc = include_directories('.', 'src')

config = configuration_data()
# Defined for compatibility with autotools
# The other such macros defined by autotools are not used and will not be added
config.set('PACKAGE_STRING', '"' + meson.project_name() + ' ' + meson.project_version() + '"')
config.set('PACKAGE_NAME', '"' + meson.project_name() + '"')
config.set('PACKAGE_VERSION', '"' + meson.project_version() + '"')

# write config file
build_cfg = 'config.h'  # named as such to match autotools build system
configure_file(output : build_cfg, configuration : config)

# define HAVE_CONFIG_H with compiler command line to include the generated
# config.h file (same as autotools)
add_project_arguments('-DHAVE_CONFIG_H', language : 'c')

# additional (optional) warnings
flags = [
    '-Wshadow',
    '-Wstrict-prototypes',
    '-Wmissing-prototypes',
]
add_project_arguments(cc.get_supported_arguments(flags), language : 'c')

sources = files(
    'src/eegview.c',
    'src/event-tracker.c',
    'src/event-tracker.h',
)

threads = dependency('threads', required : true)
eegdev = cc.find_library('eegdev', required : true)
mcpanel = cc.find_library('mcpanel', required : true)
mmlib = cc.find_library('mmlib', required : true)
xdffileio = cc.find_library('xdffileio', required : true)

eegview = executable('eegview',
        sources,
        install : true,
        include_directories : configuration_inc,
        dependencies : [eegdev, mcpanel, mmlib, threads, xdffileio],
)

install_man(files('doc/eegview.1'))

install_data('data/eegview.desktop',
        install_dir : get_option('datadir') / 'applications')

install_data('data/eegview.conf',
        install_dir : get_option('datadir') / 'doc' / 'eegview')
