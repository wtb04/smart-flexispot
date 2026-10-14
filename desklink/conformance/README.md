# Conformance

`fixtures.json` is what the panel's own envelope and protocol code says, so a Desk Link can be
tested against it without a panel: sealed messages it must open, ones it must refuse, the
freshness rule, and the panel's message bodies. The tool also opens, seals and reads what a
Desk Link under test produces.

    cmake -S . -B build && cmake --build build
    ./build/conformance fixtures > fixtures.json     # after the panel's laptop component changes

It is built from `components/laptop/` with mbedTLS, as the panel has it; CMake fetches mbedTLS
and cJSON.
