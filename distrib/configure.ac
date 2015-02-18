dnl Process this file with autoconf to produce a configure script.

AC_PREREQ(2.57)
AC_INIT(ibssa, 0.0.8, linux-rdma@vger.kernel.org)
AC_CONFIG_SRCDIR([src/distrib.c])
AC_CONFIG_AUX_DIR(config)
AM_CONFIG_HEADER(config.h)
AM_INIT_AUTOMAKE([subdir-objects])
AM_INIT_AUTOMAKE
m4_ifdef([AM_SILENT_RULES], [AM_SILENT_RULES([yes])])

AM_PROG_LIBTOOL

AC_ARG_ENABLE(libcheck, [  --disable-libcheck  do not test for presence of ib libraries],
[       if test "$enableval" = "no"; then
                disable_libcheck=yes
        fi
])

dnl Checks for programs
AC_PROG_CC

dnl Checks for typedefs, structures, and compiler characteristics.
AC_C_CONST
AC_CHECK_SIZEOF(long)

AC_ARG_ENABLE(fake-acm,
	AS_HELP_STRING([--enable-fake-acm],
		       [enable using fake ACM clients [default=disable]]),
	with_fake_acm="true", with_fake_acm="false")

if test "x$with_fake_acm" == xtrue; then
	AC_DEFINE([SIM_SUPPORT_FAKE_ACM], [], ["using fake ACM clients in access"])
	AC_MSG_NOTICE([fake ACM mode is enabled])
else
	AC_MSG_NOTICE([fake ACM mode is disabled by default])
fi

dnl Checks for libraries
if test "$disable_libcheck" != "yes"; then
AC_CHECK_LIB(ibverbs, ibv_get_device_list, [],
    AC_MSG_ERROR([ibv_get_device_list() not found.  ibssa requires libibverbs.]))
AC_CHECK_LIB(rdmacm, rselect, [],
    AC_MSG_ERROR([rselect() not found.  ibssa requires librdmacm.]))
AC_CHECK_LIB(ibumad, umad_send, [],
    AC_MSG_ERROR([umad_send() not found.  ibssa requires libibumad.]))
PKG_CHECK_MODULES([GLIB], [glib-2.0 >= 2.2])
AC_CHECK_LIB(gthread-2.0, g_thread_init, [],
    AC_MSG_ERROR([g_thread_init() not found.  libgthread-2.0 is required.]))
fi

dnl Checks for header files.
AC_HEADER_STDC
if test "$disable_libcheck" != "yes"; then
AC_CHECK_HEADER(infiniband/verbs.h, [],
    AC_MSG_ERROR([<infiniband/verbs.h> not found.  Is libibverbs installed?]))
AC_CHECK_HEADER(rdma/rsocket.h, [],
    AC_MSG_ERROR([<rdma/rsocket.h> not found.  Is librdmacm installed?]))

dnl Checks for getsockopt RDMA_ROUTE support in librdmacm.
AC_LANG(C)
AC_MSG_CHECKING([rsockets getsockopt RDMA_ROUTE])
AC_RUN_IFELSE(
    [AC_LANG_PROGRAM(
        [[#include <stdio.h>
          #include <errno.h>
          #include <netinet/in.h>
          #include <netinet/tcp.h>
          #include <sys/types.h>
          #include <rdma/rsocket.h>
          #include <infiniband/ib.h>]],
        [[
          int rsock;
          struct ibv_path_data route;
          socklen_t route_len = sizeof(route) - 1;
          rsock = rsocket(AF_IB, SOCK_STREAM, 0);
          if (rsock < 0)
              return 1;
          rgetsockopt(rsock, SOL_RDMA, RDMA_ROUTE, &route, &route_len);
          rclose(rsock);
          if (errno != EINVAL)
              return 1;
          return 0;
        ]])],
        [ AC_MSG_RESULT([yes]) ],
        [ AC_MSG_RESULT([no])
          AC_MSG_WARN([rsockets does not support getsockopt RDMA_ROUTE.  librdmacm 1.0.19 or beyond is needed.])
        ]
)

AC_CHECK_MEMBER(struct umad_hdr.base_version, [],
    AC_MSG_ERROR([struct umad_hdr not found.  A newer version of libibumad is required.]),
    [#include <infiniband/umad_types.h>])
fi

dnl Check for pthread_setname_np function
AC_CHECK_FUNC(pthread_setname_np,
    AC_DEFINE(HAVE_PTHREAD_SET_NAME_NP, [1], [Define if using pthread_setname_np]))

dnl Define package version
commit="`git log -n 1 --pretty=format:"%h" 2> /dev/null`"
if test "$commit" == "" ; then
	ver="${PACKAGE_VERSION}"
else
	ver="${PACKAGE_VERSION}_${commit}"
	git diff-index --quiet HEAD -- ../ > /dev/null 2>&1
	if test "$?" == "1" ; then
		ver="${ver}_dirty"
	fi
fi
AC_DEFINE_UNQUOTED([IB_SSA_VERSION], ["$ver"], [ibssa package version])

dnl Check for gcc atomic intrinsics
AC_MSG_CHECKING(compiler support for atomics)
AC_TRY_LINK([int i = 0;],
    [ return __sync_add_and_fetch(&i, 1) != __sync_sub_and_fetch(&i, 1); ],
    [ AC_MSG_RESULT(yes) ],
    [
        AC_MSG_RESULT(no)
        AC_DEFINE(DEFINE_ATOMICS, 1, [Set to 1 to implement atomics])
    ])

AC_CACHE_CHECK(whether ld accepts --version-script, ac_cv_version_script,
    if test -n "`$LD --help < /dev/null 2>/dev/null | grep version-script`"; then
        ac_cv_version_script=yes
    else
        ac_cv_version_script=no
    fi)

AM_CONDITIONAL(HAVE_LD_VERSION_SCRIPT, test "$ac_cv_version_script" = "yes")

AC_ARG_VAR(rdmadir, [Directory for configuration files])
if test "x$rdmadir" = "x"; then
   AC_SUBST(rdmadir, rdma)
fi
AC_SUBST(sbindir)

AC_ARG_VAR(rdmascript, [Required dependency script name])
if test "x$rdmascript" = "x"; then
   if test -n "`grep -q 'release 6' /etc/redhat-release`"; then
      AC_SUBST(rdmascript, rdma)
   else
      AC_SUBST(rdmascript, openibd)
   fi
fi

AC_PATH_PROGS([ADDR2LINE], [addr2line])
if test "x${ADDR2LINE}" = x
then
	echo "WARN: addr2line missing!"
else
AC_DEFINE_UNQUOTED(ADDR2LINE_PATH, ["$ADDR2LINE"], [Path to addr2line])
AC_DEFINE(HAVE_ADDR2LINE, [1], [Define if using addr2line])
fi

AC_PATH_PROGS([GSTACK], [gstack])
if test "x${GSTACK}" = x
then
	AC_PATH_PROGS([GSTACK], [pstack])
fi
if test "x${GSTACK}" = x
then
	echo "WARN: gstack missing!"
else
AC_DEFINE_UNQUOTED(GSTACK_PATH, ["$GSTACK"], [Path to gstack])
AC_DEFINE(HAVE_GSTACK, [1], [Define if using gstack])
fi

AC_CONFIG_FILES([Makefile ibssa.spec ibssa.init])
AC_OUTPUT