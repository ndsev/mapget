# Stage Markdown for the mapget-mcp-docs ALL target created by McpHelp.cmake.
# CMake runs this script with -P during the build. REGISTRY names the generated
# JSON object mapping component names to source directories; DESTINATION is the
# build's mcp-help directory, which is packaged beside the executable/webapp.
# Rescanning at build time picks up added and deleted documents without reconfigure.
if(NOT REGISTRY OR NOT DESTINATION)
  message(FATAL_ERROR "MCP help staging requires REGISTRY and DESTINATION")
endif()

file(READ "${REGISTRY}" registry)

# Replace the staging tree so deleted source pages cannot survive in a package.
file(REMOVE_RECURSE "${DESTINATION}")
file(MAKE_DIRECTORY "${DESTINATION}")

string(JSON count LENGTH "${registry}")
if(count GREATER 0)
  math(EXPR last "${count} - 1")
  foreach(index RANGE ${last})
    string(JSON component MEMBER "${registry}" ${index})
    string(JSON source GET "${registry}" "${component}")

    # Preserve component and relative paths for help section identity and links.
    file(GLOB_RECURSE documents RELATIVE "${source}" "${source}/*.md")
    foreach(document IN LISTS documents)
      get_filename_component(parent "${document}" DIRECTORY)
      file(MAKE_DIRECTORY "${DESTINATION}/${component}/${parent}")
      configure_file("${source}/${document}" "${DESTINATION}/${component}/${document}" COPYONLY)
    endforeach()
  endforeach()
endif()
