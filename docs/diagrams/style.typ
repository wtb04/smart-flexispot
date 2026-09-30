// What the diagrams share: a datasheet look, in GitHub's light or dark colours.
#import "@preview/cetz:0.4.2": canvas, draw

#let dark = sys.inputs.at("theme", default: "light") == "dark"
#let ink = if dark { rgb("#e6edf3") } else { rgb("#1f2328") }
#let muted = if dark { rgb("#9198a1") } else { rgb("#59636e") }
#let shade = if dark { rgb("#151b23") } else { rgb("#f6f8fa") }
#let page-bg = if dark { rgb("#0d1117") } else { white }

#let thin = 0.6pt + ink
#let head = (symbol: ">", fill: ink, scale: 0.6)

#let setup(body) = {
  set page(width: auto, height: auto, margin: 12pt, fill: none)
  set text(font: "Helvetica Neue", fill: ink, size: 7.5pt)
  body
}

#let block-of(x1, y1, x2, y2, name, ..lines) = {
  import draw: *
  rect((x1, y1), (x2, y2), fill: shade, stroke: thin)
  content((x1 + 0.2, y2 - 0.2), text(weight: "bold", size: 8pt, name), anchor: "north-west")
  for (i, l) in lines.pos().enumerate() {
    content((x1 + 0.2, y2 - 0.62 - i * 0.34), text(fill: muted, l), anchor: "north-west")
  }
}

#let callout(at, n) = {
  import draw: *
  circle(at, radius: 0.19, fill: page-bg, stroke: thin)
  content(at, text(size: 6.5pt, weight: "bold", str(n)))
}

#let label(at, body, anchor: "south") = draw.content(at, text(fill: muted, size: 6.5pt, body), anchor: anchor, padding: 0.08)

#let legend(width, ..items) = block(width: width, grid(
  columns: (auto, 1fr),
  column-gutter: 6pt,
  row-gutter: 6pt,
  ..items.pos().enumerate().map(((i, body)) => (text(weight: "bold", str(i + 1)), body)).flatten(),
))
