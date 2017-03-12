
if(NOT HAVE_SSL_HOST_VALIDATION)
        set(COMPAT_SRCS hostcheck.c  hostcheck.h  openssl_hostname_validation.c  openssl_hostname_validation.h)
endif()
ADD_LIBRARY(osslcompat OBJECT ${COMPAT_SRCS})
SET_TARGET_PROPERTIES(osslcompat PROPERTIES COMPILE_FLAGS "${ACNG_COMPFLAGS}")
