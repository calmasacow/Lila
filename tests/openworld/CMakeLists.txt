# World Remix and site kits: their cores compiled in directly (with Terrain's readers, erosion and router, as the extension does), and
# the package driven through its commands. Fixtures are small synthetic files in data/ (make_fixtures.py); no network.
set(_ow ${CMAKE_SOURCE_DIR}/extensions/OpenWorld)
set(_terrain ${CMAKE_SOURCE_DIR}/extensions/Terrain)
add_executable(lila_openworld_tests main.cpp test_remix.cpp test_remix_commands.cpp test_sites.cpp
    ${_ow}/OsmDoc.cpp ${_ow}/RemixSpec.cpp ${_ow}/Warp.cpp ${_ow}/Remix.cpp ${_ow}/Features.cpp ${_ow}/Repair.cpp ${_ow}/Check.cpp ${_ow}/SitesLayout.cpp ${_ow}/SitesCoast.cpp ${_ow}/SitesLand.cpp ${_ow}/SitesCompound.cpp
    ${_terrain}/Core.cpp ${_terrain}/Erosion.cpp ${_terrain}/Inputs.cpp ${_terrain}/Projection.cpp ${_terrain}/Osm.cpp
    ${_terrain}/Fetch.cpp ${_terrain}/LandCover.cpp ${_terrain}/Stamps.cpp ${_terrain}/Brushes.cpp ${_terrain}/Shaping.cpp ${_terrain}/Painted.cpp)
set_source_files_properties(${_terrain}/Core.cpp ${_terrain}/Erosion.cpp ${_terrain}/Inputs.cpp ${_terrain}/Projection.cpp ${_terrain}/Osm.cpp
    ${_terrain}/Fetch.cpp ${_terrain}/LandCover.cpp ${_terrain}/Stamps.cpp ${_terrain}/Brushes.cpp ${_terrain}/Shaping.cpp ${_terrain}/Painted.cpp
    TARGET_DIRECTORY lila_openworld_tests PROPERTIES SKIP_UNITY_BUILD_INCLUSION ON)
target_include_directories(lila_openworld_tests PRIVATE ${_ow} ${_terrain})
find_package(miniz CONFIG REQUIRED)
target_link_libraries(lila_openworld_tests PRIVATE LilaUI doctest::doctest miniz::miniz)
add_dependencies(lila_openworld_tests lila_openworld_extension lila_terrain_extension)
target_compile_definitions(lila_openworld_tests PRIVATE
    LILA_TEST_OPENWORLD_PACKAGE="${CMAKE_BINARY_DIR}/bin/$<CONFIG>/extensions/OpenWorld.lilaext"
    LILA_TEST_TERRAIN_PACKAGE="${CMAKE_BINARY_DIR}/bin/$<CONFIG>/extensions/Terrain.lilaext"
    LILA_TEST_OPENWORLD_DATA="${CMAKE_CURRENT_SOURCE_DIR}/data")
add_test(NAME lila_openworld_tests COMMAND lila_openworld_tests)
