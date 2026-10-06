import CoreGraphics
import Foundation
import ImageIO

/// A YouTube video's own thumbnail, for one a browser plays: Safari hands Now
/// Playing a small cover, or none at all. The video is found by searching its
/// title and channel and taking the result with both exactly; when Safari did
/// give a cover, the thumbnail must look the same as it, or it is not used.
/// The channel's own picture comes with it, square, for where a square is drawn.
enum YouTubeCover {
    struct Found {
        let thumbnail: Data
        let channel: Data?
    }

    private struct Video {
        let id: String
        let avatar: String?  // where its channel's picture is
        let short: Bool
    }

    private static let agent = "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/605.1.15"
    private static let sameEnough = 12.0  // mean difference over 16 by 9 greys; another video is near 60
    private static let largest = "=s480"  // the panel's largest picture

    static func find(title: String, channel: String, small: Data?) async -> Found? {
        guard let video = await video(title: title, channel: channel) else { return nil }
        for size in ["maxresdefault", "mqdefault"] {
            guard let data = await fetch("https://i.ytimg.com/vi/\(video.id)/\(size).jpg") else { continue }
            // A Short's cover is its upright frame, never like this wide one;
            // its channel was made sure of instead.
            if !video.short, let small, let difference = difference(small, data), difference > sameEnough {
                return nil  // a video of the same name, but not this one
            }
            var picture: Data?
            if let avatar = video.avatar { picture = await fetch(avatar) }
            return Found(thumbnail: data, channel: picture)
        }
        return nil
    }

    private static func fetch(_ address: String) async -> Data? {
        guard let url = URL(string: address) else { return nil }
        var request = URLRequest(url: url, timeoutInterval: 5)
        request.setValue(agent, forHTTPHeaderField: "User-Agent")
        request.setValue("en", forHTTPHeaderField: "Accept-Language")
        guard let (data, response) = try? await URLSession.shared.data(for: request),
              (response as? HTTPURLResponse)?.statusCode == 200 else { return nil }
        return data
    }

    private static func video(title: String, channel: String) async -> Video? {
        var search = URLComponents(string: "https://www.youtube.com/results")!
        search.queryItems = [URLQueryItem(name: "search_query", value: "\(title) \(channel)")]
        guard let data = await fetch(search.url!.absoluteString),
              let page = String(data: data, encoding: .utf8),
              let start = page.range(of: "var ytInitialData = "),
              let end = page.range(of: ";</script>", range: start.upperBound..<page.endIndex),
              let json = try? JSONSerialization.jsonObject(with: Data(page[start.upperBound..<end.lowerBound].utf8))
        else { return nil }
        let wanted = (title.lowercased(), channel.lowercased())
        if let found: Video = first(in: json, { object in
            guard let video = object["videoRenderer"] as? [String: Any], let id = video["videoId"] as? String else { return nil }
            let named = ((video["title"] as? [String: Any])?["runs"] as? [[String: Any]])?.first?["text"] as? String
            let owner = ((video["ownerText"] as? [String: Any])?["runs"] as? [[String: Any]])?.first?["text"] as? String
            guard named?.lowercased() == wanted.0, wanted.1.isEmpty || owner?.lowercased() == wanted.1 else { return nil }
            return Video(id: id, avatar: avatar(of: video), short: false)
        }) {
            return found
        }
        // A Short is listed by its title and views alone: its channel is asked for after.
        let short: String? = first(in: json) { object in
            guard let short = object["shortsLockupViewModel"] as? [String: Any],
                  let text = short["accessibilityText"] as? String, text.lowercased().hasPrefix(wanted.0 + ","),
                  let entity = short["entityId"] as? String, entity.hasPrefix("shorts-shelf-item-") else { return nil }
            return String(entity.dropFirst("shorts-shelf-item-".count))
        }
        guard let short else { return nil }
        return await shortsChannel(short, named: wanted.1).map { Video(id: short, avatar: $0, short: true) }
    }

    /// Where a Short's channel's picture is, once its channel is the one named;
    /// "" when it is but has no picture, nil when it is another's.
    private static func shortsChannel(_ id: String, named channel: String) async -> String?? {
        var embed = URLComponents(string: "https://www.youtube.com/oembed")!
        embed.queryItems = [URLQueryItem(name: "url", value: "https://www.youtube.com/shorts/\(id)"),
                            URLQueryItem(name: "format", value: "json")]
        guard let data = await fetch(embed.url!.absoluteString),
              let about = try? JSONSerialization.jsonObject(with: data) as? [String: Any],
              let author = about["author_name"] as? String,
              channel.isEmpty || author.lowercased() == channel else { return nil }
        guard let home = about["author_url"] as? String, let page = await fetch(home),
              let html = String(data: page, encoding: .utf8),
              let tag = html.range(of: #"<meta property="og:image" content="[^"]+""#, options: .regularExpression)
        else { return .some(nil) }
        let url = html[tag].dropFirst(#"<meta property="og:image" content=""#.count).dropLast()
        return .some(url.replacingOccurrences(of: #"=s\d+"#, with: largest, options: .regularExpression))
    }

    /// The first object, depth first, that `pick` makes something of.
    private static func first<T>(in node: Any, _ pick: ([String: Any]) -> T?) -> T? {
        if let object = node as? [String: Any] {
            if let found = pick(object) { return found }
            for value in object.values {
                if let found = first(in: value, pick) { return found }
            }
        } else if let array = node as? [Any] {
            for value in array {
                if let found = first(in: value, pick) { return found }
            }
        }
        return nil
    }

    /// The channel's picture as the search gives it, 68 square, asked for at the panel's largest.
    private static func avatar(of video: [String: Any]) -> String? {
        let renderer = (video["channelThumbnailSupportedRenderers"] as? [String: Any])?["channelThumbnailWithLinkRenderer"]
        let thumbnails = (((renderer as? [String: Any])?["thumbnail"] as? [String: Any])?["thumbnails"] as? [[String: Any]])
        guard let url = thumbnails?.last?["url"] as? String else { return nil }
        return url.replacingOccurrences(of: #"=s\d+"#, with: largest, options: .regularExpression)
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
