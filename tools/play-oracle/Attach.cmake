# Inject our optional research target after Play!'s own targets, without editing
# the pinned upstream checkout. This file is passed as CMAKE_PROJECT_Play_INCLUDE.
cmake_language(EVAL CODE "cmake_language(DEFER DIRECTORY \"${CMAKE_SOURCE_DIR}\" CALL include \"${CMAKE_CURRENT_LIST_DIR}/CMakeLists.txt\")")
