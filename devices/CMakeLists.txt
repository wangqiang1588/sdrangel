project(devices)

find_package(LibUSB)

if (BUILD_DEBIAN)
    add_subdirectory(bladerf)
    add_subdirectory(hackrf)
    add_subdirectory(limesdr)
    add_subdirectory(perseus)
    add_subdirectory(plutosdr)
else(BUILD_DEBIAN)
    find_package(LibBLADERF)
    if(LIBUSB_FOUND AND LIBBLADERF_FOUND)
        add_subdirectory(bladerf)
    endif(LIBUSB_FOUND AND LIBBLADERF_FOUND)

    find_package(LibHACKRF)
    if(LIBUSB_FOUND AND LIBHACKRF_FOUND)
        add_subdirectory(hackrf)
    endif(LIBUSB_FOUND AND LIBHACKRF_FOUND)

    find_package(LimeSuite)
    if(LIBUSB_FOUND AND LIMESUITE_FOUND)
        add_subdirectory(limesdr)
    endif(LIBUSB_FOUND AND LIMESUITE_FOUND)
    
    find_package(LibIIO)
    if(LIBUSB_FOUND AND LIBIIO_FOUND)
        add_subdirectory(plutosdr)
    endif(LIBUSB_FOUND AND LIBIIO_FOUND)
    
    find_package(LibPerseus)
    if(LIBUSB_FOUND AND LIBPERSEUS_FOUND)
        add_subdirectory(perseus)
    endif()
    
endif (BUILD_DEBIAN)
