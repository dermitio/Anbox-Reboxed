find_package(Boost COMPONENTS filesystem)

include_directories(
    .
    ${Boost_INCLUDE_DIRS}
)

add_library(xdg STATIC xdg.cpp)
set_property(TARGET xdg PROPERTY CXX_STANDARD 17)
target_link_libraries(xdg ${Boost_LIBRARIES})

if(BUILD_TESTING)
    find_package(Boost COMPONENTS unit_test_framework REQUIRED)
    add_definitions(-DBOOST_TEST_DYN_LINK -DBOOST_TEST_MAIN -DBOOST_TEST_MODULE=xdg)
    add_executable(xdg_test xdg_test.cpp)
    set_property(TARGET xdg_test PROPERTY CXX_STANDARD 17)
    target_link_libraries(xdg_test xdg ${Boost_LIBRARIES})

    add_test(xdg_test xdg_test)
endif()
