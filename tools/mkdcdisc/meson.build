# mkdcdisc - Sega Dreamcast Disc Image Generator
#
# SPDX-License-Identifier: MIT
# Copyright (C) 2022-2026 Simulant and contributors
# (https://www.simulant.dev/)

project(
    'mkdcdisc', ['c', 'cpp'],
    default_options: ['c_std=c99', 'cpp_std=c++17'],
    version: '0.0.4'
)

#
## Host detection
#

c_compiler = meson.get_compiler('c')
is_windows_msys2 = (run_command('sh', '-c', 'echo "$MSYSTEM"', check: false).stdout().strip()) == 'MSYS'
is_windows_mingw64 = host_machine.system() == 'windows' and not is_windows_msys2
is_windows = is_windows_msys2 or is_windows_mingw64
is_linux = host_machine.system() == 'linux' or is_windows_msys2

#
## Variables
#

project_link_libraries = []
project_link_extra_args = []
project_deps = []
project_include_directories = ['src']

is_install_required = is_linux

project_sources = files(
    'src/main.cpp',
    'src/audio.cpp',
    'src/boot_checks.cpp',
    'src/build_cd.cpp',
    'src/build_gd.cpp',
    'src/plan.cpp',
    'src/cli.cpp',
    'src/disc_layout.cpp',
    'src/duplicates.cpp',
    'src/input_files.cpp',
    'src/interrupt.cpp',
    'src/ip_boot.cpp',
    'src/iso_builder.cpp',
    'src/iso_reader.cpp',
    'src/logging.cpp',
    'src/mr_image.cpp',
    'src/output_files.cpp',
    'src/progress.cpp',
    'src/temp_dir.cpp',
    'src/terminal.cpp',
    'src/sort_file.cpp',
    'src/third_party/elf_parser/elf_parser.cpp',
    'src/third_party/makeip/crc.cpp',
    'src/third_party/scramble/scramble.cpp'
)

# Use system libisofs for all OS except MinGW-w64 (where we will use local libisofs)
if not is_windows_mingw64
	project_deps += [
		dependency('libisofs-1', required: true)
    ]
endif

#
## Optional PNG support for the MR boot-logo image (-i/--bootstrap-mr-image)
#

# Optional by default: when libpng isn't available, mkdcdisc still accepts
# already-converted .mr files, it just can't convert a PNG itself. Pass
# -Dpng=enabled to turn a missing libpng into a configuration error instead
# (see meson_options.txt), which is what the CI build jobs do so they cannot
# quietly publish a binary without PNG support.
#
# MinGW-w64 asks for the static libpng: that build has to run as a standalone
# Windows executable, and an import library would make it die at startup with
# "libpng16-16.dll is missing" outside of MSYS2. The global '-static' below
# cannot fix that by itself -- pkg-config hands meson the full path of
# libpng16.dll.a, and an explicit path bypasses -static entirely.
png_dep = dependency('libpng', required: get_option('png'), static: is_windows_mingw64)
if png_dep.found()
    project_deps += [png_dep]
    # The conversion proper, from makeip and under makeip's license (see
    # src/third_party/makeip/COPYRIGHT). Listed here rather than above because
    # it is the only source in the project that has no reason to be compiled at
    # all without libpng -- src/mr_image.cpp still has a format sniffer to
    # offer, so it stays unconditional and guards its own PNG half.
    project_sources += files('src/third_party/makeip/mr.cpp')
    add_project_arguments('-DMKDCDISC_WITH_PNG=1', language: ['c', 'cpp'])
endif

#
## Windows additions for MinGW-w64 environment (but NOT FOR MSYS2!)
#

# MSYS2 is handled like a regular Unix system, as it will uses the POSIX layer
# emulation; while regular MinGW-w64 is pure native Windows solution, where
# this POSIX layer isn't available. It means for MSYS2, we can use the
# regular libisofs package from source, where for MinGW-w64 we need to use
# our patched copy provided here.

if is_windows_mingw64
    # Static for the same reason as libpng above (and because a static libpng
    # pulls zlib in with it): the .exe must not import zlib1.dll.
    zlib_dep = dependency('zlib', required: true, static: true)
    iconv_dep = dependency('iconv', required: true)
    project_deps += [
		zlib_dep,
        iconv_dep,
    ]

    libisofs_compile_args = [
        '-DHAVE_CONFIG_H=1',        
        '-DWIN32_LEAN_AND_MEAN',
#		'-D_DEBUG=1'
    ]

    libisofs = static_library(
        'isofs',
        [
			'src/third_party/libisofs/libisofs/builder.c',			
			'src/third_party/libisofs/libisofs/node.c',			
			'src/third_party/libisofs/libisofs/tree.c',
			'src/third_party/libisofs/libisofs/find.c',			
			'src/third_party/libisofs/libisofs/image.c',			
			'src/third_party/libisofs/libisofs/fsource.c',
			'src/third_party/libisofs/libisofs/fs_local.c',
			'src/third_party/libisofs/libisofs/fs_image.c',		
			'src/third_party/libisofs/libisofs/messages.c',			
			'src/third_party/libisofs/libisofs/libiso_msgs.c',			
			'src/third_party/libisofs/libisofs/stream.c',			
			'src/third_party/libisofs/libisofs/filter.c',
#			'src/third_party/libisofs/libisofs/filters/external.c',
			'src/third_party/libisofs/libisofs/filters/zisofs.c',
			'src/third_party/libisofs/libisofs/filters/gzip.c',			
			'src/third_party/libisofs/libisofs/util.c',
			'src/third_party/libisofs/libisofs/util_rbtree.c',
			'src/third_party/libisofs/libisofs/util_htable.c',			
			'src/third_party/libisofs/libisofs/filesrc.c',			
			'src/third_party/libisofs/libisofs/ecma119.c',			
			'src/third_party/libisofs/libisofs/ecma119_tree.c',			
			'src/third_party/libisofs/libisofs/buffer.c',
			'src/third_party/libisofs/libisofs/rockridge.c',
			'src/third_party/libisofs/libisofs/rockridge_read.c',
			'src/third_party/libisofs/libisofs/joliet.c',
			'src/third_party/libisofs/libisofs/hfsplus.c',
			'src/third_party/libisofs/libisofs/hfsplus_decompose.c',
			'src/third_party/libisofs/libisofs/hfsplus_classes.c',
			'src/third_party/libisofs/libisofs/hfsplus_case.c',
			'src/third_party/libisofs/libisofs/eltorito.c',
			'src/third_party/libisofs/libisofs/system_area.c',
			'src/third_party/libisofs/libisofs/make_isohybrid_mbr.c',
			'src/third_party/libisofs/libisofs/iso1999.c',
			'src/third_party/libisofs/libisofs/data_source.c',
			'src/third_party/libisofs/libisofs/aaip_0_2.c',
			'src/third_party/libisofs/libisofs/md5.c'
        ],
        include_directories: ['src/third_party/libisofs/libisofs'],
        dependencies: [zlib_dep, iconv_dep],
        c_args: libisofs_compile_args,
        install: false
    )
	
	project_include_directories += ['src/third_party/libisofs/libisofs']
	project_link_extra_args += ['-static']
    project_link_libraries += [libisofs]
endif

#
## Generated version header (used e.g. for the ISO preparer identifier)
#

# Defined here rather than in each file that shows it, so the startup banner
# and the Windows version resource cannot end up claiming different years.
copyright_string = 'Copyright (C) 2022-2026 Simulant and contributors'

version_conf = configuration_data()
version_conf.set('VERSION_STRING', meson.project_version())
version_conf.set('COPYRIGHT_STRING', copyright_string)

version_h = configure_file(
    input: 'src/version.h.in',
    output: 'version.h',
    configuration: version_conf
)

project_sources += [version_h]

#
## Windows resource file (version info, embedded via windres)
#

if is_windows
    windows = import('windows')

    version_parts = meson.project_version().split('.')

    rc_conf = configuration_data()
    rc_conf.set('VERSION_MAJOR', version_parts[0])
    rc_conf.set('VERSION_MINOR', version_parts[1])
    rc_conf.set('VERSION_PATCH', version_parts[2])
    rc_conf.set('VERSION_STRING', meson.project_version())
    rc_conf.set('COPYRIGHT_STRING', copyright_string)

    mkdcdisc_rc = configure_file(
        input: 'src/mkdcdisc.rc.in',
        output: 'mkdcdisc.rc',
        configuration: rc_conf
    )

    project_sources += windows.compile_resources(mkdcdisc_rc)
endif

#
## EDC/ECC static library (third party)
#

# The Reed-Solomon EDC/ECC encoder every raw sector goes through, Mode 1 and
# Mode 2 Form 1 alike, taken from schilytools and licensed under the CDDL rather
# than this project's MIT (see src/third_party/libedc/COPYRIGHT and
# CDDL.Schily.txt beside it). It is built as a library of its own,
# rather than compiled into libdiscimage below, so that the differently
# licensed object code stays in an archive of its own -- and because it can be:
# it includes nothing from disc_image/, only libc, its own headers and the
# table files it #includes.
#
# The table files (crctable.out, encoder_tables, l2sq_table, scramble_table)
# are deliberately absent from the source list: they are #included into
# edc_ecc.c, not compiled separately.
libedc = static_library(
    'edc', [
        'src/third_party/libedc/edc_ecc.c',
        'src/third_party/libedc/ecc.h',
        'src/third_party/libedc/edc.h',
        'src/third_party/libedc/libedc.h',
    ],
    # edc_ecc.c is upstream's file untouched, so it reaches for its own build
    # environment as <schily/mconfig.h> and friends. src/third_party/libedc/schily/
    # answers those with the handful of definitions it actually needs, and this
    # is what lets an angle-bracket include find them.
    #
    # Scoped to this target on purpose: nothing else in mkdcdisc should be able
    # to include <schily/...>. Consumers of this library go through libedc.h,
    # which pulls the same shims in by quoted path and so needs no include path
    # of its own.
    include_directories: include_directories('src/third_party/libedc'),
    # Upstream contradicts itself about do_encode_L2()'s first parameter:
    # edc_ecc.c declares it as `unsigned char inout[2352]` while ecc.h declares
    # the same function as `unsigned char *inout`. The two are compatible in C
    # (the array decays), and this is the warning the previous vendored copy had
    # silenced by editing the prototype. Silencing it here instead keeps
    # edc_ecc.c byte-identical to upstream, which is the whole point of the
    # schily/ shims. Filtered through get_supported_arguments() because older
    # compilers do not know the flag -- they do not emit the warning either.
    # -Wno-unused-const-variable is for sccsid[], the version string upstream
    # embeds in every file for the `what`/`ident` tools to find. It is meant to
    # be unreferenced.
    c_args: c_compiler.get_supported_arguments([
        '-Wno-array-parameter',
        '-Wno-unused-const-variable',
    ]),
)

#
## Disc Image static library
#

libdiscimage = static_library(
    'discimage', [
        'src/disc_image/formats/cdi.c',
        'src/disc_image/formats/mds.c',
        'src/disc_image/formats/nrg.c',
        'src/disc_image/formats/gdi.c',
        'src/disc_image/disc_image.c',
        'src/disc_image/raw_sector.c',
        'src/disc_image/raw_sector.h',
        'src/disc_image/disc_image.h',
        'src/disc_image/private.h',
    ],
    # For src/progress.h, the progress sink type the writers report through.
    # It is shared with the C++ side, so it sits above this library rather
    # than inside it.
    include_directories: include_directories('src'),
)

# The two archives travel together: the sector writers in libdiscimage call
# do_encode_L2() from libedc, so nothing can link one without the other. Handing
# consumers a single dependency keeps that pairing in one place -- the
# executable and the unit test binaries alike ask for the disc image library and
# get the encoder with it.
#
# Note this is deliberately not expressed as libdiscimage depending on libedc:
# meson would then build libdiscimage as a thin archive (ar 'T'), which gcc-ar
# rejects here, and a static library does not link another one anyway -- the
# final executable is what resolves the symbols.
libdiscimage_dep = declare_dependency(link_with: [libdiscimage, libedc])

project_deps += [libdiscimage_dep]

#
## Final Executable
#

# Wrapped via include_directories() (rather than passed as bare strings) so
# it can be safely reused from tests/meson.build: bare strings passed as
# include_directories: are resolved relative to the meson.build of the
# target being declared, not relative to where the variable was assigned.
project_include_dirs = include_directories(project_include_directories)

mkdcdisc_exe = executable(
    meson.project_name(),
	project_sources,
    dependencies: project_deps,
	link_args: project_link_extra_args,
    link_with: project_link_libraries,
	include_directories: project_include_dirs,
    install: is_install_required
)

#
## Maintenance targets
#

# Regenerate the committed C-array includes in src/assets/ from the binary
# assets in assets/ (IP.BIN boot images and the default MR logo). The includes
# are checked in precisely so a normal build needs no extra tooling, so this is
# deliberately a manual, maintainer-only target -- it never runs as part of the
# build. Run it after changing one of the source binaries:
#
#   meson compile -C builddir regen-includes   (or: ninja -C builddir regen-includes)
#
# It relies on `xxd` (from Vim); the regular build does not.
regen_sh = find_program('sh', required: false)
if regen_sh.found()
    run_target('regen-includes',
        command: [regen_sh, files('tools/regen-includes.sh')])
endif

#
## Tests
#

# Archicast vendored copy: tests/ is not shipped (see VENDOR.md).
# subdir('tests')
