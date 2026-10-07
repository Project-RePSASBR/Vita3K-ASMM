add_library(
    net
    STATIC
    include/net/epoll.h
    include/net/functions.h
    include/net/p2pmux.h
    include/net/psas_connector.h
    include/net/state.h
    include/net/types.h
    include/net/socket.h
    src/net.cpp
    src/epoll.cpp
    src/posixsocket.cpp
    src/p2psocket.cpp
    src/p2pmux.cpp
    src/psas_connector.cpp
)

target_include_directories(net PUBLIC include)
target_link_libraries(net PUBLIC emuenv mem np)
target_link_libraries(net PRIVATE config io)
if (WIN32)
    target_link_libraries(net PRIVATE winsock)
endif()

if(NOT ANDROID)
    add_executable(
        net-tests
        tests/p2pmux_tests.cpp
        tests/psas_connector_tests.cpp
    )

    target_link_libraries(net-tests PRIVATE net googletest util)
    if (WIN32)
        target_link_libraries(net-tests PRIVATE winsock)
    endif()
    add_test(NAME net COMMAND net-tests)
endif()
