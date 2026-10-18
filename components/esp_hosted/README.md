# esp_hosted 1.4.0, patched

The host half of [esp-hosted-mcu](https://github.com/espressif/esp-hosted-mcu)
1.4.0 (commit 6040085), the version the Tab5's C6 runs, with the slave,
examples and docs left out. `main/idf_component.yml` points the dependency here.

Changes, in `host/drivers/transport/sdio/sdio_drv.c` and
`host/drivers/transport/transport_drv.c` and marked "Smart Flexispot patch",
take what goes over the link out of internal RAM:

- In streaming mode the link reads everything the C6 has queued in one go,
  into two buffers. Upstream grows them in internal RAM to each longer burst,
  and a burst of a few dozen kilobytes later asked for a block the heap no
  longer had in one piece; the driver's assert then restarted the panel. Here
  they are taken in PSRAM, which the SDMMC host reads into as well, at once at
  the most the C6 can queue (20 × 1536 bytes).
- Each packet is then copied out of the stream into a buffer of its own.
  Upstream takes those from a pool of internal DMA memory that never gives
  back, so a Jellyfin burst kept some 36 KB of it for good. They need no DMA,
  and are taken in PSRAM instead.
- Each packet sent is copied into a buffer of its own as well, from two more
  such pools: one for Wi-Fi, one for the requests to the C6 and Bluetooth,
  which held some 23 KB from the link coming up alone. Those are read by the
  SDMMC host, and are taken in PSRAM at a cache line's alignment.

Moving to a newer esp_hosted means reflashing the C6 to match; check whether
the patch is still needed then.
