cmake_minimum_required(VERSION 2.6)

unset(SDTYPE)
if(HAVE_SD_NOTIFY)
	set(SDTYPE notify)
else()
	set(SDTYPE simple)
endif()
CONFIGURE_FILE(apt-cacher-ng.service.in apt-cacher-ng.service)
CONFIGURE_FILE(apt-cacher-ng.conf.in apt-cacher-ng.conf)
unset(SDTYPE)

# this is experimental and is supposed to match LSB
if(SDINSTALL)
	install(FILES ${CMAKE_CURRENT_BINARY_DIR}/apt-cacher-ng.conf DESTINATION ${CMAKE_INSTALL_PREFIX}/lib/tmpfiles.d/)
	install(FILES ${CMAKE_CURRENT_BINARY_DIR}/apt-cacher-ng.service DESTINATION /lib/systemd/system)
endif()
