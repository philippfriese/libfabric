dnl Configury specific to the libfabrics monitor hooking provider

dnl Called to configure this provider
dnl
dnl Arguments:
dnl
dnl $1: action if configured successfully
dnl $2: action if not configured successfully
dnl

AC_DEFUN([FI_MONITOR_CONFIGURE],[
    # Determine if we can support the monitor hooking provider
    monitor_happy=0

    # Support non-standard install path for curl. This is needed by fi_mon_sampler utility.
    AC_ARG_WITH([curl],
            [AS_HELP_STRING([--with-curl=DIR], [Install directory for curl])],
            [])

    AS_IF([test x"$enable_monitor" != x"no"], [
            monitor_happy=1
            # Add on curl if installed in non-default location.
            if test "$with_curl" != "" && test "$with_curl" != "no"; then
                FI_CHECK_PREFIX_DIR([$with_curl], [curl])
            else
                curl_PREFIX=""
                curl_LIBDIR=""
            fi

            FI_CHECK_PACKAGE([libcurl],
                [curl/curl.h],
                [curl],
                [curl_global_init],
                [],
                [$curl_PREFIX],
                [$curl_LIBDIR],
                [],
                [monitor_happy=0])

            fi_mon_sampler_CFLAGS="$libcurl_CPPFLAGS"
            fi_mon_sampler_LDFLAGS="$libcurl_LDFLAGS"
            fi_mon_sampler_LIBS="$libcurl_LIBS"

            AC_SUBST(fi_mon_sampler_CFLAGS)
            AC_SUBST(fi_mon_sampler_LDFLAGS)
            AC_SUBST(fi_mon_sampler_LIBS)
        ],
        [monitor_happy=0])
    AS_IF([test $monitor_happy -eq 1], [$1], [$2])
])
