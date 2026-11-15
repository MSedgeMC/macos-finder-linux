add_executable(finder-search-test
    search.cpp
    ../src/search.cpp ../src/search.h
)
target_include_directories(finder-search-test PRIVATE "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(finder-search-test PRIVATE Qt6::Core Threads::Threads)

add_executable(finder-tags-test
    tags.cpp
    ../src/tags.cpp ../src/tags.h
)
target_include_directories(finder-tags-test PRIVATE "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(finder-tags-test PRIVATE Qt6::Core Qt6::Gui KF6::FileMetaData)

foreach(test_target IN ITEMS finder-search-test finder-tags-test)
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(${test_target} PRIVATE -Wall -Wextra)
    endif()
    add_test(NAME ${test_target}
        COMMAND "${CMAKE_COMMAND}" -E env
            --unset=DISPLAY
            --unset=WAYLAND_DISPLAY
            --unset=DBUS_SESSION_BUS_ADDRESS
            --unset=SESSION_MANAGER
            "$<TARGET_FILE:${test_target}>"
    )
    set_tests_properties(${test_target} PROPERTIES
        TIMEOUT 30
        LABELS "unit;headless"
    )
endforeach()
