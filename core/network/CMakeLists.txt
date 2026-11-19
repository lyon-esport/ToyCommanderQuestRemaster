target_sources(${PROJECT_NAME} PRIVATE
        alienfnt_modem.cpp
        alienfnt_modem.h
        dns.cpp
        ggpo.cpp
        ggpo.h
        miniupnp.cpp
        miniupnp.h
        naomi_network.cpp
        naomi_network.h
        net_handshake.cpp
        net_handshake.h
        net_platform.h
        tcvr_inet.cpp
        tcvr_inet.h
        tcvr_inet_proto.cpp
        tcvr_inet_proto.h
        ${CMAKE_CURRENT_SOURCE_DIR}/../deps/monocypher/monocypher.c
        tcvr_lobby.cpp
        tcvr_lobby.h
        tcvr_match.cpp
        tcvr_match.h
        output.cpp
        output.h
        picoppp.cpp
        picoppp.h
        netservice.cpp
        netservice.h
        dcnet.cpp
        dcnet.h
        ice.cpp
        ice.h
        ifaddrs.c)

# tcvr: internet play's secure random numbers (BCryptGenRandom)
if(WIN32)
    target_link_libraries(${PROJECT_NAME} PRIVATE bcrypt)
endif()
