include(GNUInstallDirs)
install(TARGETS winhttppal
    EXPORT winhttppal-export
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
)
