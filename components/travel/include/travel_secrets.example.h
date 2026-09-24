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

// What the service expects as X-Api-Key; the same value as reis_api_key in the
// Ansible vault. Empty sends none, which a local run does not ask for.
#define TRAVEL_API_KEY ""

