# tcvr-server: the internet meeting point for Toy Commander VR multiplayer (see README.md here).
# Standalone: it needs only this repository's core/network/tcvr_inet_proto.*, Monocypher and asio.
#   cmake -S tcvr/server -B build-server -DCMAKE_BUILD_TYPE=Release && cmake --build build-server
cmake_minimum_required(VERSION 3.16)
project(tcvr_server CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(REPO ${CMAKE_CURRENT_SOURCE_DIR}/../..)
option(TCVR_INET_PEER "Also build tcvr-inet-peer, the test player (needs the lobby code)" ON)

add_library(tcvr_inet_proto STATIC
	${REPO}/core/network/tcvr_inet_proto.cpp
	${REPO}/core/deps/monocypher/monocypher.c)
target_include_directories(tcvr_inet_proto PUBLIC ${REPO}/core ${REPO}/core/deps ${REPO}/core/deps/asio/asio/include)
target_compile_definitions(tcvr_inet_proto PUBLIC ASIO_STANDALONE)
if(WIN32)
	target_compile_definitions(tcvr_inet_proto PUBLIC _WIN32_WINNT=0x0601 WIN32_LEAN_AND_MEAN NOMINMAX)
	target_link_libraries(tcvr_inet_proto PUBLIC ws2_32 mswsock bcrypt iphlpapi)
else()
	find_package(Threads REQUIRED)
	target_link_libraries(tcvr_inet_proto PUBLIC Threads::Threads)
endif()

add_executable(tcvr-server tcvr_server.cpp)
target_link_libraries(tcvr-server PRIVATE tcvr_inet_proto)
if(MINGW)
	target_link_options(tcvr-server PRIVATE -static)
endif()

if(TCVR_INET_PEER)
	add_executable(tcvr-inet-peer tcvr_inet_peer.cpp
		${REPO}/core/network/tcvr_inet.cpp
		${REPO}/core/network/tcvr_lobby.cpp)
	target_link_libraries(tcvr-inet-peer PRIVATE tcvr_inet_proto)
	if(MINGW)
		target_link_options(tcvr-inet-peer PRIVATE -static)
	endif()
endif()
