// The system overview in the README, drawn as a datasheet block diagram.
// typst compile --format svg --input theme=light docs/diagrams/overview.typ docs/diagrams/overview-light.svg
// typst compile --format svg --input theme=dark  docs/diagrams/overview.typ docs/diagrams/overview-dark.svg
#import "style.typ": *
#show: setup

#canvas({
  import draw: *

  rect((0, 0), (6, 5.6), stroke: (paint: muted, thickness: 0.6pt, dash: "dashed"))
  content((0.2, 5.4), text(weight: "bold", size: 8pt, "M5Stack Tab5"), anchor: "north-west")
  block-of(0.5, 3.1, 5.5, 4.8, "ESP32-P4", "UI, desk driver, network")
  block-of(0.5, 0.5, 5.5, 2.2, "ESP32-C6", "Wi-Fi 6, Bluetooth LE")
  line((3, 3.1), (3, 2.2), stroke: thin, mark: (start: head, end: head))
  label((3.1, 2.65), "SDIO", anchor: "west")

  block-of(14, 0.5, 17.6, 4.8, "Control box", "LoctekMotion", "HS01B, HS13B")

  line((5.5, 3.95), (14, 3.95), stroke: thin, mark: (start: head, end: head))
  callout((9.75, 3.95), 1)
  label((9.75, 4.2), "UART")

  block-of(8.1, 0.5, 11.7, 2.2, "ESP32 companion", "on the carrier board")
  line((5.5, 1.35), (8.1, 1.35), stroke: (paint: ink, thickness: 0.6pt, dash: "dashed"), mark: (start: head, end: head))
  callout((7.05, 1.35), 2)
  label((7.05, 1.6), "Bluetooth LE")
  line((11.7, 1.7), (14, 1.7), stroke: thin, mark: (start: head, end: head))
  label((12.85, 1.75), "UART")
  line((14, 1.0), (11.7, 1.0), stroke: thin, mark: (end: head))
  label((12.85, 0.95), "+5 V", anchor: "north")
})

#v(4pt)
#legend(17.6cm,
  [A cable from the Tab5's M5-Bus to the control box's RJ45 port. UART at 9600 baud, 8N1, with 5 V logic from the box.],
  [Bluetooth to a companion that stays on the cable. A GATT service with 12-byte commands and 16-byte status packets. A held key is resent every 100 ms and let go after 300 ms. The companion runs off the box's 5 V.],
)
