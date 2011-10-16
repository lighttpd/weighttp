#! /usr/bin/env python
# encoding: utf-8

"""
 * weighttp - a lightweight and simple webserver benchmarking tool
 *
 * Author:
 *     Copyright (c) 2009-2011 Thomas Porzelt
 *
 * License:
 *     MIT, see COPYING file
"""

import Options

# the following two variables are used by the target "waf dist"
VERSION='0.3'
APPNAME='weighttp'

# these variables are mandatory ('/' are converted automatically)
srcdir = '.'
blddir = 'build'


def set_options(opt):
	opt.tool_options('compiler_cc')
	
	# ./waf configure options
	#opt.add_option('--with-xyz', action='store_true', help='with xyz', dest = 'xyz', default = False)


def configure(conf):
	conf.env['CCFLAGS'] += [
		'-std=gnu99', '-Wall', '-Wshadow', '-W', '-pedantic', '-g', '-g2', '-O2', '-Wmissing-declarations',
		'-Wdeclaration-after-statement', '-Wno-pointer-sign', '-Wcast-align', '-Winline', '-Wsign-compare',
		'-Wnested-externs', '-Wpointer-arith', '-Wbad-function-cast', '-Wmissing-prototypes',
		'-fPIC', '-fno-strict-aliasing',
	]

	conf.check_tool('compiler_cc')

	# check for libev
	conf.check(lib='ev', uselib_store='ev', mandatory=True)
	conf.check(header_name='ev.h', uselib='ev', mandatory=True)

	# check for libpthread
	conf.check(lib='pthread', uselib_store='pthread', mandatory=True)
	conf.check(header_name='pthread.h', uselib='pthread', mandatory=True)

	# check for needed headers
	conf.check(header_name='unistd.h')
	conf.check(header_name='stdint.h')
	conf.check(header_name='fcntl.h')
	conf.check(header_name='inttypes.h')

	# check for needed functions
	#conf.check(function_name='writev', header_name='sys/uio.h', define_name='HAVE_WRITEV')


def build(bld):
	bld.new_task_gen(
		features = 'cc cprogram',
		source = ['src/client.c', 'src/weighttp.c', 'src/worker.c'],
		defines = ['HAVE_CONFIG_H=1', 'VERSION="' + VERSION + '"'],
		includes = '.',
		uselib = 'ev pthread',
		target = 'weighttp'
	)
