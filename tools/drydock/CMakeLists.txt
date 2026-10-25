# drydock: recompiles a game from the player's disc (see build.hpp)
add_executable(drydock main.cpp inspect.cpp)
target_link_libraries(drydock PRIVATE drydock-build)
