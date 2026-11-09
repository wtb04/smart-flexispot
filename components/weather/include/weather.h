#pragma once

#include "esp_err.h"
#include "weather_parse.h"

namespace weather {
/** Called on the weather task once something new has come in. */
using UpdateHandler = void (*)();

esp_err_t start(UpdateHandler on_update);

/** Home Assistant's home, rounded to 0.01 degree as the radar keeps it. Nothing
 *  is asked for before it is known. */
void set_home(float lat, float lon);

/** Thread-safe copies of the last answers; false or none before the first. */
bool forecast(Forecast &out);
/** The next two hours of rain at home: Buienradar's where it reaches, else Open-Meteo's. */
int  rain(RainStep *out, int capacity);
/** KNMI's warnings for home, most severe first; none outside the Netherlands. */
int  warnings(Warning *out, int capacity);

}  // namespace weather
