# edge_cli – program terminalowy do recznych prob (menu albo opcje)
add_executable(edge_cli main.cpp)
target_link_libraries(edge_cli PRIVATE edge_core)
