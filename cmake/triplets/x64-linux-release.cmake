include("${CMAKE_CURRENT_LIST_DIR}/x64-linux.cmake")
# Release packages need no second copy of every dependency's Debug build.
set(VCPKG_BUILD_TYPE release)
