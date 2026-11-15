cmake_minimum_required(VERSION 3.22)
project(macos-finder-linux VERSION 0.1.0 LANGUAGES CXX)

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(FATAL_ERROR "This file manager currently requires Linux (mount metadata and local file tags).")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_AUTOMOC ON)

find_package(Qt6 6.8 REQUIRED COMPONENTS
    Core Gui Widgets DBus Network Pdf Multimedia MultimediaWidgets Xml PrintSupport
)
find_package(Threads REQUIRED)
find_package(KF6KIO 6.2 REQUIRED)
find_package(KF6CoreAddons 6.2 REQUIRED)
find_package(KF6ItemViews 6.2 REQUIRED)
find_package(KF6Bookmarks 6.2 REQUIRED)
find_package(KF6WidgetsAddons 6.2 REQUIRED)
find_package(KF6JobWidgets 6.2 REQUIRED)
find_package(KF6Config 6.2 REQUIRED)
find_package(KF6WindowSystem 6.2 REQUIRED)
find_package(KF6I18n 6.2 REQUIRED)
find_package(KF6Service 6.2 REQUIRED)
find_package(KF6Solid 6.2 REQUIRED)
find_package(KF6FileMetaData 6.2 REQUIRED)
find_package(KF6Archive 6.2 REQUIRED)

add_executable(finder
    src/main.cpp
    src/browser.cpp src/browser.h
    src/preview.cpp src/preview.h
    src/places.cpp src/places.h
    src/search.cpp src/search.h
    src/tags.cpp src/tags.h
    src/settings.cpp src/settings.h
    src/preferences.cpp src/preferences.h
    src/scroll.cpp src/scroll.h
    src/quickactions.cpp src/quickactions.h
    src/smartfolder.cpp src/smartfolder.h
    src/archives.cpp src/archives.h
)
target_link_libraries(finder PRIVATE
    Qt6::Core Qt6::Gui Qt6::Widgets Qt6::DBus Qt6::Network
    Qt6::Pdf Qt6::Multimedia Qt6::MultimediaWidgets Qt6::Xml Qt6::PrintSupport
    Threads::Threads
    KF6::KIOCore KF6::KIOGui KF6::KIOWidgets KF6::KIOFileWidgets
    KF6::CoreAddons KF6::ItemViews KF6::Bookmarks KF6::WidgetsAddons
    KF6::JobWidgets KF6::ConfigCore KF6::WindowSystem KF6::I18n
    KF6::Service KF6::Solid KF6::FileMetaData KF6::Archive
)
target_compile_definitions(finder PRIVATE FINDER_PREVIEW_PRINT_SUPPORT=1)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(finder PRIVATE -Wall -Wextra)
endif()

include(GNUInstallDirs)
install(TARGETS finder RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")

include(CTest)
if(BUILD_TESTING)
    add_subdirectory(tests)
endif()
