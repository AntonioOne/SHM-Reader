cmake_minimum_required(VERSION 3.10)
project(ShmRgbViewer CXX)

set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

find_package(OpenCV REQUIRED COMPONENTS core imgproc imgcodecs highgui)

add_executable(ShmRgbViewer main.cpp single_instance.cpp)
target_include_directories(ShmRgbViewer PRIVATE ${OpenCV_INCLUDE_DIRS})
target_link_libraries(ShmRgbViewer PRIVATE ${OpenCV_LIBS})
