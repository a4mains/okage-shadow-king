# Inject our optional research target after Play!'s own targets, without editing
# the pinned upstream checkout. This file is passed as CMAKE_PROJECT_Play_INCLUDE.
if(OKAGE_REFERENCE_VULKAN)
    enable_language(OBJCXX)
    # Languages and subdirectories cannot be added from the deferred callback.
    # PlayCore's project hook runs after the root platform setup, before its
    # dependency guards, so the optional graphics targets can be shared normally.
    set(CMAKE_PROJECT_PlayCore_INCLUDE "${CMAKE_CURRENT_LIST_DIR}/Graphics.cmake")
endif()
cmake_language(EVAL CODE "cmake_language(DEFER DIRECTORY \"${CMAKE_SOURCE_DIR}\" CALL include \"${CMAKE_CURRENT_LIST_DIR}/CMakeLists.txt\")")
