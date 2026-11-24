# Open World, built as <bin>/<config>/extensions/OpenWorld.lilaext next to lila.exe, which loads it by default.
# Terrain's readers, erosion and router are compiled in (as its tests do), so a remix works on real data before any
# terrain exists; extensions don't link each other.
set(_terrain ${CMAKE_SOURCE_DIR}/extensions/Terrain)
set(_terrain_sources ${_terrain}/Core.cpp ${_terrain}/Erosion.cpp ${_terrain}/Inputs.cpp ${_terrain}/Projection.cpp ${_terrain}/Osm.cpp
    ${_terrain}/Fetch.cpp ${_terrain}/LandCover.cpp ${_terrain}/Stamps.cpp ${_terrain}/Brushes.cpp ${_terrain}/Shaping.cpp ${_terrain}/Painted.cpp)
lila_add_extension(lila_openworld_extension ID OpenWorld MANIFEST extension.json UI
    SOURCES OpenWorld.cpp OpenWorld.h RemixNode.h OsmDoc.cpp OsmDoc.h RemixSpec.cpp RemixSpec.h Warp.cpp Warp.h Remix.cpp Remix.h RemixWork.h
            Features.cpp Repair.cpp Check.cpp RemixNode.cpp RemixCommands.cpp RemixUi.cpp
            Zones.cpp Zones.h ZoneScene.h ZoneCommands.cpp ZoneUi.cpp
            ${_terrain_sources})
set_source_files_properties(${_terrain_sources} PROPERTIES SKIP_UNITY_BUILD_INCLUSION ON)
target_include_directories(lila_openworld_extension PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${_terrain})
find_package(miniz CONFIG REQUIRED)
target_link_libraries(lila_openworld_extension PRIVATE miniz::miniz)
foreach(_host lila lilabatch)
    if(TARGET ${_host})
        add_dependencies(${_host} lila_openworld_extension)
    endif()
endforeach()
