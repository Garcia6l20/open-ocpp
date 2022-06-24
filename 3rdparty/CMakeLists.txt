
# Rapidjson is an header only library
add_library(rapidjson INTERFACE)
if("${CMAKE_CXX_STANDARD}" GREATER_EQUAL 17)
    # use of std::iterator is deprecated since c++ 17
    target_compile_definitions(rapidjson INTERFACE -DRAPIDJSON_NOMEMBERITERATORCLASS=1)
endif()
target_include_directories(rapidjson INTERFACE rapidjson/include)

# Doctest is an header only on library
add_library(doctest INTERFACE)
target_include_directories(doctest INTERFACE doctest/doctest)

add_subdirectory(libwebsockets)
