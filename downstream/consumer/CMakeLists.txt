cmake_minimum_required(VERSION 3.21)

project(LoadSheddingConsumer LANGUAGES CXX)

# An independent consumer: it knows nothing about the Load Shedding build tree and
# finds the package only through CMAKE_PREFIX_PATH.
find_package(LoadShedding 1.0.0 REQUIRED CONFIG)

add_executable(consumer main.cpp)
target_link_libraries(consumer PRIVATE LoadShedding::load_shedding)
set_target_properties(consumer PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)

add_custom_target(run_consumer COMMAND consumer DEPENDS consumer)
