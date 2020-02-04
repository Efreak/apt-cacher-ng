PROJECT(ACNG CXX C)

CONFIGURE_FILE("acng.conf.in" "${CMAKE_CURRENT_BINARY_DIR}/acng.conf")
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/acng.conf" DESTINATION ${CFGDIR})
install(FILES security.conf DESTINATION ${CFGDIR})
file(GLOB OTHER_DATA "*mirrors" "*mirrors.gz" "*.html" "*.css" "*default")
install(FILES ${OTHER_DATA} DESTINATION ${LIBDIR})

#message("${OTHER_DATA} goes to ${LIBDIR}")

