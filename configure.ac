dnl Process this file with autoconf to produce a configure script.
dnl
dnl Copyright 2002-2008, 2010, 2012-2015, 2017, 2021, 2023 Andrew Wood
dnl
dnl License GPLv3+: GNU GPL version 3 or later; see `docs/COPYING'.

AC_INIT([pv], [1.8.5], [pv@ivarch.com], [pv], [https://www.ivarch.com/programs/pv.shtml])
AC_CONFIG_SRCDIR([src/include/config.h.in])
AC_CONFIG_AUX_DIR([build-aux])
AC_CONFIG_HEADERS([src/include/config.h])
AM_INIT_AUTOMAKE([-Wall -Werror foreign])
AC_LANG(C)
AC_PROG_CC
AC_PROG_INSTALL
AC_USE_SYSTEM_EXTENSIONS
AC_C_CONST
AC_HEADER_STDBOOL
AC_SYS_LARGEFILE
AM_GNU_GETTEXT_VERSION([0.21])
AM_GNU_GETTEXT([external])
if test "$USE_NLS" = "yes"; then
  CPPFLAGS="$CPPFLAGS -DLOCALEDIR=\\\"\$(localedir)\\\""
  LIBS="$LIBS $LIBINTL"
fi

dnl Items we can use if present, but can do without if not present.
AC_CHECK_FUNCS([getopt_long])
AC_CHECK_FUNCS([vsnprintf strlcat strtoul memrchr])
AC_CHECK_FUNCS([fdatasync])
AC_CHECK_FUNCS([fpathconf sysconf posix_memalign posix_fadvise])
AC_CHECK_FUNCS([nanosleep])
AC_CHECK_HEADERS([getopt.h])
AC_CHECK_HEADERS([limits.h])
AC_CHECK_HEADERS([wctype.h])
AC_CHECK_HEADERS([termios.h])
AC_CHECK_HEADERS([libgen.h])
AC_CHECK_HEADERS([sys/ioctl.h])
AC_CHECK_HEADERS([libintl.h locale.h])
AC_CHECK_MEMBERS([struct stat.st_blksize])

dnl Libraries that may contain key functions.
AC_SEARCH_LIBS([clock_gettime],[rt])

dnl Items we can't do without.
AC_CHECK_FUNCS([alarm basename clock_gettime dup2 getcwd memcpy memmove memset select setlocale strchr strerror strrchr strstr], [], [AC_MSG_ERROR([required function is missing])])
AC_CHECK_HEADERS([fcntl.h sys/file.h sys/time.h unistd.h], [], [AC_MSG_ERROR([required header file is missing])])

dnl Make sure all the types we use are defined.
AC_TYPE_INT32_T
AC_TYPE_MODE_T
AC_TYPE_OFF_T
AC_TYPE_PID_T
AC_TYPE_SIZE_T
AC_TYPE_SSIZE_T
AC_TYPE_UID_T

AH_BOTTOM([#include "config-aux.h"])

AC_ARG_ENABLE([debugging],
  [  --enable-debugging      compile with debugging support],
  if test "$enable_debugging" = "yes"; then
  	CFLAGS="$CFLAGS -g -Wall"
	AC_DEFINE([ENABLE_DEBUGGING], [1], [Debugging support enabled])
  fi
)

AC_ARG_ENABLE([profiling],
  [  --enable-profiling      compile with profiling support],
  if test "$enable_profiling" = "yes"; then
  	CFLAGS="-pg $CFLAGS"
  fi
)

SPLICE_SUPPORT="no"
AC_ARG_ENABLE([splice], [  --disable-splice        do not use splice system call],
  if test "$enable_splice" = "yes"; then
    SPLICE_SUPPORT="yes"
  fi,
  SPLICE_SUPPORT="yes"
)

IPC_SUPPORT="no"
AC_ARG_ENABLE([ipc], [  --disable-ipc           turn off IPC messaging],
  if test "$enable_ipc" = "yes"; then
    IPC_SUPPORT="yes"
  fi,
  IPC_SUPPORT="yes"
)

AC_ARG_ENABLE([static],
  [AS_HELP_STRING([--enable-static], [enable static linking])],
  [
    if test "$enable_static" = "yes"; then
      CFLAGS="$CFLAGS -static"
    fi
  ])

dnl AIX needs -lc128
AC_EGREP_CPP([^yes$], [#ifdef _AIX
yes
#endif
], [LIBS="$LIBS -lc128"])

if test "$IPC_SUPPORT" = "yes"; then
  AC_CHECK_HEADERS([sys/ipc.h], [], [IPC_SUPPORT=no])
  AC_CHECK_HEADERS([sys/param.h])
  if test "$IPC_SUPPORT" = "yes"; then
    AC_CHECK_FUNCS([shmget], [], [IPC_SUPPORT=no])
  fi
fi

if test "$IPC_SUPPORT" = "yes"; then
  AC_DEFINE([HAVE_IPC], [1], [IPC support enabled])
fi

if test "$SPLICE_SUPPORT" = "yes"; then
  AC_CHECK_FUNCS([splice])
fi

CPPFLAGS="$CPPFLAGS -I\$(top_srcdir)/src/include"

dnl This must go after all the compiler based tests above.
AC_LANG_WERROR

AC_CONFIG_FILES([
 po/Makefile.in
 Makefile
])

AC_OUTPUT
