cmake_minimum_required(VERSION 3.21)

project(AMRMapManager VERSION 0.3.0 LANGUAGES CXX)

include(GNUInstallDirs)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
if(MSVC)
    add_compile_options(/utf-8)
endif()

option(BUILD_MAP_MANAGER "Build the desktop map manager" ON)
find_package(Qt6 6.2 REQUIRED COMPONENTS Core Network)

add_library(amr-map-core STATIC
    src/core/Robot.h
    src/core/MapDeployer.h
    src/core/MapDeployer.cpp
    src/protocol/RbkProtocol.h
    src/protocol/RbkProtocol.cpp
    src/network/RequestJob.h
    src/network/RequestJob.cpp
)
target_include_directories(amr-map-core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(amr-map-core PUBLIC Qt6::Core Qt6::Network)

qt_add_executable(amr-map-agent
    src/agent/main.cpp
    src/agent/FleetClient.h
    src/agent/FleetClient.cpp
    src/agent/MapAgent.h
    src/agent/MapAgent.cpp
)
target_link_libraries(amr-map-agent PRIVATE amr-map-core)
install(TARGETS amr-map-agent RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

include(CTest)
if(BUILD_TESTING)
    qt_add_executable(map-deployer-test tests/MapDeployerTest.cpp)
    target_link_libraries(map-deployer-test PRIVATE amr-map-core)
    add_test(NAME map-deployer COMMAND map-deployer-test)
    find_package(Python3 COMPONENTS Interpreter)
    if(Python3_Interpreter_FOUND)
        add_test(NAME map-agent COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_agent.py $<TARGET_FILE:amr-map-agent>)
    endif()
endif()

if(BUILD_MAP_MANAGER)
find_package(Qt6 6.2 REQUIRED COMPONENTS Widgets Concurrent)

qt_add_executable(amr-map-manager
    src/main.cpp
    src/MainWindow.h
    src/MainWindow.cpp
    src/map/MapView.h
    src/map/MapView.cpp
    src/map/LocalizationHeatmap.h
    src/map/LocalizationHeatmap.cpp
    resources.qrc
)

# Source files use project-relative includes such as
# "protocol/RbkProtocol.h" and "network/RequestJob.h".
target_include_directories(amr-map-manager PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(amr-map-manager PRIVATE
    amr-map-core
    Qt6::Core
    Qt6::Widgets
    Qt6::Network
    Qt6::Concurrent
)

if(WIN32)
    set_target_properties(amr-map-manager PROPERTIES WIN32_EXECUTABLE TRUE)
    set(APP_ICON_ICO "${CMAKE_CURRENT_SOURCE_DIR}/assets/icons/svi-logo.ico")
    configure_file(packaging/windows/app.rc.in
                   "${CMAKE_CURRENT_BINARY_DIR}/app.rc" @ONLY)
    target_sources(amr-map-manager PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/app.rc")
endif()

install(TARGETS amr-map-manager
    BUNDLE DESTINATION .
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

endif() # BUILD_MAP_MANAGER

if(UNIX AND NOT APPLE AND BUILD_MAP_MANAGER)
    install(FILES packaging/linux/amr-map-manager.desktop
            DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
    install(FILES logo/svi_logo.svg
            DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps
            RENAME amr-map-manager.svg)

    set(CPACK_GENERATOR "DEB")
    set(CPACK_PACKAGE_NAME "amr-map-manager")
    set(CPACK_PACKAGE_VENDOR "SVI")
    set(CPACK_PACKAGE_CONTACT "shuang.zhu@svi.co.th")
    set(CPACK_PACKAGE_DESCRIPTION_SUMMARY
        "Manage, transfer, verify, switch, and preview SEER AMR maps")
    set(CPACK_PACKAGE_VERSION ${PROJECT_VERSION})
    set(CPACK_DEBIAN_PACKAGE_SECTION "utils")
    set(CPACK_DEBIAN_PACKAGE_PRIORITY "optional")
    set(CPACK_DEBIAN_PACKAGE_DEPENDS "qt6-qpa-plugins")
    set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
    set(CPACK_DEBIAN_FILE_NAME DEB-DEFAULT)
    set(CPACK_PACKAGING_INSTALL_PREFIX "/usr")
    set(CPACK_PACKAGE_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/dist/ubuntu")
    include(CPack)
endif()
