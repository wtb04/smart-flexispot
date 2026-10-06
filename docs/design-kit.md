# The design kit

Everything the panel draws with lives in one place: the colours, sizes,
surfaces, controls, instruments, popovers and motion, in
[`components/ui/include/kit.h`](../components/ui/include/kit.h). A page is
built only from what the kit hands out, and never styles itself. That way a
change of look, accent or glass restyles every screen at once, and every page
gets what was learned about drawing fast on the panel.

## The look

Things that stay are solid. Things you touch or that float are glass.

- **Solid:** a flat fill lifted by one soft shadow (`tile`, raised a little
  more with `Lift::High`), or sunk into the surface (`well`). No gradients on
  solid surfaces.
- **Glass:** the dock, the bar's capsules, popovers and notices. It is a flat
  tint with a rim, over a blurred picture of what lies behind.
- **Tick marks and zone bands:** tick marks only where a value is read off a
  scale, zone bands only where good, fair and poor are the point.

The ground is a warm, lamp-lit dark, or warm paper in the light look. It is
worked out in code and dithered, so its soft glows don't band.

Sizes, spacing, radii and durations are tokens (`space::`, `radius::`,
`size::`, `motion::`). Text has roles (`Role::Label` up to `Role::ReadXXL`) and
inks (`Ink::Primary`, `Secondary`, `Tertiary`, `Accent`, `Green`, `Amber`,
`Red`), and never a font or colour of its own. Figures are tabular, so a
changing number doesn't shift.

## Layers

`kit::screen(scr)` builds the planes a screen stands on, back to front:

| Layer | What lives there |
|---|---|
| Ground | the ground picture, and the tile shadows baked into it |
| Content | pages, which scroll under the chrome |
| Chrome | the bar and the dock, which stay put and cast onto the content |
| Overlay | popovers and sheets, over a scrim |
| Alert | notices, over everything |

A thing's layer decides how it is lit and what it covers, so put it on the
right one with `kit::layer(Layer::...)`. Don't fake depth with a bigger shadow.

## Building blocks

| Call | What it makes |
|---|---|
| `tile`, `well`, `glass` | the surfaces |
| `text`, `icon`, `hairline` | words, glyphs (Material Symbols names in `glyph::`), dividers |
| `button` | `Raised`, `Accent`, `Flat` or `Soft`, with a glyph and/or a label, round or not, checkable or not |
| `lamp_tile`, `toggle`, `segmented` | on/off things and choices |
| `capsule`, `capsule_item` | the bar's glass pills |
| `Dial` | the one instrument for readings: needle, zones, a target knob, lit ticks for a timer |
| `Rail` | a straight scale, slider or progress, with notches for presets |
| `popover`, `popover_source` | a glass card that grows out of what was tapped |
| `notice` | a slab that drops from under the bar and leaves on its own |
| `section`, `group`, `row` | a caption, and rows in one tile split by hairlines: a glyph, a time, a feed's mark, a title and a detail line, then a value, a chevron, a toggle or a badge |
| `list` | a tile with a column that scrolls inside it, for a long list |
| `badge`, `empty` | a small pill in an ink, and what shows where a feed is missing |
| `Readout`, `Stepper` | a big figure, and minus, value, plus, both a size that never changes |
| `sheet`, `sheet_row` | a big centred popover whose body scrolls, and a label with its value on it |
| `dot`, `status` | a state at a glance, and the bar's clock with the panel's own Wi-Fi and phone marks |
| `Cover`, `Backdrop` | a picture in a rounded rectangle or a circle, its corners cut once as it arrives, and the blurred backdrop the music and cinema views lay behind everything |
| `page`, `widget_grid`, `widget` | a page on the Content plane, and Home's grid of widgets that only show |
| `dock_item`, `show_page` | the dock's choices, its mark sliding to the chosen one, and the change from one page to the next |
| `set_usable` | faded to 40% and untappable, never LVGL's grey |

A control the playing device can never act on is hidden, not faded. A feed
that is missing or failing shows empty, never invented data.

## Motion

One spring moves everything (`motion::spring`, damping 0.86), scaled to each
duration, plus `motion::settle` for going back into something without
overshooting:

- **Press and release:** a press gives a little on the first frame after the finger
  lands, and lets go on the spring.
- **Popovers:** a popover grows out of its source, and its frost spreads out
  from the source in bands. On closing it shrinks back into the source, and the
  live screen returns in bands.
- **Notices:** a notice drops from under the bar and accelerates away when it
  leaves.

## The lab

```sh
cd sim && cmake -S . -B build -G Ninja -DLVGL_DIR=<lvgl> && ninja -C build
./build/sim --lab
```

The lab shows every part of the kit in every state, on sheets picked with keys
1 to 6:

| Key | Does |
|---|---|
| `l` | switches the look |
| `g` | changes the glass |
| `a` | changes the accent |
| `o` | opens a popover |
| `c` | closes it |
| `p` | runs the bench |
| `v` | runs the list study: one long list built several ways, each scrolled and timed |

The bench drives each kind of change in turn and prints the milliseconds per
frame, and on the panel it splits each frame into drawing, rotating onto the
panel, and waiting.

On the panel, the development build opens the lab and runs the bench over the
network:

```sh
curl 'http://<panel>/bench?lab'       # the lab
curl 'http://<panel>/bench?k=p'       # a key, here the bench
curl 'http://<panel>/bench?report'    # its table
```

The Mac is 20 to 50 times faster than the panel and has no 2D unit, so it is
only good for comparing before with after. Times that matter are measured on
the panel.

## Drawing fast on the panel

The panel has no GPU. Every changed pixel is drawn by the CPU, then turned onto
the portrait panel by the 2D unit. What a frame costs depends on how much of
the screen changes, so:

1. **Move small things, never the whole screen.** Redrawing the whole screen
   costs about 80 to 90 ms, plus about 40 ms to turn it onto the panel, so
   anything that slides or fades the whole screen runs at about 8 frames a
   second. Something that has to cover the screen does it in bands, each a
   small redraw. That is how popovers frost and close.
2. **Shadows only from the kit.** Tiles have their shadow baked into the ground
   while they stand still, and glass keeps a picture of its own shadow. A big
   soft shadow on something that moves is worked out again every frame over
   everything it passes (`motion::moving` swaps it for a small one). A shadow
   ends before it reaches a tile beside it: across a tile's edge its soft
   fade comes out as one hard step in 16-bit colour, which is why the dock is
   `Glass::Dock`.
3. **Opaque where possible.** LVGL stops drawing what lies beneath an opaque
   object that covers the change. A transparent full-screen container makes it
   draw everything under it.
4. **Set only on change.** Setting a style or a text redraws the object even
   when the value is the same. The kit's setters check first, and page code
   should too.
5. **Many still parts become one picture.** A dial's scale is drawn once into a
   picture, and only its needle redraws.
6. **No live blur.** A popover's backdrop is a picture of the screen, taken and
   blurred once on the other core. That work starts as the finger lands on the
   popover's source (`popover_source`), so the card can open on the next
   frame.
7. **No `clip_corner`, and no LVGL gradients on big areas.** The first is very
   slow, and the second bands in 16-bit colour. Soft areas generated in code
   are dithered (`kit_pixels.h`).
8. **Fixed widths for live labels.** A badge or readout that changes keeps one
   width. That keeps it steady to look at, and saves a new layout of everything
   around it.
9. **Scroll a list inside its tile, never the page.** `kit::list` scrolls its
   column inside the tile's padding, clear of its rounded corners, so the
   tile covers what scrolls and a frame of it is the tile's flat fill and the
   rows in sight. Its rows draw no fill of their own until pressed, no shadow,
   and draw their hairlines themselves. In the lab's list study, against that:
   a rounded tile scrolling itself draws the ground under it as well, about 2
   times the drawing; rows over glass about 3 times, over a flat area on
   glass about 4; the whole page scrolling under the bar about 6 to 8, with 6
   times the pixels. A sheet's body scrolls over a picture of the glass under
   it, a fifth of what the same body on plain glass costs.
10. **Pages slide as a carousel, by copying.** The new page comes in from
   the side its item lies on in the dock. On the panel it is drawn once, at
   rest, into a frame held back from the panel. Each step then copies the two
   pages side by side with the PPA, and LVGL draws nothing meanwhile: its
   refresh is put off rather than paused, since anything marked for drawing
   wakes a paused one. Sliding by drawing the whole page every frame ran at 8
   frames a second. A wipe in bands was tried and dropped. Pages are made once
   and hidden, never rebuilt, and the bar and the dock stay as they are.
11. **Big buffers once, in PSRAM.** They are made once and freed when done.
   Free PSRAM is tight while the old UI still runs alongside.

What that buys, measured on the panel:

| Change | ms a frame |
|---|---|
| a button press | 5 to 12 |
| a segmented control | 2 to 5 |
| dial needles | 13 to 19 |
| a popover growing | 27 to 36, after a first frame of 56 to 76 |
| a popover closing | 8 bands of 25 to 30 |
| a notice | 13 to 18 |
| scrolling a whole page under the bar | 105 to 145 |
