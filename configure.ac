m4_define([bin_current],0)
m4_define([bin_revision],1)
m4_define([bin_age],0)


AC_INIT(eegview,[bin_current.bin_revision],[nicolas.bourdaud@gmail.com])
AC_CONFIG_SRCDIR([src/eegview.c])
#AC_CONFIG_LIBOBJ_DIR([lib])
AC_CONFIG_MACRO_DIR([m4])
AC_CONFIG_AUX_DIR([build-aux])
AC_CONFIG_HEADERS([config/config.h])
AM_INIT_AUTOMAKE

AC_CANONICAL_HOST
AC_CANONICAL_BUILD

AC_PROG_CC_C99
AM_PROG_CC_C_O

AC_SUBST(LIBTOOL_DEPS)

AC_HEADER_STDC
AC_C_CONST


os_support=other
case $host in
	*win32* | *mingw32* | *cygwin* | *windows*)
		#AM_LDFLAGS="$AM_LDFLAGS -no-undefined"
		os_support=win32
		;;
esac

AC_SEARCH_LIBS([pthread_create], [pthread posix4], [], AC_MSG_ERROR([The pthread library must be installed. Consider the installation of pthreads-win32 if on windows platform.]))
AC_SEARCH_LIBS([mcp_create], [mcpanel], [], AC_MSG_ERROR([The mcpanel library must be installed.]))
AC_SEARCH_LIBS([xdf_open], [xdffileio], [], AC_MSG_ERROR([The xdffileio library must be installed.]))
AC_SEARCH_LIBS([egd_start], [eegdev], [], AC_MSG_ERROR([The eegdev library must be installed.]))
AC_SEARCH_LIBS([mm_socket], [mmlib], [], AC_MSG_ERROR([The mmlib library must be installed.]))
AC_SUBST(AM_LDFLAGS)

AC_CONFIG_FILES([Makefile src/Makefile doc/Makefile data/Makefile])
AC_OUTPUT

