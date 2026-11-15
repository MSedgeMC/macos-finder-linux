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

add_executable(finder-preview-test preview.cpp ../src/preview.cpp ../src/preview.h ../src/tags.cpp ../src/preferences.cpp)
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

add_executable(finder-browser-metadata-test browser-metadata.cpp ../src/browser.cpp ../src/tags.cpp ../src/preferences.cpp ../src/scroll.cpp)
target_include_directories(finder-browser-metadata-test PRIVATE "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(finder-browser-metadata-test PRIVATE Qt6::Widgets Threads::Threads KF6::KIOCore KF6::KIOGui KF6::KIOWidgets KF6::KIOFileWidgets KF6::JobWidgets KF6::FileMetaData KF6::Service)
add_test(NAME finder-browser-metadata-test COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/run-headless.py" "$<TARGET_FILE:finder-browser-metadata-test>")
set_tests_properties(finder-browser-metadata-test PROPERTIES TIMEOUT 50 LABELS "unit;headless")

add_executable(finder-browser-arrangement-test browser-arrangement.cpp ../src/browser.cpp ../src/tags.cpp ../src/preferences.cpp ../src/scroll.cpp)
target_include_directories(finder-browser-arrangement-test PRIVATE "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(finder-browser-arrangement-test PRIVATE Qt6::Widgets Threads::Threads KF6::KIOCore KF6::KIOGui KF6::KIOWidgets KF6::KIOFileWidgets KF6::JobWidgets KF6::FileMetaData KF6::Service)
add_test(NAME finder-browser-arrangement-test COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/run-headless.py" "$<TARGET_FILE:finder-browser-arrangement-test>")
set_tests_properties(finder-browser-arrangement-test PROPERTIES TIMEOUT 60 LABELS "unit;headless")

add_executable(finder-archives-test archives.cpp ../src/archives.cpp ../src/archives.h)
target_include_directories(finder-archives-test PRIVATE "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(finder-archives-test PRIVATE Qt6::Widgets Threads::Threads KF6::Archive KF6::KIOCore KF6::KIOGui KF6::KIOWidgets)
add_test(NAME finder-archives-test COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/run-headless.py" "$<TARGET_FILE:finder-archives-test>")
set_tests_properties(finder-archives-test PROPERTIES TIMEOUT 90 LABELS "unit;headless")
