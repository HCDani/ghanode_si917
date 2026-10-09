# Define minimal required version of CMake.
cmake_minimum_required(VERSION "3.25")

# Project definition
project(
	wifi_embedded_mqtt_client_soc
	VERSION 1.0
	LANGUAGES C CXX ASM
)

# Include the definition of the slc_wifi_embedded_mqtt_client_soc target,
# which contains the content of the SLC project
include(wifi_embedded_mqtt_client_soc.cmake)

add_executable(wifi_embedded_mqtt_client_soc
    # Add additional sources here
)

target_include_directories(wifi_embedded_mqtt_client_soc PUBLIC
    # Add additional include paths here
)

target_compile_definitions(wifi_embedded_mqtt_client_soc PUBLIC
    # Add additional macros here
)

target_compile_options(wifi_embedded_mqtt_client_soc PUBLIC
    # Set additional compiler flags here
)

target_link_options(wifi_embedded_mqtt_client_soc PUBLIC
    # Set additional linker flags here
)

# Link with the content defined in the SLC project
target_link_libraries(wifi_embedded_mqtt_client_soc PRIVATE
    slc
)

# Include managed project content if available
include(wifi_embedded_mqtt_client_soc_project.cmake OPTIONAL RESULT_VARIABLE managed_project)
if(managed_project)
    message(STATUS "Using managed project content from ${managed_project}")
endif()

# Force the gcc linker command
set_target_properties(wifi_embedded_mqtt_client_soc PROPERTIES LINKER_LANGUAGE C)

# The SiWx917 bootloader image is created by the slpb post-build (create_rps).
# Extra hex/s37/bin copies only clutter the Silicon Labs flash file picker.

# Run post-build pipeline to perform additional post-processing
if(post_build_command)
add_custom_command(TARGET wifi_embedded_mqtt_client_soc
    POST_BUILD
    WORKING_DIRECTORY ${CMAKE_CURRENT_LIST_DIR}/..
    COMMAND ${post_build_command}
)
endif()