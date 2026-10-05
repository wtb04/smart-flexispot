# Desk Link

A menu bar app that shares what plays on this Mac with the desk panel: the title, the
artist, the cover and where it is, from anything macOS shows under Now Playing,
YouTube in Safari as much as Spotify. The panel's media card then plays, pauses,
seeks and skips it.

Its menu turns sharing on and off, says what it shares and whether the panel
answers, and sets the panel's address and key and whether it opens at login.

## Build

From the repository's root:

    git submodule update --init
    cd desklink && make install   # builds, copies to /Applications and starts it

Needs macOS 26, Xcode's command line tools and CMake. It is signed with the
first Apple Development identity there is, so the keychain trusts each new
build as the same app. The key is the panel's `DESK_LINK_KEY`, from
`components/laptop/include/desk_link_secrets.h`; type it in the menu, or hand
it over before the first start:

    defaults write nl.w-tb.desklink key <DESK_LINK_KEY>

and Desk Link moves it into the login keychain.

## How it talks to the panel

Every message, each way, is sealed with AES-256-GCM under that key, as the
panel's `components/laptop/include/link_envelope.h` describes, and carries
when it was sent, so one that is stale or seen before is refused. Desk Link
tells the panel's `/link` how it is whenever what plays changes and every ten
seconds whatever it is; the panel answers with a pong, and pings port 47801
here to say whether it reaches the Mac back. The panel sends its commands to
`/link` here and asks `/cover` for the picture. tools/claude-hook hands its
events to `/claude` here, which only this Mac may reach, and Desk Link passes
them on. The messages are in `components/laptop/include/laptop_protocol.h`.

Since macOS 15.4 Apple only lets its own apps read Now Playing; Desk Link reads
it through [mediaremote-adapter](https://github.com/ungive/mediaremote-adapter),
which runs it under `/usr/bin/perl`.
