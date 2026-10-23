// A crop of the screen's corner for the README: its corners rounded and a
// soft shadow under it, on nothing, so it sits on GitHub's light and dark.
// A window, which comes with corners of its own, keeps them: --window gives
// it only the shadow, the shape of its own.
//
//     swift desklink/shots/round.swift menu-raw.png menu.png && tools/strip_metadata.sh
//     swift desklink/shots/round.swift --window settings-raw.png settings.png
import AppKit

var arguments = Array(CommandLine.arguments.dropFirst())
let window = arguments.first == "--window"
if window { arguments.removeFirst() }
guard arguments.count == 2,
      let image = NSImage(contentsOfFile: arguments[0])?.cgImage(forProposedRect: nil, context: nil, hints: nil)
else {
    print("usage: round.swift in.png out.png")
    exit(2)
}
let radius = 24.0, blur = 36.0, room = 48
let width = image.width + room * 2, height = image.height + room * 2
let context = CGContext(data: nil, width: width, height: height, bitsPerComponent: 8, bytesPerRow: 0,
                        space: CGColorSpaceCreateDeviceRGB(), bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue)!
let frame = CGRect(x: room, y: room, width: image.width, height: image.height)
let shape = CGPath(roundedRect: frame, cornerWidth: radius, cornerHeight: radius, transform: nil)
context.saveGState()
context.setShadow(offset: CGSize(width: 0, height: -10), blur: blur, color: CGColor(gray: 0, alpha: 0.35))
if window {
    context.draw(image, in: frame)  // the shadow takes its shape from the window's own edges
} else {
    context.addPath(shape)
    context.setFillColor(CGColor(gray: 0, alpha: 1))
    context.fillPath()
}
context.restoreGState()
if !window {
    context.addPath(shape)
    context.clip()
    context.draw(image, in: frame)
}
let destination = CGImageDestinationCreateWithURL(URL(fileURLWithPath: arguments[1]) as CFURL, "public.png" as CFString, 1, nil)!
CGImageDestinationAddImage(destination, context.makeImage()!, nil)
CGImageDestinationFinalize(destination)
print("wrote \(arguments[1]), \(width)x\(height)")
