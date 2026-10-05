import Foundation
import Network

struct Request {
    let method: String
    let path: String
    let headers: [String: String]  // names in lower case
    let body: Data
}

struct Response {
    var status: Int
    var type = ""
    var body = Data()

    static let noContent = Response(status: 204)
    static let notFound = Response(status: 404)
}

/// Where the panel sends its commands and fetches the cover: one request to a
/// connection, as the panel closes it after.
final class CommandServer {
    private let port: NWEndpoint.Port
    private let handle: (Request) -> Response
    private let queue = DispatchQueue(label: "nl.w-tb.desklink.server")
    private var listener: NWListener?

    private static let largest = 64 * 1024

    init(port: UInt16, handle: @escaping (Request) -> Response) {
        self.port = NWEndpoint.Port(rawValue: port)!
        self.handle = handle
    }

    func start() {
        guard listener == nil, let listener = try? NWListener(using: .tcp, on: port) else { return }
        listener.newConnectionHandler = { [weak self] connection in self?.serve(connection) }
        listener.stateUpdateHandler = { state in
            if case .failed(let error) = state { NSLog("Desk Link: cannot listen: \(error)") }
        }
        listener.start(queue: queue)
        self.listener = listener
    }

    func stop() {
        listener?.cancel()
        listener = nil
    }

    private func serve(_ connection: NWConnection) {
        connection.start(queue: queue)
        receive(connection, Data())
    }

    private func receive(_ connection: NWConnection, _ soFar: Data) {
        connection.receive(minimumIncompleteLength: 1, maximumLength: Self.largest) { [weak self] data, _, done, error in
            guard let self else { return }
            var buffer = soFar
            if let data { buffer.append(data) }
            if let request = Self.parse(buffer) {
                self.answer(connection, self.handle(request))
            } else if done || error != nil || buffer.count > Self.largest {
                connection.cancel()
            } else {
                self.receive(connection, buffer)
            }
        }
    }

    private func answer(_ connection: NWConnection, _ response: Response) {
        var head = "HTTP/1.1 \(response.status) \(Self.reason(response.status))\r\n"
        head += "Content-Length: \(response.body.count)\r\nConnection: close\r\n"
        if !response.type.isEmpty { head += "Content-Type: \(response.type)\r\n" }
        head += "\r\n"
        connection.send(content: Data(head.utf8) + response.body, completion: .contentProcessed { _ in
            connection.cancel()
        })
    }

    /// Nil until the whole request is in.
    private static func parse(_ data: Data) -> Request? {
        guard let end = data.range(of: Data("\r\n\r\n".utf8)) else { return nil }
        let lines = String(decoding: data[..<end.lowerBound], as: UTF8.self).components(separatedBy: "\r\n")
        let start = lines.first?.split(separator: " ") ?? []
        guard start.count >= 2 else { return nil }
        var headers: [String: String] = [:]
        for line in lines.dropFirst() {
            guard let colon = line.firstIndex(of: ":") else { continue }
            headers[line[..<colon].lowercased()] = line[line.index(after: colon)...].trimmingCharacters(in: .whitespaces)
        }
        let length = Int(headers["content-length"] ?? "") ?? 0
        let body = data[end.upperBound...]
        guard body.count >= length else { return nil }
        return Request(method: String(start[0]), path: String(start[1]), headers: headers,
                       body: Data(body.prefix(length)))
    }

    private static func reason(_ status: Int) -> String {
        switch status {
        case 200: return "OK"
        case 204: return "No Content"
        case 400: return "Bad Request"
        case 403: return "Forbidden"
        default: return "Not Found"
        }
    }
}
