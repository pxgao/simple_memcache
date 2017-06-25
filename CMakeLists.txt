cmake_minimum_required (VERSION 2.6)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -std=c++11")




project (objserver)
add_executable(objserver objservermain.cc objworker.cc objserver.cc log.cc)

target_link_libraries(objserver pthread)
target_link_libraries(objserver boost_log)
target_link_libraries(objserver boost_thread)
target_link_libraries(objserver boost_system)
target_link_libraries(objserver rt)

