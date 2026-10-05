import CoreGraphics
import Foundation
import ImageIO

/// A YouTube video's own thumbnail, for one a browser plays: Safari hands Now
/// Playing a small cover, or none at all. The video is found by searching its
/// title and channel and taking the result with both exactly; when Safari did
/// give a cover, the thumbnail must look the same as it, or it is not used.
enum YouTubeCover {
    private static let agent = "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/605.1.15"
    private static let sameEnough = 12.0  // mean difference over 16 by 9 greys; another video is near 60

    static func find(title: String, channel: String, small: Data?) async -> Data? {
        guard let id = await videoID(title: title, channel: channel) else { return nil }
        for size in ["maxresdefault", "mqdefault"] {
            guard let url = URL(string: "https://i.ytimg.com/vi/\(id)/\(size).jpg"),
                  let (data, response) = try? await URLSession.shared.data(from: url),
                  (response as? HTTPURLResponse)?.statusCode == 200 else { continue }
            if let small, let difference = difference(small, data), difference > sameEnough {
                return nil  // a video of the same name, but not this one
            }
            return data
        }
        return nil
    }

    private static func videoID(title: String, channel: String) async -> String? {
        var search = URLComponents(string: "https://www.youtube.com/results")!
        search.queryItems = [URLQueryItem(name: "search_query", value: "\(title) \(channel)")]
        var request = URLRequest(url: search.url!, timeoutInterval: 5)
        request.setValue(agent, forHTTPHeaderField: "User-Agent")
        request.setValue("en", forHTTPHeaderField: "Accept-Language")
        guard let (data, _) = try? await URLSession.shared.data(for: request),
              let page = String(data: data, encoding: .utf8),
              let start = page.range(of: "var ytInitialData = "),
              let end = page.range(of: ";</script>", range: start.upperBound..<page.endIndex),
              let json = try? JSONSerialization.jsonObject(with: Data(page[start.upperBound..<end.lowerBound].utf8))
        else { return nil }
        let wanted = (title.lowercased(), channel.lowercased())
        return first(in: json) { video in
            let named = ((video["title"] as? [String: Any])?["runs"] as? [[String: Any]])?.first?["text"] as? String
            let owner = ((video["ownerText"] as? [String: Any])?["runs"] as? [[String: Any]])?.first?["text"] as? String
            return named?.lowercased() == wanted.0 && (wanted.1.isEmpty || owner?.lowercased() == wanted.1)
        }
    }

    /// The first videoRenderer, depth first, that `matches`, as its id.
    private static func first(in node: Any, where matches: ([String: Any]) -> Bool) -> String? {
        if let object = node as? [String: Any] {
            if let video = object["videoRenderer"] as? [String: Any], matches(video), let id = video["videoId"] as? String {
                return id
            }
            for value in object.values {
                if let id = first(in: value, where: matches) { return id }
            }
        } else if let array = node as? [Any] {
            for value in array {
                if let id = first(in: value, where: matches) { return id }
            }
        }
        return nil
    }

    private static func difference(_ a: Data, _ b: Data) -> Double? {
        guard let a = greys(a), let b = greys(b) else { return nil }
        return zip(a, b).map { abs($0 - $1) }.reduce(0, +) / Double(a.count)
    }

    private static func greys(_ data: Data) -> [Double]? {
        guard let source = CGImageSourceCreateWithData(data as CFData, nil),
              let image = CGImageSourceCreateImageAtIndex(source, 0, nil) else { return nil }
        let (w, h) = (16, 9)
        var pixels = [UInt8](repeating: 0, count: w * h * 4)
        guard let context = CGContext(data: &pixels, width: w, height: h, bitsPerComponent: 8, bytesPerRow: w * 4,
                                      space: CGColorSpaceCreateDeviceRGB(),
                                      bitmapInfo: CGImageAlphaInfo.noneSkipLast.rawValue) else { return nil }
        context.interpolationQuality = .high
        context.draw(image, in: CGRect(x: 0, y: 0, width: w, height: h))
        return stride(from: 0, to: pixels.count, by: 4).map {
            (Double(pixels[$0]) + Double(pixels[$0 + 1]) + Double(pixels[$0 + 2])) / 3
        }
    }
}
