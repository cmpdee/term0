cmake_minimum_required(VERSION 3.21)
project(term0 VERSION 0.7.5 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

# Qt 6.2 keeps the project usable on older LTS Linux distributions while all
# APIs used by term0 are available there.
find_package(Qt6 6.2 REQUIRED COMPONENTS Widgets SerialPort Concurrent)

qt_add_executable(term0
    main.cpp
    MainWindow.h
    MainWindow.cpp
    SerialWorker.h
    SerialWorker.cpp
    resources.qrc
)

target_link_libraries(term0 PRIVATE
    Qt6::Widgets
    Qt6::SerialPort
    Qt6::Concurrent
)

if(MSVC)
    target_compile_options(term0 PRIVATE /W4)
else()
    target_compile_options(term0 PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(WIN32)
    target_sources(term0 PRIVATE term0.rc)
    set_target_properties(term0 PROPERTIES WIN32_EXECUTABLE TRUE)
endif()

if(APPLE)
    target_sources(term0 PRIVATE assets/term0.icns)
    set_source_files_properties(assets/term0.icns PROPERTIES
        MACOSX_PACKAGE_LOCATION "Resources"
    )
    set_target_properties(term0 PROPERTIES
        MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_ICON_FILE term0.icns
    )
endif()

include(GNUInstallDirs)
install(TARGETS term0 RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
                        BUNDLE DESTINATION .)
if(UNIX AND NOT APPLE)
    install(FILES term0.desktop
            DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
    install(FILES assets/term0.png
            DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/1024x1024/apps
            RENAME term0.png)
endif()
