
file(GLOB shader_files RELATIVE ${CMAKE_CURRENT_SOURCE_DIR} *.glsl)

add_custom_target(
    shaders
    COMMAND ${CMAKE_COMMAND}
        -DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}
        -DDESTINATION_DIR=${CMAKE_BINARY_DIR}/shaders
        -DFILES="${shader_files}"
        -P ${CMAKE_SOURCE_DIR}/cmake/shaders.cmake
    COMMENT "Creating the files to include the GLSL shaders"
)

