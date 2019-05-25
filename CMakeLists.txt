cmake_minimum_required(VERSION 3.13)
project(indexation_part_3)

set(CMAKE_CXX_STANDARD 11)
SET(PACKAGE "libzip")

find_package(ICU REQUIRED COMPONENTS uc i18n)
find_package(Boost COMPONENTS system locale REQUIRED)
find_package(Boost COMPONENTS filesystem REQUIRED)
find_package(Threads REQUIRED)
find_package(TBB REQUIRED tbb)

FIND_PACKAGE(ZLIB REQUIRED)
INCLUDE_DIRECTORIES(${ZLIB_INCLUDE_DIR})
set(CMAKE_REQUIRED_INCLUDES ${ZLIB_INCLUDE_DIR})

IF (Boost_FOUND)
    INCLUDE_DIRECTORIES(${Boost_INCLUDE_DIRS})
    LINK_DIRECTORIES(${Boost_LIBRARY_DIRS})
ENDIF (Boost_FOUND)


include_directories(.)

add_executable(indexation_part_3 main.cpp)

target_link_libraries(indexation_part_3 ${Boost_LIBRARIES})
target_link_libraries(indexation_part_3 Threads::Threads)
target_link_libraries(indexation_part_3 ${ICU_LIBRARIES})
target_link_libraries(indexation_part_3 ${TBB_IMPORTED_TARGETS})
target_link_libraries(indexation_part_3 "/usr/local/opt/libzip/lib/libzip.dylib")