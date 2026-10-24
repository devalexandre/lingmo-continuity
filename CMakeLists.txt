cmake_minimum_required(VERSION 3.16)
project(lingmo-continuity VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

include(GNUInstallDirs)

option(BUILD_TESTING "Build the tests" ON)

find_package(Qt6 REQUIRED COMPONENTS Core Gui Network DBus Qml Quick LinguistTools)
find_package(PkgConfig REQUIRED)
pkg_check_modules(XCB REQUIRED IMPORTED_TARGET xcb xcb-xfixes)

# Everything but main(): shared by the daemon and the tests
add_library(continuity-core STATIC
    src/deskflowconfig.cpp
    src/deskflowrunner.cpp
    src/discovery.cpp
    src/channel.cpp
    src/identity.cpp
    src/store.cpp
    src/firewall.cpp
    src/dragwatcher.cpp
    src/feedback.cpp
    src/service.cpp
)
target_include_directories(continuity-core PUBLIC src)
target_link_libraries(continuity-core PUBLIC
    Qt6::Core Qt6::Gui Qt6::Network Qt6::DBus Qt6::Qml Qt6::Quick PkgConfig::XCB)

add_executable(lingmo-continuityd
    src/main.cpp
    qml.qrc
)
target_link_libraries(lingmo-continuityd PRIVATE continuity-core)

set(TRANSLATIONS_DIR ${CMAKE_INSTALL_FULL_DATADIR}/lingmo-continuity/translations)
target_compile_definitions(continuity-core PUBLIC
    TRANSLATIONS_DIR="${TRANSLATIONS_DIR}"
    LINGMO_CONTINUITY_VERSION="${PROJECT_VERSION}"
)

# lrelease only: building never rewrites the .ts sources
qt_add_translation(QM_FILES translations/lingmo-continuity_pt_BR.ts)
add_custom_target(translations ALL DEPENDS ${QM_FILES})
install(FILES ${QM_FILES} DESTINATION ${TRANSLATIONS_DIR})

install(TARGETS lingmo-continuityd RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
# Started with the Lingmo session (lingmo-session reads XDG autostart); it quits
# right away when the feature is off
install(FILES data/lingmo-continuityd.desktop DESTINATION ${CMAKE_INSTALL_FULL_SYSCONFDIR}/xdg/autostart)
# Settings talks to it over D-Bus, which starts it when needed
install(FILES data/com.lingmo.Continuity.service DESTINATION ${CMAKE_INSTALL_DATADIR}/dbus-1/services)
install(FILES data/com.lingmo.Continuity.xml DESTINATION ${CMAKE_INSTALL_DATADIR}/dbus-1/interfaces)

if(BUILD_TESTING)
    enable_testing()
    find_package(Qt6 REQUIRED COMPONENTS Test)
    add_executable(tst_continuity tests/tst_continuity.cpp)
    target_link_libraries(tst_continuity PRIVATE continuity-core Qt6::Test)
    add_test(NAME tst_continuity COMMAND tst_continuity)
    # Helper for trying the edge file drag on Xvfb (not a test by itself)
    add_executable(dragsource tests/dragsource.cpp)
    target_link_libraries(dragsource PRIVATE Qt6::Gui)
    set_tests_properties(tst_continuity PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen;LC_ALL=C")
endif()
