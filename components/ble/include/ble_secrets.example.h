#pragma once

// Copy this file to ble_secrets.h and fill it in. ble_secrets.h is gitignored.

// The phone's Identity Resolving Key, as 32 hex characters.
//
// An iPhone changes its Bluetooth address roughly every 15 minutes so it cannot
// be tracked by address alone. The address carries a random part and a hash of
// that random part made with this key, so holding the key -- and only holding
// the key -- lets the panel recognise the phone across every change.
//
// Paste it exactly as you were given it, in either form: 32 hex characters as
// Home Assistant's "Private BLE Device" integration shows it, or the 24
// base64 characters the Apple side gives. Both mean the same bytes in the same
// order. Separators are tolerated and case does not matter. An empty string
// disables resolution and leaves the scanner counting anonymous devices.
#define BLE_PHONE_IRK ""

