#pragma once

// Copy this file to ble_secrets.h (gitignored) and fill it in.
//
// The phone's Identity Resolving Key. An iPhone changes its Bluetooth address
// every 15 minutes or so; the address carries a random part and a hash of that
// part made with this key, so only the key lets the panel recognise the phone
// across the changes.
//
// Paste it exactly as you were given it: 32 hex characters as Home Assistant's
// "Private BLE Device" integration shows it, or the 24 base64 characters the
// Apple side gives. Separators and case do not matter. An empty string disables
// resolution.
#define BLE_PHONE_IRK ""

