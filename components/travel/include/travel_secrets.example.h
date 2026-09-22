#pragma once

// Copy this file to travel_secrets.h (gitignored) and point it wherever the
// Reis service is running. While the panel is being worked on that is usually a
// laptop on the same network, over plain http; in the end it is the deployed
// one.
//
//   #define TRAVEL_HOST "http://<laptop>:5199"
//
// No trailing slash. An empty host leaves travel out altogether.
#define TRAVEL_HOST "https://travel.example.org"
