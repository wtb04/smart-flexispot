# Desk Link

A menu bar app that shares what plays on this Mac with the desk panel
([smart-flexispot](https://github.com/wtb04/smart-flexispot)): the title, the
artist, the cover and where it is, from anything macOS shows under Now Playing,
YouTube in Safari as much as Spotify. The panel's media card then plays, pauses,
seeks and skips it.

Its menu turns sharing on and off, says what it shares and whether the panel
answers, and sets the panel's address and key and whether it opens at login.

## Build

    git submodule update --init
    make install      # builds, copies to /Applications and starts it

Needs Xcode's command line tools and CMake. The panel's key is its
`LAPTOP_KEY`, from `components/laptop/include/laptop_secrets.h`; type it in the
menu, or hand it over before the first start:

    defaults write nl.w-tb.desklink key <LAPTOP_KEY>

and Desk Link moves it into the login keychain.

## How it talks to the panel

It posts to the panel's `/laptop` whenever what plays changes, and every ten
seconds while anything does; the panel lets go of a Mac it has not heard from
in thirty. The panel sends `/command` to port 47801 here, and fetches the cover
from `/art.jpg`. Both sides send the key as `X-Laptop-Key`. The format is in
the panel's `components/laptop/include/laptop_protocol.h`.

Since macOS 15.4 Apple only lets its own apps read Now Playing; Desk Link reads
it through [mediaremote-adapter](https://github.com/ungive/mediaremote-adapter),
which runs it under `/usr/bin/perl`.
