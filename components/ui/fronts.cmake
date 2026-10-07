# The two screens built on the shared core, one of which goes into a build:
# the one the panel has had, and the design kit's, which is replacing it.
set(UI_CLASSIC_SOURCES
    classic/classic_front.cpp classic/dock.cpp classic/notify.cpp classic/home_page.cpp classic/nav.cpp
    classic/diagnostics_page.cpp classic/settings_page.cpp classic/splash.cpp classic/radar_page.cpp
    classic/calendar_page.cpp classic/focus_view.cpp classic/popout.cpp classic/control_bar.cpp
    classic/claude_pill.cpp classic/thermostat.cpp classic/favourites.cpp classic/speaker_art.cpp
    classic/cinema.cpp classic/music.cpp classic/volume_steps.cpp classic/fullscreen_chrome.cpp
    classic/modal_overlay.cpp)
set(UI_KIT_SOURCES
    front/kit_front.cpp front/kit_front_dock.cpp front/kit_front_desk.cpp front/kit_front_bar.cpp
    front/kit_front_cards.cpp front/kit_front_home.cpp front/kit_front_calendar.cpp front/kit_front_radar.cpp
    front/kit_front_radar_view.cpp front/kit_front_setup.cpp front/kit_front_views.cpp
    front/kit_front_setup_diag.cpp front/kit_front_views_media.cpp front/kit_front_views_focus.cpp)
