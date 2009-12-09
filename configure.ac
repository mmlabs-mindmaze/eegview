m4_define([bin_release],0)
m4_define([bin_current],0)
m4_define([bin_revision],0)
m4_define([bin_age],0)


AC_INIT(eegview,[bin_release.bin_current.bin_age.bin_revision])
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

LT_INIT([win32-dll])
AC_SUBST(LIBTOOL_DEPS)

AC_HEADER_STDC
AC_C_CONST


os_support=other
case $host in
	*win32* | *mingw32* | *cygwin* | *windows*)
		AM_LDFLAGS="$AM_LDFLAGS -no-undefined"
		os_support=win32
		;;
esac


AC_SEARCH_LIBS([pthread_create], [pthread posix4], [], AC_MSG_ERROR([The pthread library must be installed. Consider the installation of pthreads-win32 if on windows platform.]))
AC_SEARCH_LIBS([eegpanel_create], [eegpanel], [], AC_MSG_ERROR([The eegpanel library must be installed.]))
AC_SEARCH_LIBS([ActivetwoConnectToSystem], [activetwo act2demux], [], AC_MSG_ERROR([The act2lib library must be installed.]))
AC_SEARCH_LIBS([OpenBDFFile], [bdffileio], [], AC_MSG_ERROR([The bdffileio library must be installed.]))
AC_SEARCH_LIBS([cfg_parse], [confuse], [], AC_MSG_ERROR([The libconfuse library must be installed.]))

AC_SUBST(AM_LDFLAGS)

AC_CONFIG_FILES([Makefile src/Makefile])
AC_OUTPUT

