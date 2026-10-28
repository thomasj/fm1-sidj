// SPDX-License-Identifier: GPL-3.0-only
// Record the FM-1's USB audio input on a Mac (or any input), for testing SiDJ's USB audio.
//   swiftc -O tools/fm1_record.swift -o build/fm1_record
//   build/fm1_record                         list the audio inputs
//   build/fm1_record SECONDS OUT.wav [NAME] [CHANNELS]  record the input whose name contains NAME
//       (default "SiDJ") as 16-bit WAV; CHANNELS picks among the device's formats first (the FM-1
//       offers one, set on it: KNOB 3 in SONGS, 2 channels the mix, 5 the mix and the voices)
// macOS asks for microphone access the first time (any audio input counts).
import AVFoundation
import CoreAudio

func prop<T>(_ id: AudioObjectID, _ sel: AudioObjectPropertySelector, _ scope: AudioObjectPropertyScope = kAudioObjectPropertyScopeGlobal, _ v: inout T) -> OSStatus {
    var a = AudioObjectPropertyAddress(mSelector: sel, mScope: scope, mElement: kAudioObjectPropertyElementMain)
    var size = UInt32(MemoryLayout<T>.size)
    return AudioObjectGetPropertyData(id, &a, 0, nil, &size, &v)
}
func devices() -> [AudioDeviceID] {
    var a = AudioObjectPropertyAddress(mSelector: kAudioHardwarePropertyDevices, mScope: kAudioObjectPropertyScopeGlobal, mElement: kAudioObjectPropertyElementMain)
    var size: UInt32 = 0
    AudioObjectGetPropertyDataSize(AudioObjectID(kAudioObjectSystemObject), &a, 0, nil, &size)
    var ids = [AudioDeviceID](repeating: 0, count: Int(size) / MemoryLayout<AudioDeviceID>.size)
    AudioObjectGetPropertyData(AudioObjectID(kAudioObjectSystemObject), &a, 0, nil, &size, &ids)
    return ids
}
func name(_ id: AudioDeviceID) -> String {
    var n: CFString = "" as CFString
    _ = prop(id, kAudioObjectPropertyName, kAudioObjectPropertyScopeGlobal, &n)
    return n as String
}
func inputChannels(_ id: AudioDeviceID) -> Int {
    var a = AudioObjectPropertyAddress(mSelector: kAudioDevicePropertyStreamConfiguration, mScope: kAudioObjectPropertyScopeInput, mElement: kAudioObjectPropertyElementMain)
    var size: UInt32 = 0
    if AudioObjectGetPropertyDataSize(id, &a, 0, nil, &size) != 0 || size == 0 { return 0 }
    let buf = UnsafeMutableRawPointer.allocate(byteCount: Int(size), alignment: 16)
    defer { buf.deallocate() }
    AudioObjectGetPropertyData(id, &a, 0, nil, &size, buf)
    let list = UnsafeMutableAudioBufferListPointer(buf.assumingMemoryBound(to: AudioBufferList.self))
    return list.reduce(0) { $0 + Int($1.mNumberChannels) }
}
func inputStream(_ id: AudioDeviceID) -> AudioStreamID? {
    var a = AudioObjectPropertyAddress(mSelector: kAudioDevicePropertyStreams, mScope: kAudioObjectPropertyScopeInput, mElement: kAudioObjectPropertyElementMain)
    var size: UInt32 = 0
    if AudioObjectGetPropertyDataSize(id, &a, 0, nil, &size) != 0 || size == 0 { return nil }
    var ids = [AudioStreamID](repeating: 0, count: Int(size) / MemoryLayout<AudioStreamID>.size)
    AudioObjectGetPropertyData(id, &a, 0, nil, &size, &ids)
    return ids.first
}
func formats(_ st: AudioStreamID) -> [AudioStreamRangedDescription] {
    var a = AudioObjectPropertyAddress(mSelector: kAudioStreamPropertyAvailablePhysicalFormats, mScope: kAudioObjectPropertyScopeGlobal, mElement: kAudioObjectPropertyElementMain)
    var size: UInt32 = 0
    if AudioObjectGetPropertyDataSize(st, &a, 0, nil, &size) != 0 { return [] }
    var f = [AudioStreamRangedDescription](repeating: AudioStreamRangedDescription(), count: Int(size) / MemoryLayout<AudioStreamRangedDescription>.size)
    AudioObjectGetPropertyData(st, &a, 0, nil, &size, &f)
    return f
}
func rate(_ id: AudioDeviceID) -> Double { var r: Float64 = 0; _ = prop(id, kAudioDevicePropertyNominalSampleRate, kAudioObjectPropertyScopeGlobal, &r); return r }

let args = CommandLine.arguments
if args.count < 3 {
    for d in devices() where inputChannels(d) > 0 {
        let f = inputStream(d).map { formats($0).map { "\($0.mFormat.mChannelsPerFrame) ch" } } ?? []
        print("\(name(d)): \(inputChannels(d)) in, \(rate(d)) Hz; formats: \(f.joined(separator: ", "))")
    }
    exit(0)
}
let secs = Double(args[1]) ?? 10, want = args.count > 3 ? args[3] : "SiDJ"
guard var dev = devices().first(where: { inputChannels($0) > 0 && name($0).contains(want) }) else { print("no input named \(want)"); exit(2) }
if args.count > 4, let ch = UInt32(args[4]), let st = inputStream(dev) {
    guard var f = formats(st).first(where: { $0.mFormat.mChannelsPerFrame == ch })?.mFormat else { print("no \(ch)-channel format"); exit(4) }
    var a = AudioObjectPropertyAddress(mSelector: kAudioStreamPropertyPhysicalFormat, mScope: kAudioObjectPropertyScopeGlobal, mElement: kAudioObjectPropertyElementMain)
    let r = AudioObjectSetPropertyData(st, &a, 0, nil, UInt32(MemoryLayout<AudioStreamBasicDescription>.size), &f)
    if r != 0 { print("cannot set \(ch) channels: \(r)"); exit(4) }
    Thread.sleep(forTimeInterval: 0.5)
}
let engine = AVAudioEngine()
let input = engine.inputNode
let st = AudioUnitSetProperty(input.audioUnit!, kAudioOutputUnitProperty_CurrentDevice, kAudioUnitScope_Global, 0, &dev, UInt32(MemoryLayout<AudioDeviceID>.size))
if st != 0 { print("cannot select the device: \(st)"); exit(3) }
let fmt = input.inputFormat(forBus: 0)
setvbuf(stdout, nil, _IOLBF, 0)
print("recording \(name(dev)): \(fmt.channelCount) ch, \(fmt.sampleRate) Hz, \(secs) s")
let settings: [String: Any] = [AVFormatIDKey: kAudioFormatLinearPCM, AVSampleRateKey: fmt.sampleRate, AVNumberOfChannelsKey: fmt.channelCount,
                               AVLinearPCMBitDepthKey: 16, AVLinearPCMIsFloatKey: false, AVLinearPCMIsBigEndianKey: false]
var file: AVAudioFile? = try AVAudioFile(forWriting: URL(fileURLWithPath: args[2]), settings: settings, commonFormat: .pcmFormatFloat32, interleaved: false)
var frames: AVAudioFramePosition = 0
input.installTap(onBus: 0, bufferSize: 4096, format: fmt) { buf, _ in try? file?.write(from: buf); frames += AVAudioFramePosition(buf.frameLength) }
try engine.start()
Thread.sleep(forTimeInterval: secs)
engine.stop()
input.removeTap(onBus: 0)
file = nil                                  // closes the file: the WAV header gets its sizes
print("frames \(frames)")
