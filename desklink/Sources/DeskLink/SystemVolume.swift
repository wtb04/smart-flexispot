import AudioToolbox
import CoreAudio
import Foundation

/// The Mac's own output: the default device's volume and mute, as the menu
/// bar's slider has them. Some outputs, such as a display over HDMI, take no
/// volume from the Mac at all; then there is none to share.
final class SystemVolume {
    var onChange: (() -> Void)?

    private var device = AudioObjectID(kAudioObjectUnknown)
    private let queue = DispatchQueue.main
    private lazy var changed: AudioObjectPropertyListenerBlock = { [weak self] _, _ in self?.onChange?() }
    private lazy var moved: AudioObjectPropertyListenerBlock = { [weak self] _, _ in
        self?.follow()
        self?.onChange?()
    }

    private static var volumeAddress = AudioObjectPropertyAddress(
        mSelector: kAudioHardwareServiceDeviceProperty_VirtualMainVolume,
        mScope: kAudioDevicePropertyScopeOutput, mElement: kAudioObjectPropertyElementMain)
    private static var muteAddress = AudioObjectPropertyAddress(
        mSelector: kAudioDevicePropertyMute,
        mScope: kAudioDevicePropertyScopeOutput, mElement: kAudioObjectPropertyElementMain)
    private static var defaultAddress = AudioObjectPropertyAddress(
        mSelector: kAudioHardwarePropertyDefaultOutputDevice,
        mScope: kAudioObjectPropertyScopeGlobal, mElement: kAudioObjectPropertyElementMain)

    init() {
        AudioObjectAddPropertyListenerBlock(AudioObjectID(kAudioObjectSystemObject), &Self.defaultAddress, queue, moved)
        follow()
    }

    /// Percent, or nil when the output takes no volume from here.
    var percent: Int? {
        guard settable(&Self.volumeAddress) else { return nil }
        var value = Float32(0)
        var size = UInt32(MemoryLayout<Float32>.size)
        guard AudioObjectGetPropertyData(device, &Self.volumeAddress, 0, nil, &size, &value) == noErr else { return nil }
        return Int((value * 100).rounded())
    }

    var muted: Bool {
        var value = UInt32(0)
        var size = UInt32(MemoryLayout<UInt32>.size)
        guard AudioObjectGetPropertyData(device, &Self.muteAddress, 0, nil, &size, &value) == noErr else { return false }
        return value != 0
    }

    func set(percent: Int) {
        var value = Float32(min(max(percent, 0), 100)) / 100
        AudioObjectSetPropertyData(device, &Self.volumeAddress, 0, nil, UInt32(MemoryLayout<Float32>.size), &value)
    }

    func set(muted: Bool) {
        var value = UInt32(muted ? 1 : 0)
        AudioObjectSetPropertyData(device, &Self.muteAddress, 0, nil, UInt32(MemoryLayout<UInt32>.size), &value)
    }

    private func follow() {
        if device != kAudioObjectUnknown {
            AudioObjectRemovePropertyListenerBlock(device, &Self.volumeAddress, queue, changed)
            AudioObjectRemovePropertyListenerBlock(device, &Self.muteAddress, queue, changed)
        }
        var found = AudioObjectID(kAudioObjectUnknown)
        var size = UInt32(MemoryLayout<AudioObjectID>.size)
        AudioObjectGetPropertyData(AudioObjectID(kAudioObjectSystemObject), &Self.defaultAddress, 0, nil, &size, &found)
        device = found
        AudioObjectAddPropertyListenerBlock(device, &Self.volumeAddress, queue, changed)
        AudioObjectAddPropertyListenerBlock(device, &Self.muteAddress, queue, changed)
    }

    private func settable(_ address: inout AudioObjectPropertyAddress) -> Bool {
        var can = DarwinBoolean(false)
        return AudioObjectHasProperty(device, &address)
            && AudioObjectIsPropertySettable(device, &address, &can) == noErr && can.boolValue
    }
}
