find_package(Python3 REQUIRED COMPONENTS Interpreter)
add_executable(finder-search-test
    search.cpp
    ../src/search.cpp ../src/search.h
)
target_include_directories(finder-search-test PRIVATE "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(finder-search-test PRIVATE Qt6::Core Threads::Threads)

add_executable(finder-tags-test
    tags.cpp
    ../src/tags.cpp ../src/tags.h
    ../src/preferences.cpp ../src/preferences.h
)
target_include_directories(finder-tags-test PRIVATE "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(finder-tags-test PRIVATE Qt6::Core Qt6::Gui KF6::FileMetaData KF6::KIOCore)

foreach(test_target IN ITEMS finder-search-test finder-tags-test)
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(${test_target} PRIVATE -Wall -Wextra)
    endif()
    add_test(NAME ${test_target}
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/run-headless.py" "$<TARGET_FILE:${test_target}>"
    )
    set_tests_properties(${test_target} PROPERTIES
        TIMEOUT 30
        LABELS "unit;headless"
    )
endforeach()

add_executable(finder-scroll-test scroll.cpp ../src/scroll.cpp ../src/scroll.h)
target_include_directories(finder-scroll-test PRIVATE "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(finder-scroll-test PRIVATE Qt6::Widgets)
add_test(NAME finder-scroll-test COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/run-headless.py" "$<TARGET_FILE:finder-scroll-test>")
set_tests_properties(finder-scroll-test PROPERTIES TIMEOUT 30 LABELS "unit;headless")

add_executable(finder-preview-test preview.cpp ../src/preview.cpp ../src/preview.h ../src/tags.cpp ../src/preferences.cpp ../src/appearance.cpp ../src/appearance.h)
target_include_directories(finder-preview-test PRIVATE "${PROJECT_SOURCE_DIR}/src")
target_compile_definitions(finder-preview-test PRIVATE FINDER_PREVIEW_PRINT_SUPPORT=1)
target_link_libraries(finder-preview-test PRIVATE Qt6::Widgets Qt6::Pdf Qt6::Multimedia Qt6::MultimediaWidgets Qt6::PrintSupport KF6::KIOCore KF6::KIOGui KF6::KIOWidgets KF6::FileMetaData KF6::Service)
add_test(NAME finder-preview-test COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/run-headless.py" "$<TARGET_FILE:finder-preview-test>")
set_tests_properties(finder-preview-test PROPERTIES TIMEOUT 40 LABELS "unit;headless")

add_executable(finder-quickactions-test quickactions.cpp ../src/quickactions.cpp ../src/quickactions.h)
target_include_directories(finder-quickactions-test PRIVATE "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(finder-quickactions-test PRIVATE Qt6::Widgets Qt6::Pdf KF6::KIOCore KF6::KIOGui KF6::KIOWidgets)
add_test(NAME finder-quickactions-test COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/run-headless.py" "$<TARGET_FILE:finder-quickactions-test>")
set_tests_properties(finder-quickactions-test PROPERTIES TIMEOUT 60 LABELS "unit;headless")

add_executable(finder-smartfolder-test smartfolder.cpp ../src/smartfolder.cpp ../src/smartfolder.h)
target_include_directories(finder-smartfolder-test PRIVATE "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(finder-smartfolder-test PRIVATE Qt6::Core Threads::Threads)
target_link_options(finder-smartfolder-test PRIVATE -Wl,--wrap=open -Wl,--wrap=read -Wl,--wrap=fstat)
add_test(NAME finder-smartfolder-test COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/run-headless.py" "$<TARGET_FILE:finder-smartfolder-test>")
set_tests_properties(finder-smartfolder-test PROPERTIES TIMEOUT 30 LABELS "unit;headless")

add_executable(finder-browser-metadata-test browser-metadata.cpp ../src/browser.cpp ../src/tags.cpp ../src/preferences.cpp ../src/scroll.cpp ../src/appearance.cpp ../src/appearance.h)
target_include_directories(finder-browser-metadata-test PRIVATE "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(finder-browser-metadata-test PRIVATE Qt6::Widgets Threads::Threads KF6::KIOCore KF6::KIOGui KF6::KIOWidgets KF6::KIOFileWidgets KF6::JobWidgets KF6::FileMetaData KF6::Service)
add_test(NAME finder-browser-metadata-test COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/run-headless.py" "$<TARGET_FILE:finder-browser-metadata-test>")
set_tests_properties(finder-browser-metadata-test PROPERTIES TIMEOUT 50 LABELS "unit;headless")

add_executable(finder-browser-arrangement-test browser-arrangement.cpp ../src/browser.cpp ../src/tags.cpp ../src/preferences.cpp ../src/scroll.cpp ../src/appearance.cpp ../src/appearance.h)
target_include_directories(finder-browser-arrangement-test PRIVATE "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(finder-browser-arrangement-test PRIVATE Qt6::Widgets Threads::Threads KF6::KIOCore KF6::KIOGui KF6::KIOWidgets KF6::KIOFileWidgets KF6::JobWidgets KF6::FileMetaData KF6::Service)
add_test(NAME finder-browser-arrangement-test COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/run-headless.py" "$<TARGET_FILE:finder-browser-arrangement-test>")
set_tests_properties(finder-browser-arrangement-test PROPERTIES TIMEOUT 60 LABELS "unit;headless")

add_executable(finder-archives-test archives.cpp ../src/archives.cpp ../src/archives.h)
target_include_directories(finder-archives-test PRIVATE "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(finder-archives-test PRIVATE Qt6::Widgets Threads::Threads KF6::Archive KF6::KIOCore KF6::KIOGui KF6::KIOWidgets)
add_test(NAME finder-archives-test COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/run-headless.py" "$<TARGET_FILE:finder-archives-test>")
set_tests_properties(finder-archives-test PROPERTIES TIMEOUT 90 LABELS "unit;headless")

add_executable(finder-criteriaeditor-test criteriaeditor.cpp ../src/criteriaeditor.cpp ../src/criteriaeditor.h)
target_include_directories(finder-criteriaeditor-test PRIVATE "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(finder-criteriaeditor-test PRIVATE Qt6::Widgets)
add_test(NAME finder-criteriaeditor-test COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/run-headless.py" "$<TARGET_FILE:finder-criteriaeditor-test>")
set_tests_properties(finder-criteriaeditor-test PROPERTIES TIMEOUT 30 LABELS "unit;headless")

add_executable(finder-inspector-test inspector.cpp ../src/inspector.cpp ../src/inspector.h ../src/tags.cpp ../src/preferences.cpp)
target_include_directories(finder-inspector-test PRIVATE "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(finder-inspector-test PRIVATE Qt6::Widgets Threads::Threads KF6::FileMetaData KF6::KIOCore)
target_link_options(finder-inspector-test PRIVATE -Wl,--wrap=open -Wl,--wrap=fstat -Wl,--wrap=statx -Wl,--wrap=faccessat)
add_test(NAME finder-inspector-test COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/run-headless.py" "$<TARGET_FILE:finder-inspector-test>")
set_tests_properties(finder-inspector-test PROPERTIES TIMEOUT 40 LABELS "unit;headless")

add_executable(finder-appearance-test appearance.cpp ../src/appearance.cpp ../src/appearance.h ../src/browser.cpp ../src/browser.h ../src/places.cpp ../src/places.h ../src/preview.cpp ../src/preview.h ../src/settings.cpp ../src/settings.h ../src/tags.cpp ../src/preferences.cpp ../src/scroll.cpp)
target_include_directories(finder-appearance-test PRIVATE "${PROJECT_SOURCE_DIR}/src")
target_compile_definitions(finder-appearance-test PRIVATE FINDER_PREVIEW_PRINT_SUPPORT=1)
target_link_libraries(finder-appearance-test PRIVATE Qt6::Widgets Qt6::DBus Qt6::Network Qt6::Pdf Qt6::Multimedia Qt6::MultimediaWidgets Qt6::PrintSupport Threads::Threads KF6::KIOCore KF6::KIOGui KF6::KIOWidgets KF6::KIOFileWidgets KF6::CoreAddons KF6::ItemViews KF6::Bookmarks KF6::WidgetsAddons KF6::JobWidgets KF6::ConfigCore KF6::Service KF6::Solid KF6::FileMetaData)
add_test(NAME finder-appearance-test COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/run-headless.py" "$<TARGET_FILE:finder-appearance-test>")
set_tests_properties(finder-appearance-test PROPERTIES TIMEOUT 45 LABELS "unit;headless")

add_executable(finder-searchresults-test searchresults.cpp ../src/searchresults.cpp ../src/searchresults.h)
target_include_directories(finder-searchresults-test PRIVATE "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(finder-searchresults-test PRIVATE Qt6::Widgets KF6::KIOCore KF6::KIOGui)
add_test(NAME finder-searchresults-test COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/run-headless.py" "$<TARGET_FILE:finder-searchresults-test>")
set_tests_properties(finder-searchresults-test PROPERTIES TIMEOUT 40 LABELS "unit;headless")

add_executable(finder-gestures-test gestures.cpp ../src/gestures.cpp ../src/gestures.h)
target_include_directories(finder-gestures-test PRIVATE "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(finder-gestures-test PRIVATE Qt6::Widgets)
add_test(NAME finder-gestures-test COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/run-headless.py" "$<TARGET_FILE:finder-gestures-test>")
set_tests_properties(finder-gestures-test PROPERTIES TIMEOUT 30 LABELS "unit;headless")

add_executable(finder-taboverview-test taboverview.cpp ../src/taboverview.cpp ../src/taboverview.h)
target_include_directories(finder-taboverview-test PRIVATE "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(finder-taboverview-test PRIVATE Qt6::Widgets)
add_test(NAME finder-taboverview-test COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/run-headless.py" "$<TARGET_FILE:finder-taboverview-test>")
set_tests_properties(finder-taboverview-test PROPERTIES TIMEOUT 30 LABELS "unit;headless")

add_executable(finder-browser-folder-test browser-folder.cpp ../src/browser.cpp ../src/browser.h ../src/tags.cpp ../src/preferences.cpp ../src/scroll.cpp ../src/appearance.cpp ../src/appearance.h)
target_include_directories(finder-browser-folder-test PRIVATE "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(finder-browser-folder-test PRIVATE Qt6::Widgets Threads::Threads KF6::KIOCore KF6::KIOGui KF6::KIOWidgets KF6::KIOFileWidgets KF6::JobWidgets KF6::FileMetaData KF6::Service)
add_test(NAME finder-browser-folder-test COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/run-headless.py" "$<TARGET_FILE:finder-browser-folder-test>")
set_tests_properties(finder-browser-folder-test PROPERTIES TIMEOUT 60 LABELS "unit;headless")

foreach(browser_target IN ITEMS finder-browser-metadata-test finder-browser-arrangement-test finder-browser-folder-test finder-appearance-test)
    target_sources(${browser_target} PRIVATE ../src/batchrename.cpp ../src/batchrename.h ../src/filedetails.cpp ../src/filedetails.h)
endforeach()

add_executable(finder-batchrename-test batchrename.cpp ../src/batchrename.cpp ../src/batchrename.h)
target_include_directories(finder-batchrename-test PRIVATE "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(finder-batchrename-test PRIVATE Qt6::Widgets KF6::KIOCore KF6::KIOGui KF6::KIOWidgets)
add_test(NAME finder-batchrename-test COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/run-headless.py" "$<TARGET_FILE:finder-batchrename-test>")
set_tests_properties(finder-batchrename-test PROPERTIES TIMEOUT 90 LABELS "unit;headless")

add_executable(finder-filedetails-test filedetails.cpp ../src/filedetails.cpp ../src/filedetails.h)
target_include_directories(finder-filedetails-test PRIVATE "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(finder-filedetails-test PRIVATE Qt6::Widgets Threads::Threads)
target_link_options(finder-filedetails-test PRIVATE -Wl,--wrap=open -Wl,--wrap=openat -Wl,--wrap=fgetxattr -Wl,--wrap=fsetxattr -Wl,--wrap=fremovexattr)
add_test(NAME finder-filedetails-test COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/run-headless.py" "$<TARGET_FILE:finder-filedetails-test>")
set_tests_properties(finder-filedetails-test PROPERTIES TIMEOUT 60 LABELS "unit;headless")

add_executable(finder-infohost-test infohost.cpp ../src/infohost.cpp ../src/infohost.h ../src/filedetails.cpp ../src/filedetails.h)
target_include_directories(finder-infohost-test PRIVATE "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(finder-infohost-test PRIVATE Qt6::Widgets Threads::Threads KF6::KIOCore KF6::KIOWidgets KF6::WidgetsAddons)
target_link_options(finder-infohost-test PRIVATE -Wl,--wrap=fgetxattr)
add_test(NAME finder-infohost-test COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/run-headless.py" "$<TARGET_FILE:finder-infohost-test>")
set_tests_properties(finder-infohost-test PROPERTIES TIMEOUT 40 LABELS "unit;headless")

add_executable(finder-browser-performance browser-performance.cpp ../src/browser.cpp ../src/browser.h ../src/tags.cpp ../src/preferences.cpp ../src/scroll.cpp ../src/appearance.cpp ../src/appearance.h ../src/batchrename.cpp ../src/batchrename.h ../src/filedetails.cpp ../src/filedetails.h)
target_include_directories(finder-browser-performance PRIVATE "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(finder-browser-performance PRIVATE Qt6::Widgets Threads::Threads KF6::KIOCore KF6::KIOGui KF6::KIOWidgets KF6::KIOFileWidgets KF6::JobWidgets KF6::FileMetaData KF6::Service)
# Diagnostic executable: timings are measured, never asserted as macOS parity.

add_executable(finder-browser-targets-test browser-targets.cpp ../src/browser.cpp ../src/browser.h ../src/tags.cpp ../src/preferences.cpp ../src/scroll.cpp ../src/appearance.cpp ../src/appearance.h ../src/batchrename.cpp ../src/batchrename.h ../src/filedetails.cpp ../src/filedetails.h)
target_include_directories(finder-browser-targets-test PRIVATE "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(finder-browser-targets-test PRIVATE Qt6::Widgets Threads::Threads KF6::KIOCore KF6::KIOGui KF6::KIOWidgets KF6::KIOFileWidgets KF6::JobWidgets KF6::FileMetaData KF6::Service)
add_test(NAME finder-browser-targets-test COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/run-headless.py" "$<TARGET_FILE:finder-browser-targets-test>")
set_tests_properties(finder-browser-targets-test PROPERTIES TIMEOUT 40 LABELS "unit;headless")

add_executable(finder-directory-state-test directory-state.cpp ../src/browser.cpp ../src/browser.h ../src/tags.cpp ../src/preferences.cpp ../src/scroll.cpp ../src/appearance.cpp ../src/appearance.h ../src/batchrename.cpp ../src/batchrename.h ../src/filedetails.cpp ../src/filedetails.h)
target_include_directories(finder-directory-state-test PRIVATE "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(finder-directory-state-test PRIVATE Qt6::Widgets Threads::Threads KF6::KIOCore KF6::KIOGui KF6::KIOWidgets KF6::KIOFileWidgets KF6::JobWidgets KF6::FileMetaData KF6::Service)
add_test(NAME finder-directory-state-test COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/run-headless.py" "$<TARGET_FILE:finder-directory-state-test>")
set_tests_properties(finder-directory-state-test PROPERTIES TIMEOUT 40 LABELS "unit;headless")

add_executable(finder-alerts-controls-test alerts-controls.cpp ../src/appearance.cpp ../src/appearance.h)
target_include_directories(finder-alerts-controls-test PRIVATE "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(finder-alerts-controls-test PRIVATE Qt6::Widgets KF6::KIOCore KF6::KIOWidgets)
add_test(NAME finder-alerts-controls-test COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/run-headless.py" "$<TARGET_FILE:finder-alerts-controls-test>")
set_tests_properties(finder-alerts-controls-test PROPERTIES TIMEOUT 30 LABELS "unit;headless")

add_executable(finder-browser-layout-test browser-layout.cpp ../src/browser.cpp ../src/browser.h ../src/tags.cpp ../src/preferences.cpp ../src/scroll.cpp ../src/appearance.cpp ../src/appearance.h ../src/batchrename.cpp ../src/batchrename.h ../src/filedetails.cpp ../src/filedetails.h)
target_include_directories(finder-browser-layout-test PRIVATE "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(finder-browser-layout-test PRIVATE Qt6::Widgets Threads::Threads KF6::KIOCore KF6::KIOGui KF6::KIOWidgets KF6::KIOFileWidgets KF6::JobWidgets KF6::FileMetaData KF6::Service)
target_compile_options(finder-browser-layout-test PRIVATE -Wall -Wextra)
add_test(NAME finder-browser-layout-test COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/run-headless.py" "$<TARGET_FILE:finder-browser-layout-test>")
set_tests_properties(finder-browser-layout-test PROPERTIES TIMEOUT 40 LABELS "unit;headless")

add_executable(finder-viewer-test viewer.cpp
    ../src/viewer.cpp ../src/viewer.h ../src/windowframe.cpp ../src/gestures.cpp ../src/gestures.h ../src/preview.cpp ../src/preview.h
    ../src/quickactions.cpp ../src/quickactions.h ../src/appearance.cpp ../src/appearance.h
    ../src/tags.cpp ../src/preferences.cpp)
target_include_directories(finder-viewer-test PRIVATE "${PROJECT_SOURCE_DIR}/src")
target_compile_definitions(finder-viewer-test PRIVATE FINDER_PREVIEW_PRINT_SUPPORT=1)
target_link_libraries(finder-viewer-test PRIVATE Qt6::Widgets Qt6::Pdf Qt6::Multimedia Qt6::MultimediaWidgets Qt6::PrintSupport
    KF6::KIOCore KF6::KIOGui KF6::KIOWidgets KF6::FileMetaData KF6::Service)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(finder-viewer-test PRIVATE -Wall -Wextra)
endif()
add_test(NAME finder-viewer-test COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/run-headless.py" "$<TARGET_FILE:finder-viewer-test>")
set_tests_properties(finder-viewer-test PROPERTIES TIMEOUT 40 LABELS "unit;headless")

add_test(NAME finder-preview-install-test COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/install-preview.py")
set_tests_properties(finder-preview-install-test PROPERTIES TIMEOUT 15 LABELS "unit;headless")
