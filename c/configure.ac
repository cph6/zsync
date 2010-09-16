# Process this file with autoconf to produce a configure script.

AC_INIT([zsync],[0.6.2],[http://zsync.moria.org.uk/])

AC_CONFIG_SRCDIR([client.c])
AC_CONFIG_AUX_DIR([autotools])
AC_CONFIG_MACRO_DIR([autotools])

AC_CANONICAL_HOST

AM_INIT_AUTOMAKE([dist-bzip2 no-dist-gzip foreign check-news -Woverride -Wobsolete -Wportability -Wsyntax -Wunsupported])
AM_MAINTAINER_MODE

dnl --- Check for programs
AC_PROG_CC
AC_PROG_LN_S
AC_PROG_RANLIB

AC_ARG_ENABLE([profile],
   AS_HELP_STRING([--enable-profile],[Turns on profiling]))

AS_IF([test "x$enable_profile" = "xyes"], [
   ZS_CFLAGS="${ZS_CFLAGS} -pg" ])

dnl --- Header files, typedefs, structures, libraries
AC_C_CONST
AC_HEADER_STDC
# string.h, memory.h, stdlib.h both included in the default header checks
# but we do need to give at least one .h to test, or Solaris sh errors
AC_CHECK_HEADERS([string.h])
AC_TYPE_SIZE_T
AC_CHECK_FUNCS(memcpy pwrite pread mkstemp)

X_TYPE_SOCKLEN_T
X_TYPE_IN_PORT_T
X_DECL_H_ERRNO

dnl Solaris needs -lsocket - and we need this for the getaddrinfo test
AC_CHECK_LIB(socket,socket)

AC_REPLACE_FUNCS(getaddrinfo)

dnl - Large file support if available
AC_SYS_LARGEFILE
AC_FUNC_FSEEKO
AC_CHECK_SIZEOF(size_t)
AC_CHECK_SIZEOF(off_t)

AM_WITH_DMALLOC

case $host_os in
   mingw32)
       host_os_mingw32=yes
       LIBS="${LIBS} -lwsock32"
       ;;
   *)
       AC_DEFINE([_XOPEN_SOURCE], 600, [Enable POSIX extensions if present])
       AC_DEFINE([_BSD_SOURCE],1, [Enable BSD extensions if present])
       ;;
esac
AM_CONDITIONAL([MINGW32], test "x$host_os_mingw32" = "xyes")

X_C_COMPILE_FLAGS($ZS_CFLAGS -g -Wall -Wwrite-strings -Winline -Wextra -Winline -Wmissing-noreturn -Wredundant-decls -Wnested-externs -Wundef -Wbad-function-cast -Wcast-align -Wvolatile-register-var -ffast-math)

dnl --- output
AC_SUBST(ac_aux_dir)
AC_CONFIG_HEADERS([config.h])
AC_CONFIG_FILES([Makefile librcksum/Makefile zlib/Makefile libzsync/Makefile doc/Makefile])
AC_OUTPUT

