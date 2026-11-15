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
