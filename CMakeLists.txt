cmake_minimum_required(VERSION 3.20)
project(descriptor_heap_repro LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Vulkan REQUIRED)
find_program(GLSLC_EXECUTABLE glslc REQUIRED)

set(SHADER_SRC_DIR ${CMAKE_SOURCE_DIR}/shaders)
set(SHADER_BIN_DIR ${CMAKE_BINARY_DIR}/shaders)
file(MAKE_DIRECTORY ${SHADER_BIN_DIR})

set(SHADER_SPV_FILES "")
foreach(SHADER_NAME heap_constant.comp heap_dynamic.comp heap_pushdata.comp heap_pushdata_nonzero.comp
                    fullscreen.vert frag_slot0.frag frag_slot1.frag
                    frag_slot0_dynamic.frag)
    set(SPV_FILE ${SHADER_BIN_DIR}/${SHADER_NAME}.spv)
    add_custom_command(
        OUTPUT ${SPV_FILE}
        COMMAND ${GLSLC_EXECUTABLE} --target-env=vulkan1.4
                ${SHADER_SRC_DIR}/${SHADER_NAME} -o ${SPV_FILE}
        DEPENDS ${SHADER_SRC_DIR}/${SHADER_NAME}
        COMMENT "Compiling ${SHADER_NAME}"
        VERBATIM
    )
    list(APPEND SHADER_SPV_FILES ${SPV_FILE})
endforeach()

add_custom_target(shaders DEPENDS ${SHADER_SPV_FILES})

# One binary only: running the wrong one and concluding "it passes" is a
# very easy mistake to make with a bug repro.
add_executable(descriptor_heap_repro
    main.cpp
    compute_controls.cpp
    graphics_repro.cpp
)
add_dependencies(descriptor_heap_repro shaders)
target_link_libraries(descriptor_heap_repro PRIVATE Vulkan::Vulkan)
target_compile_definitions(descriptor_heap_repro PRIVATE SHADER_DIR="${SHADER_BIN_DIR}")
