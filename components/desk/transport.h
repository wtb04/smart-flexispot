#pragma once

#include "esp_err.h"
#include "freertos/FreeRTOS.h"

namespace desk::detail {

/** A line of the desk's trace: what the link heard or was asked, with when. */
void trace(const char *what, int a, int b = 0);


// What a link reports about itself, shown and logged as is. The supervisor
// compares against these pointers, so each link returns these very strings.
inline constexpr char kConnected[] = "connected";
inline constexpr char kAsleep[]    = "desk display asleep";

/** The way the desk is reached: the UART on this board, or the companion. */
class Transport {
public:
    virtual ~Transport() = default;

    /** Starts the link. Heights arrive on the link's own task, never waited on. */
    virtual esp_err_t start(void (*on_height)(int height_mm)) = 0;

    /** Up, down, or zero for a stop, which is always sent: a hand that was
     *  never holding can still end a travel. */
    virtual void move(int direction) = 0;

    /** One of the control box's own presets: go there, or store the height. */
    virtual void preset(int index) = 0;
    virtual void store(int index)  = 0;

    /** Steers to a height the box has no preset for. */
    virtual bool goto_height(int height_mm) = 0;

    virtual void wake() = 0;
    virtual bool can_wake() const = 0;

    /** For a display that answers but shows no height. */
    virtual void nudge() {}

    /** -1, 0 or +1, as the link last reported it. */
    virtual int  motion() const     = 0;
    virtual bool travelling() const = 0;

    /** How the link is doing, one of a fixed set of strings; kConnected when
     *  the desk can be driven. Called on every supervisor tick. */
    virtual const char *status(TickType_t now, bool height_known, int wake_attempts) = 0;

    virtual const char *name() const = 0;
};

Transport &wire();
Transport &bluetooth();

}  // namespace desk::detail
