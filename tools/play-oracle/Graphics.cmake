if(NOT TARGET gsh_vulkan)
    add_subdirectory("${CMAKE_SOURCE_DIR}/Source/gs/GSH_Vulkan" "${CMAKE_BINARY_DIR}/okage-vulkan")
    # GSHandler's layout changes with debugger support. Match PlayCore's ABI.
    target_compile_definitions(gsh_vulkan PUBLIC DEBUGGER_INCLUDED=1)
endif()
