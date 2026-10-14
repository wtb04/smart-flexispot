import CoreGraphics
import CryptoKit
import Foundation
import ImageIO
import UniformTypeIdentifiers

/// The cover as the panel takes it: a baseline JPEG no larger than it draws,
/// both sides a multiple of eight so its decoder does it in hardware.
struct Artwork {
    let jpeg: Data
    let version: String
    let wide: Bool  // a video's frame rather than a record's sleeve

    private static let largest = 480

    init?(_ original: Data) {
        guard let source = CGImageSourceCreateWithData(original as CFData, nil),
              let image = CGImageSourceCreateImageAtIndex(source, 0, nil) else { return nil }
        let scale = min(1, Double(Self.largest) / Double(max(image.width, image.height)))
        let width = max(8, Int(Double(image.width) * scale) / 8 * 8)
        let height = max(8, Int(Double(image.height) * scale) / 8 * 8)
        guard let context = CGContext(data: nil, width: width, height: height, bitsPerComponent: 8,
                                      bytesPerRow: 0, space: CGColorSpaceCreateDeviceRGB(),
                                      bitmapInfo: CGImageAlphaInfo.noneSkipLast.rawValue) else { return nil }
        context.interpolationQuality = .high
        context.draw(image, in: CGRect(x: 0, y: 0, width: width, height: height))
        guard let scaled = context.makeImage() else { return nil }
        let out = NSMutableData()
        guard let destination = CGImageDestinationCreateWithData(out, UTType.jpeg.identifier as CFString, 1, nil)
        else { return nil }
        CGImageDestinationAddImage(destination, scaled, [kCGImageDestinationLossyCompressionQuality: 0.85] as CFDictionary)
        guard CGImageDestinationFinalize(destination) else { return nil }
        jpeg = out as Data
        wide = Double(image.width) > Double(image.height) * 1.2
        version = SHA256.hash(data: original).prefix(6).map { String(format: "%02x", $0) }.joined()
    }
}
