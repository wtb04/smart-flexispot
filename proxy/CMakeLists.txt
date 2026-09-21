cmake_minimum_required(VERSION 3.16)

# The desk protocol and its UART driver are shared with the panel rather than
# copied: the frames, the CRC and the key repetition have one home, and the
# host tests that cover them cover both boards.
# Only the desk component, not the whole shelf: the panel's board support is
# for a part this board is not, and pulling it in makes the dependency solver
# go looking for a Tab5.
set(EXTRA_COMPONENT_DIRS "../components/loctek" "../components/deskproto")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(desk_proxy)
