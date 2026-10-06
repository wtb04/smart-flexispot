# The two screens built on the shared core, one of which goes into a build:
# the one the panel has had, and the design kit's, which is replacing it.
set(UI_CLASSIC_SOURCES
    classic_front.cpp dock.cpp notify.cpp home_page.cpp nav.cpp diagnostics_page.cpp settings_page.cpp
    splash.cpp calendar_page.cpp focus_view.cpp popout.cpp control_bar.cpp claude_pill.cpp
    thermostat.cpp favourites.cpp speaker_art.cpp cinema.cpp music.cpp volume_steps.cpp fullscreen_chrome.cpp
    modal_overlay.cpp)
# Shared by both while the design kit's radar keeps the old one's map, planes and zoom.
set(UI_SHARED_SOURCES radar_page.cpp)
set(UI_KIT_SOURCES
    kit_front.cpp kit_front_dock.cpp kit_front_desk.cpp kit_front_bar.cpp kit_front_cards.cpp kit_front_home.cpp
    kit_front_calendar.cpp kit_front_radar.cpp kit_front_setup.cpp kit_front_views.cpp)
