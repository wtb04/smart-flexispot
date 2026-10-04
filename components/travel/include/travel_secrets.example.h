#pragma once

// Copy this file to travel_secrets.h (gitignored) and point it at the travel
// service the panel asks for the way to its next event. No trailing slash. An
// empty host leaves travel out altogether.
//
//   #define TRAVEL_HOST "http://<laptop>:5199"
#define TRAVEL_HOST ""

// What the service expects as its X-Api-Key header. Empty sends none.
#define TRAVEL_API_KEY ""
