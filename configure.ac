dnl Process this file with autoconf to produce a configure script.
dnl
dnl Copyright 2002-2008, 2010, 2012-2015, 2017, 2021, 2023-2025 Andrew Wood
dnl
dnl License GPLv3+: GNU GPL version 3 or later; see `docs/COPYING'.

AC_INIT([pv], [1.9.31], [https://codeberg.org/ivarch/pv/issues], [pv], [https://ivarch.com/programs/pv.shtml])
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
AC_CHECK_FUNCS([setitimer])
AC_CHECK_FUNCS([setproctitle])
AC_CHECK_HEADERS([getopt.h])
AC_CHECK_HEADERS([limits.h])
AC_CHECK_HEADERS([langinfo.h])
AC_CHECK_HEADERS([wctype.h])
AC_CHECK_HEADERS([termios.h])
AC_CHECK_HEADERS([libgen.h])
AC_CHECK_HEADERS([sys/ioctl.h])
AC_CHECK_HEADERS([libintl.h locale.h])
AC_CHECK_HEADERS([sys/sysmacros.h])
AC_CHECK_MEMBERS([struct stat.st_blksize])
AC_CHECK_DECLS([SA_SIGINFO], [], [], [[#include <signal.h>]])

AC_SEARCH_LIBS([sqrtl],[m])
AC_SEARCH_LIBS([fmod],[m])
AC_CHECK_HEADERS([math.h])
AC_CHECK_FUNCS([sqrtl])
AC_CHECK_FUNCS([fmod])

dnl Libraries that may contain key functions.
AC_SEARCH_LIBS([clock_gettime],[rt])

dnl Items we can't do without.
AC_CHECK_FUNCS([alarm basename clock_gettime dup2 getcwd memcpy memmove memset mkdir select setlocale strchr strerror strrchr strstr], [], [AC_MSG_ERROR([required function is missing])])
AC_CHECK_HEADERS([fcntl.h sys/file.h sys/time.h unistd.h], [], [AC_MSG_ERROR([required header file is missing])])

dnl Make sure all the types we use are defined.
AC_TYPE_INT32_T
AC_TYPE_MODE_T
AC_TYPE_OFF_T
AC_TYPE_PID_T
AC_TYPE_SIZE_T
AC_TYPE_SSIZE_T
AC_TYPE_UID_T

AC_CHECK_MEMBERS([struct stat.st_rdev])

dnl Check whether a signal handler can see the PID of the sender.
AC_MSG_CHECKING([siginfo_t provides a signal sender's PID])
AC_RUN_IFELSE(
[AC_LANG_PROGRAM([[
#include <string.h>
#include <signal.h>
#include <unistd.h>

static pid_t signalSender = 0;

#ifdef SA_SIGINFO
static void receiveUSR2( __attribute__((unused))
			int sig, siginfo_t * info, __attribute__((unused))
			void *ucontext)
{
	if (NULL != info)
		signalSender = info->si_pid;
}
#endif
]], [[
	struct sigaction sa;
	memset(&sa, 0, sizeof(sa));
#ifdef SA_SIGINFO
	sa.sa_sigaction = receiveUSR2;
	(void) sigemptyset(&(sa.sa_mask));
	sa.sa_flags = SA_SIGINFO;
	(void) sigaction(SIGUSR2, &sa, NULL);
	raise(SIGUSR2);
#endif
	return signalSender < 1 ? 1 : 0;
]])], [
AC_MSG_RESULT([yes])
AC_DEFINE([SIGINFO_PROVIDES_PID], [1], [Signal handlers can determine the sending PID])
], [
AC_MSG_RESULT([no])
], [
AC_MSG_RESULT([cannot check, assuming yes])
AC_DEFINE([SIGINFO_PROVIDES_PID], [1], [Signal handlers can determine the sending PID])
]
)

AH_BOTTOM([#include "config-aux.h"])

AC_ARG_ENABLE([debugging],
  [AS_HELP_STRING([--enable-debugging], [compile with debugging support])],
  if test "$enable_debugging" = "yes"; then
  	CFLAGS="$CFLAGS -g -Wall"
	AC_DEFINE([ENABLE_DEBUGGING], [1], [Debugging support enabled])
  fi
)

AC_ARG_ENABLE([profiling],
  [AS_HELP_STRING([--enable-profiling], [compile with profiling support])],
  if test "$enable_profiling" = "yes"; then
  	CFLAGS="-pg $CFLAGS"
  fi
)

SPLICE_SUPPORT="no"
AC_ARG_ENABLE([splice],
  [AS_HELP_STRING([--disable-splice], [do not use splice system call])],
  if test "$enable_splice" = "yes"; then
    SPLICE_SUPPORT="yes"
  fi,
  SPLICE_SUPPORT="yes"
)

IPC_SUPPORT="no"
AC_ARG_ENABLE([ipc],
  [AS_HELP_STRING([--disable-ipc], [turn off IPC messaging])],
  if test "$enable_ipc" = "yes"; then
    IPC_SUPPORT="yes"
  fi,
  IPC_SUPPORT="yes"
)

AC_ARG_WITH([ncurses],
  [AS_HELP_STRING([--without-ncurses], [disable the use of ncurses for terminal info])],
  [],
  [with_curses=yes])
AS_IF([test "x$with_ncurses" != xno],
[
    AC_SEARCH_LIBS([tigetnum],[tinfo ncurses], [
      AC_CHECK_HEADERS([term.h],[
        AC_DEFINE([ENABLE_NCURSES], [1], [Build with ncurses support])
      ])
    ], [])
],
[])

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
  AC_CHECK_HEADERS([sys/shm.h], [], [IPC_SUPPORT=no])
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
