# edge_bench – konsolowa aplikacja Qt (tylko Qt Core) do pomiarow
add_executable(edge_bench main.cpp)
target_link_libraries(edge_bench PRIVATE edge_core Qt6::Core)

# Informacje o buildzie trafiaja do meta.json kazdego pomiaru
string(TOUPPER "${CMAKE_BUILD_TYPE}" _bt)
target_compile_definitions(edge_bench PRIVATE
    EDGE_BUILD_TYPE="${CMAKE_BUILD_TYPE}"
    EDGE_CXX_FLAGS="${CMAKE_CXX_FLAGS} ${CMAKE_CXX_FLAGS_${_bt}}"
    EDGE_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
)
