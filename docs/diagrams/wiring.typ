// The README's two wiring figures, one page each.
// typst compile --format svg --input theme=light docs/diagrams/wiring.typ 'docs/diagrams/wiring-{0p}-light.svg'
// typst compile --format svg --input theme=dark  docs/diagrams/wiring.typ 'docs/diagrams/wiring-{0p}-dark.svg'
#import "style.typ": *
#show: setup

#let step = 0.6

// One wire from a pin on the left block to a pin on the right, through a part
// when there is one. `dir` is the way the signal goes: "left", "right" or none.
#let wire-row(y, from, to-x, left-text, right-text, part: none, dir: none, part-x: (0, 0)) = {
  import draw: *
  content((from - 0.15, y), text(size: 7pt, left-text), anchor: "east")
  content((to-x + 0.15, y), text(size: 7pt, right-text), anchor: "west")
  let into-left = if dir == "left" { (end: head) } else { (:) }
  let into-right = if dir == "right" { (end: head) } else { (:) }
  if part == none {
    if dir == "left" {
      line((to-x, y), (from, y), stroke: thin, mark: into-left)
    } else {
      line((from, y), (to-x, y), stroke: thin, mark: into-right)
    }
  } else {
    let (px1, px2) = part-x
    if dir == "left" {
      line((to-x, y), (px2, y), stroke: thin)
      line((px1, y), (from, y), stroke: thin, mark: into-left)
    } else {
      line((from, y), (px1, y), stroke: thin)
      line((px2, y), (to-x, y), stroke: thin, mark: into-right)
    }
    rect((px1, y - 0.2), (px2, y + 0.2), fill: page-bg, stroke: thin)
    content(((px1 + px2) / 2, y), text(size: 6.5pt, part))
  }
}

#let frame(x1, y1, x2, y2, title, sub) = {
  import draw: *
  rect((x1, y1), (x2, y2), fill: shade, stroke: thin)
  content(((x1 + x2) / 2, y2 + 0.45), text(weight: "bold", size: 8pt, title), anchor: "south")
  content(((x1 + x2) / 2, y2 + 0.12), text(fill: muted, size: 6.5pt, sub), anchor: "south")
}

// Option 1: the Tab5's M5-Bus to the control box.
#canvas({
  import draw: *
  let rows = (
    ("pin 23, GPIO47  wake", "4  wake", "right"),
    ("pin 13, GPIO38  RX", "6  box TX", "left"),
    ("pin 14, GPIO37  TX", "5  box RX", "right"),
    ("pin 1, 3 or 5  GND", "7  GND", none),
  )
  let top = 3.0
  let bottom = top - (rows.len() - 1) * step
  frame(0, bottom - 0.45, 4.2, top + 0.45, "Tab5", "M5-Bus")
  frame(10.2, bottom - 0.45, 13.4, top + 0.45, "Control box", "RJ45")
  for (i, (l, r, d)) in rows.enumerate() {
    wire-row(top - i * step, 4.2, 10.2, l, r, dir: d)
  }
})

#v(4pt)
#block(width: 13.4cm, text(fill: muted)[HS01B-1 and HS13B-1 pinout. Measure your own control box before you connect anything: the HS13A-1 puts 29 V on pins 7 and 8.])

#pagebreak()

// Option 2: the companion's DevKit, the carrier board's parts, the jack.
#canvas({
  import draw: *
  let rows = (
    ("VIN", "8  +5 V", [PTC fuse 500 mA, SS14], "left"),
    ("GND", "7  GND", none, none),
    ("GPIO16  RX2", "6  box TX", [10k / 15k divider, BAT54S], "left"),
    ("GPIO17  TX2", "5  box RX", [220 Ω], "right"),
    ("GPIO23", "4  wake", [220 Ω], "right"),
    ("GPIO32", "LED  BT, yellow", [220 Ω], "right"),
    ("GPIO33", "LED  LINK, green", [220 Ω], "right"),
  )
  let top = 4.4
  let bottom = top - (rows.len() - 1) * step
  frame(0, bottom - 0.45, 3.2, top + 0.45, "ESP32 DevKit V1", "30-pin, in the socket")
  frame(12.4, bottom - 0.45, 15.8, top + 0.45, "RJ45 jack", "cable from the control box")
  rect((5.4, bottom - 0.45), (10.2, top + 0.45), stroke: (paint: muted, thickness: 0.6pt, dash: "dashed"))
  content((7.8, top + 0.9), text(weight: "bold", size: 8pt, "Carrier board"), anchor: "south")
  for (i, (l, r, p, d)) in rows.enumerate() {
    wire-row(top - i * step, 3.2, 12.4, l, r, part: p, dir: d, part-x: (5.8, 9.8))
  }
})

#v(4pt)
#block(width: 15.8cm, text(fill: muted)[Pins 4 to 8 measured on my control box. The two LEDs are inside the jack. Arrows show which way each signal goes.])
