// Desk Link's windows for the README, as the panel's screenshots are shown:
// side by side on the panel's own background, with room around them.
//
//     swift desklink/shots/frame.swift out.png menu.png settings.png
//
// Take the windows on a Retina screen, Cmd+Shift+4 then Space, Option held
// while clicking so they come without macOS's shadow; this adds its own.
import AppKit

let arguments = CommandLine.arguments.dropFirst()
guard arguments.count >= 2 else {
    print("usage: frame.swift out.png window.png [window.png ...]")
    exit(2)
}
let images = arguments.dropFirst().map { path -> CGImage in
    guard let image = NSImage(contentsOfFile: path)?.cgImage(forProposedRect: nil, context: nil, hints: nil) else {
        print("cannot read \(path)")
        exit(1)
    }
    return image
}

let scale = images[0].width >= 500 ? 2 : 1  // a Retina capture is twice the size
let pad = 64 * scale
let gap = 40 * scale
let width = images.map(\.width).reduce(0, +) + gap * (images.count - 1) + pad * 2
let height = (images.map(\.height).max() ?? 0) + pad * 2

let space = CGColorSpaceCreateDeviceRGB()
let context = CGContext(data: nil, width: width, height: height, bitsPerComponent: 8, bytesPerRow: 0,
                        space: space, bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue)!
// The panel's background, a little lighter at the top, as its cards are.
let colours = [CGColor(srgbRed: 0x2a / 255.0, green: 0x22 / 255.0, blue: 0x1d / 255.0, alpha: 1),
               CGColor(srgbRed: 0x15 / 255.0, green: 0x11 / 255.0, blue: 0x0f / 255.0, alpha: 1)] as CFArray
let gradient = CGGradient(colorsSpace: space, colors: colours, locations: [0, 1])!
context.drawLinearGradient(gradient, start: CGPoint(x: 0, y: height), end: CGPoint(x: 0, y: 0), options: [])

var x = pad
for image in images {
    let y = (height - image.height) / 2
    context.saveGState()
    context.setShadow(offset: CGSize(width: 0, height: -8 * scale), blur: CGFloat(28 * scale),
                      color: CGColor(gray: 0, alpha: 0.55))
    context.draw(image, in: CGRect(x: x, y: y, width: image.width, height: image.height))
    context.restoreGState()
    x += image.width + gap
}

let out = URL(fileURLWithPath: arguments.first!)
let destination = CGImageDestinationCreateWithURL(out as CFURL, "public.png" as CFString, 1, nil)!
CGImageDestinationAddImage(destination, context.makeImage()!, nil)
CGImageDestinationFinalize(destination)
print("wrote \(out.path), \(width)x\(height)")
