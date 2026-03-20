# Gun2CR Visual Effects Fix - Build Configuration
# Compiles gun2cr_hook.cpp and loader.cpp as part of dbghooks

# This file is included from parent dbghooks/CMakeLists.txt
# It adds source files to the main dbghooks target

# Gun2CR source files to compile
set(GUN2CR_SOURCES
    gun2cr_hook.cpp
    loader.cpp
)

# Append to parent dbghooks sources
list(APPEND DBGHOOKS_SOURCES ${GUN2CR_SOURCES})

# Also append the header files for IDE organization
list(APPEND DBGHOOKS_HEADERS
    gun2cr_fix/gun2cr_hook.h
    gun2cr_fix/loader.h
)

# Update parent scope variable
set(DBGHOOKS_SOURCES ${DBGHOOKS_SOURCES} PARENT_SCOPE)
set(DBGHOOKS_HEADERS ${DBGHOOKS_HEADERS} PARENT_SCOPE)

