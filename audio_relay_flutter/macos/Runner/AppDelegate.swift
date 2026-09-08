import Cocoa
import FlutterMacOS
import Foundation
import Network
import CryptoKit
import ScreenCaptureKit
import AVFoundation
import AudioToolbox
import CoreAudio
import OSLog

@main
class AppDelegate: FlutterAppDelegate {
  private var methodChannel: FlutterMethodChannel?

  override func applicationDidFinishLaunching(_ notification: Notification) {
    let controller = mainFlutterWindow?.contentViewController as! FlutterViewController
    let channel = FlutterMethodChannel(name: "com.audiorelay.flutter/desktop", binaryMessenger: controller.engine.binaryMessenger)
    self.methodChannel = channel

    if #available(macOS 13.0, *) {
      let server = MacAudioRelayServer.shared
      server.start()

      channel.setMethodCallHandler { (call, result) in
        switch call.method {
        case "getServerInfo":
          result([
            "pairCode": server.currentPairCode,
            "port": Int(server.port),
            "deviceName": server.deviceName,
            "hasPermission": server.isCapturing
          ])
        case "checkPermission":
          result(server.isCapturing)
        case "requestPermission":
          result(CGRequestScreenCaptureAccess())
        case "openPermissionSettings":
          if let url = URL(string: "x-apple.systempreferences:com.apple.preference.security?Privacy_ScreenCapture") {
            NSWorkspace.shared.open(url)
          }
          result(true)
        case "startCapture":
          server.triggerStartCapture()
          result(true)
        case "regenerateCode":
          server.generateNewPairCode()
          result(server.pairCode)
        case "getOutputDevices":
          result(server.enumerateOutputDevices())
        case "setOutputDevice":
          if let args = call.arguments as? [String: Any], let id = args["id"] as? String {
            server.setOutputDevice(id: id)
          }
          result(true)
        case "setStreamMode":
          if let args = call.arguments as? [String: Any], let mode = args["mode"] as? String {
            server.setStreamMode(mode: mode)
          }
          result(true)
        case "getStreamMode":
          result(server.streamMode)
        default:
          result(FlutterMethodNotImplemented)
        }
      }

      server.onStatusChanged = { status, clientName in
        DispatchQueue.main.async {
          channel.invokeMethod("onStatusChanged", arguments: [
            "status": status,
            "clientName": clientName ?? ""
          ])
        }
      }
    }
  }

  override func applicationShouldTerminateAfterLastWindowClosed(_ sender: NSApplication) -> Bool {
    return true
  }

  override func applicationSupportsSecureRestorableState(_ app: NSApplication) -> Bool {
    return true
  }
}

// MARK: - MacAudioCapture (ScreenCaptureKit)

@available(macOS 13.0, *)
class MacAudioCapture: NSObject, SCStreamOutput, SCStreamDelegate {
    private var stream: SCStream?
    private let sampleQueue = DispatchQueue(label: "com.audiorelay.audio.capture", qos: .userInteractive)
    var onAudioChunk: ((Data) -> Void)?

    func start() async throws {
        let content = try await SCShareableContent.excludingDesktopWindows(false, onScreenWindowsOnly: true)
        guard let display = content.displays.first else {
            throw NSError(domain: "MacAudioCapture", code: -1, userInfo: [NSLocalizedDescriptionKey: "No display found"])
        }

        let filter = SCContentFilter(display: display, excludingWindows: [])
        let config = SCStreamConfiguration()
        config.width = 100
        config.height = 100
        config.minimumFrameInterval = CMTime(value: 1, timescale: 1)
        config.showsCursor = false
        config.capturesAudio = true
        config.excludesCurrentProcessAudio = true
        config.sampleRate = 48000
        config.channelCount = 2

        let scStream = SCStream(filter: filter, configuration: config, delegate: self)
        try scStream.addStreamOutput(self, type: .audio, sampleHandlerQueue: sampleQueue)
        try await scStream.startCapture()
        self.stream = scStream
        os_log("ScreenCaptureKit audio capture started successfully")
    }

    func stop() async {
        if let s = stream {
            try? await s.stopCapture()
            stream = nil
        }
    }

    func stream(_ stream: SCStream, didOutputSampleBuffer sampleBuffer: CMSampleBuffer, of type: SCStreamOutputType) {
        guard type == .audio, sampleBuffer.isValid else { return }

        guard let formatDesc = CMSampleBufferGetFormatDescription(sampleBuffer),
              let asbd = CMAudioFormatDescriptionGetStreamBasicDescription(formatDesc)?.pointee else {
            return
        }

        var bufferListSizeNeeded: Int = 0
        var status = CMSampleBufferGetAudioBufferListWithRetainedBlockBuffer(
            sampleBuffer,
            bufferListSizeNeededOut: &bufferListSizeNeeded,
            bufferListOut: nil,
            bufferListSize: 0,
            blockBufferAllocator: nil,
            blockBufferMemoryAllocator: nil,
            flags: 0,
            blockBufferOut: nil
        )

        guard bufferListSizeNeeded > 0 else { return }

        let rawMem = UnsafeMutableRawPointer.allocate(byteCount: bufferListSizeNeeded, alignment: MemoryLayout<AudioBufferList>.alignment)
        defer { rawMem.deallocate() }
        let ablPtr = rawMem.bindMemory(to: AudioBufferList.self, capacity: 1)

        var blockBuffer: CMBlockBuffer?
        status = CMSampleBufferGetAudioBufferListWithRetainedBlockBuffer(
            sampleBuffer,
            bufferListSizeNeededOut: nil,
            bufferListOut: ablPtr,
            bufferListSize: bufferListSizeNeeded,
            blockBufferAllocator: nil,
            blockBufferMemoryAllocator: nil,
            flags: 0,
            blockBufferOut: &blockBuffer
        )

        guard status == noErr else { return }

        let buffers = UnsafeMutableAudioBufferListPointer(ablPtr)
        let frameCount = CMSampleBufferGetNumSamples(sampleBuffer)
        guard frameCount > 0, !buffers.isEmpty else { return }

        let isFloat = (asbd.mFormatFlags & kAudioFormatFlagIsFloat) != 0
        let isNonInterleaved = (asbd.mFormatFlags & kAudioFormatFlagIsNonInterleaved) != 0

        var pcm16Data = Data(count: frameCount * 2 * MemoryLayout<Int16>.size)
        pcm16Data.withUnsafeMutableBytes { (rawDest: UnsafeMutableRawBufferPointer) in
            let int16Dest = rawDest.bindMemory(to: Int16.self)

            if isFloat {
                if isNonInterleaved && buffers.count >= 2 {
                    guard let lData = buffers[0].mData, let rData = buffers[1].mData else { return }
                    let leftPtr = lData.bindMemory(to: Float32.self, capacity: frameCount)
                    let rightPtr = rData.bindMemory(to: Float32.self, capacity: frameCount)
                    for i in 0..<frameCount {
                        let l = max(-1.0, min(1.0, leftPtr[i]))
                        let r = max(-1.0, min(1.0, rightPtr[i]))
                        int16Dest[i * 2] = Int16(l * 32767.0)
                        int16Dest[i * 2 + 1] = Int16(r * 32767.0)
                    }
                } else if let rawData = buffers[0].mData {
                    let floatSamples = rawData.bindMemory(to: Float32.self, capacity: frameCount * Int(asbd.mChannelsPerFrame))
                    let channels = Int(asbd.mChannelsPerFrame)
                    for i in 0..<frameCount {
                        for ch in 0..<2 {
                            let chIdx = min(ch, channels - 1)
                            let fSample = floatSamples[i * channels + chIdx]
                            let clamped = max(-1.0, min(1.0, fSample))
                            int16Dest[i * 2 + ch] = Int16(clamped * 32767.0)
                        }
                    }
                }
            } else {
                if isNonInterleaved && buffers.count >= 2 {
                    guard let lData = buffers[0].mData, let rData = buffers[1].mData else { return }
                    let leftPtr = lData.bindMemory(to: Int16.self, capacity: frameCount)
                    let rightPtr = rData.bindMemory(to: Int16.self, capacity: frameCount)
                    for i in 0..<frameCount {
                        int16Dest[i * 2] = leftPtr[i]
                        int16Dest[i * 2 + 1] = rightPtr[i]
                    }
                } else if let rawData = buffers[0].mData {
                    let src = rawData.bindMemory(to: Int16.self, capacity: frameCount * 2)
                    for i in 0..<(frameCount * 2) {
                        int16Dest[i] = src[i]
                    }
                }
            }
        }

        onAudioChunk?(pcm16Data)
    }
}

// MARK: - MacAudioPlayer (AudioQueue for virtual audio output)

// Sequence-aware jitter buffer for the microphone receive path. Operates on
// mono int16 PCM frames and mirrors the Windows `JitterBuffer` semantics:
// reorder, pre-buffer, conceal gaps, shed backlog, and recover from desync.
final class MacJitterBuffer {
    private let lock = NSLock()
    private var chunks: [UInt32: [Int16]] = [:]
    private var nextSeq: UInt32 = 0
    private var started = false
    private var chunkFrames = 0
    private var chunkPos = 0
    private var concealCount: UInt32 = 0
    private let targetDepth: Int

    init(targetDepth: Int = 3) {
        self.targetDepth = max(1, targetDepth)
    }

    func reset() {
        lock.lock(); defer { lock.unlock() }
        resetLocked()
    }

    func push(sequence: UInt32, frames: [Int16]) {
        guard !frames.isEmpty else { return }
        lock.lock(); defer { lock.unlock() }

        if started {
            let dist = Int64(sequence) - Int64(nextSeq)
            if dist < -200 || dist > 200 {
                resetLocked()
            } else if dist < 0 {
                return  // late or duplicate
            }
        }

        guard chunks[sequence] == nil else { return }  // duplicate
        if chunks.count >= 64 {
            chunks.removeValue(forKey: chunks.keys.min()!)
        }
        if chunkFrames == 0 {
            chunkFrames = frames.count
        }
        chunks[sequence] = frames
    }

    func pop(maxFrames: Int) -> [Int16] {
        lock.lock(); defer { lock.unlock() }

        if !started {
            if chunks.count < targetDepth { return [] }
            started = true
            nextSeq = chunks.keys.min()!
            chunkPos = 0
            concealCount = 0
            if chunkFrames == 0, let first = chunks.values.first {
                chunkFrames = first.count
            }
        }

        var out: [Int16] = []
        out.reserveCapacity(maxFrames)

        while out.count < maxFrames {
            // Shed standing backlog before it becomes permanent latency.
            if chunkPos == 0 && chunks.count > targetDepth * 2 {
                while chunks.count > targetDepth {
                    chunks.removeValue(forKey: chunks.keys.min()!)
                }
                if !chunks.isEmpty { nextSeq = chunks.keys.min()! }
                chunkPos = 0
                concealCount = 0
            }

            if let chunk = chunks[nextSeq] {
                let remaining = chunk.count - chunkPos
                let n = min(remaining, maxFrames - out.count)
                out.append(contentsOf: chunk[chunkPos..<(chunkPos + n)])
                chunkPos += n
                if chunkPos >= chunk.count {
                    chunks.removeValue(forKey: nextSeq)
                    nextSeq &+= 1
                    chunkPos = 0
                    concealCount = 0
                }
            } else {
                // Conceal a missing packet with exactly one packet of silence.
                let concealLen = chunkFrames == 0 ? 480 : chunkFrames
                let remaining = concealLen - chunkPos
                let n = min(remaining, maxFrames - out.count)
                out.append(contentsOf: repeatElement(Int16(0), count: n))
                chunkPos += n
                if chunkPos >= concealLen {
                    nextSeq &+= 1
                    chunkPos = 0
                    concealCount += 1
                    if concealCount > 50 {
                        resetLocked()
                        return out
                    }
                }
            }
        }
        return out
    }

    private func resetLocked() {
        chunks.removeAll()
        nextSeq = 0
        started = false
        chunkFrames = 0
        chunkPos = 0
        concealCount = 0
    }
}

class MacAudioPlayer {
    private var queue: AudioQueueRef?
    private(set) var isRunning: Bool = false
    private var currentDeviceUID: String?

    private let bufferCount = 4
    private let bufferByteSize: UInt32 = 4096
    private var freeBuffers: [AudioQueueBufferRef] = []
    private let bufferLock = NSLock()

    private let jitter = MacJitterBuffer(targetDepth: 3)
    private let pumpQueue = DispatchQueue(label: "com.audiorelay.mac.player")
    private var pumpTimer: DispatchSourceTimer?

    func start(deviceUID: String? = nil) {
        stop()
        currentDeviceUID = deviceUID

        var desc = AudioStreamBasicDescription(
            mSampleRate: 48000.0,
            mFormatID: kAudioFormatLinearPCM,
            mFormatFlags: kAudioFormatFlagIsSignedInteger | kAudioFormatFlagIsPacked,
            mBytesPerPacket: 4,
            mFramesPerPacket: 1,
            mBytesPerFrame: 4,
            mChannelsPerFrame: 2,
            mBitsPerChannel: 16,
            mReserved: 0
        )

        let callback: AudioQueueOutputCallback = { userData, inAQ, inBuffer in
            guard let ptr = userData else { return }
            let player = Unmanaged<MacAudioPlayer>.fromOpaque(ptr).takeUnretainedValue()
            player.recycleBuffer(inBuffer)
        }

        let userPtr = UnsafeMutableRawPointer(Unmanaged.passUnretained(self).toOpaque())
        let status = AudioQueueNewOutput(&desc, callback, userPtr, nil, nil, 0, &queue)
        guard status == noErr, let q = queue else {
            os_log("Failed to create AudioQueue for output: %d", status)
            return
        }

        if let uid = deviceUID, !uid.isEmpty {
            var cfUid = uid as CFString
            AudioQueueSetProperty(q, kAudioQueueProperty_CurrentDevice, &cfUid, UInt32(MemoryLayout<CFString>.size))
        }

        // Pre-allocate buffer pool to eliminate heap allocations on the audio hot-path
        jitter.reset()
        bufferLock.lock()
        freeBuffers.removeAll()
        for _ in 0..<bufferCount {
            var buf: AudioQueueBufferRef?
            if AudioQueueAllocateBuffer(q, bufferByteSize, &buf) == noErr, let b = buf {
                freeBuffers.append(b)
            }
        }
        bufferLock.unlock()

        AudioQueueStart(q, nil)
        isRunning = true

        let timer = DispatchSource.makeTimerSource(queue: pumpQueue)
        timer.schedule(deadline: .now(), repeating: .milliseconds(10))
        timer.setEventHandler { [weak self] in
            self?.pump()
        }
        timer.resume()
        pumpTimer = timer
    }

    fileprivate func recycleBuffer(_ buffer: AudioQueueBufferRef) {
        bufferLock.lock()
        freeBuffers.append(buffer)
        bufferLock.unlock()
    }

    func stop() {
        pumpTimer?.cancel()
        pumpTimer = nil
        isRunning = false
        if let q = queue {
            AudioQueueStop(q, true)
            AudioQueueDispose(q, true)
            queue = nil
        }
        bufferLock.lock()
        freeBuffers.removeAll()
        bufferLock.unlock()
    }

    func pushPcm(_ sequence: UInt32, mono: [Int16]) {
        jitter.push(sequence: sequence, frames: mono)
    }

    private func pump() {
        guard let q = queue, isRunning else { return }

        bufferLock.lock()
        let buf = freeBuffers.popLast()
        bufferLock.unlock()
        guard let buf = buf else { return }

        // One AudioQueue buffer holds 4096 bytes == 1024 stereo frames == 1024
        // mono samples. Pop up to that many mono frames from the jitter buffer.
        let maxFrames = Int(bufferByteSize) / 4
        let mono = jitter.pop(maxFrames: maxFrames)
        if mono.isEmpty {
            bufferLock.lock()
            freeBuffers.append(buf)
            bufferLock.unlock()
            return
        }

        // Duplicate mono -> stereo (L=R) for BlackHole 2ch compatibility.
        var stereo = [Int16](repeating: 0, count: mono.count * 2)
        for i in 0..<mono.count {
            stereo[i * 2] = mono[i]
            stereo[i * 2 + 1] = mono[i]
        }
        let byteCount = stereo.count * MemoryLayout<Int16>.size
        stereo.withUnsafeBytes { raw in
            buf.pointee.mAudioData.copyMemory(from: raw.baseAddress!, byteCount: byteCount)
        }
        buf.pointee.mAudioDataByteSize = UInt32(byteCount)
        AudioQueueEnqueueBuffer(q, buf, 0, nil)
    }
}

// MARK: - MacAudioRelayServer

@available(macOS 13.0, *)
class MacAudioRelayServer {
    static let shared = MacAudioRelayServer()

    let port: UInt16 = 45108
    let audioTcpPort: UInt16 = 45109

    private(set) var streamMode: String = "speaker"
    private var selectedOutputDeviceUID: String?
    private var player = MacAudioPlayer()

    private(set) var pairCode: String = ""
    private(set) var pairCodeCreatedAt: Date = Date()
    var currentPairCode: String {
        stateLock.lock()
        let expired = Date().timeIntervalSince(pairCodeCreatedAt) > 300 || pairCode.isEmpty
        let code = pairCode
        stateLock.unlock()
        if expired {
            generateNewPairCode()
            stateLock.lock()
            defer { stateLock.unlock() }
            return pairCode
        }
        return code
    }
    private(set) var deviceId: String = UUID().uuidString
    private(set) var deviceName: String = Host.current().localizedName ?? "MacBook"
    
    private var tcpListener: NWListener?
    private var tcpAudioListener: NWListener?

    private var activeControlConnection: NWConnection?
    private var activeUdpConnection: NWConnection?
    private var activeAudioTcpConnection: NWConnection?

    private var capture = MacAudioCapture()
    private var adbTimer: Timer?

    private var sessionKey: SymmetricKey?
    private var sessionId: Data?
    private var sequence: UInt32 = 0
    private var currentNonce: String = ""
    private var clientDeviceId: String = ""
    private var pairedKeys: [String: SymmetricKey] = [:]
    private var controlBuffer: String = ""
    private let stateLock = NSLock()

    var onStatusChanged: ((String, String?) -> Void)?

    private init() {
        loadConfig()
        generateNewPairCode()
    }

    private var configURL: URL {
        let appSupport = FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask).first!
        let dir = appSupport.appendingPathComponent("AudioRelay", isDirectory: true)
        try? FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
        return dir.appendingPathComponent("config.json")
    }

    private func loadConfig() {
        guard let data = try? Data(contentsOf: configURL),
              let json = try? JSONSerialization.jsonObject(with: data) as? [String: Any] else {
            return
        }

        if let savedId = json["device_id"] as? String, !savedId.isEmpty {
            self.deviceId = savedId
        }

        if let keys = json["paired_keys"] as? [String: String] {
            for (id, hexKey) in keys {
                if let keyData = dataFromHex(hexKey), keyData.count == 32 {
                    pairedKeys[id] = SymmetricKey(data: keyData)
                }
            }
        }
    }

    private func saveConfig() {
        var keysHex: [String: String] = [:]
        for (id, symKey) in pairedKeys {
            let hex = symKey.withUnsafeBytes { ptr in
                ptr.map { String(format: "%02x", $0) }.joined()
            }
            keysHex[id] = hex
        }

        let dict: [String: Any] = [
            "device_id": deviceId,
            "paired_keys": keysHex
        ]

        if let data = try? JSONSerialization.data(withJSONObject: dict, options: [.prettyPrinted]) {
            try? data.write(to: configURL)
        }
    }

    private func dataFromHex(_ hex: String) -> Data? {
        var data = Data()
        var idx = hex.startIndex
        while idx < hex.endIndex {
            guard let nextIdx = hex.index(idx, offsetBy: 2, limitedBy: hex.endIndex) else { return nil }
            guard let byte = UInt8(hex[idx..<nextIdx], radix: 16) else { return nil }
            data.append(byte)
            idx = nextIdx
        }
        return data
    }

    func generateNewPairCode() {
        var num: UInt32 = 0
        repeat {
            _ = SecRandomCopyBytes(kSecRandomDefault, 4, &num)
        } while num >= 4294000000
        let newCode = String(format: "%06d", num % 1000000)
        stateLock.lock()
        pairCode = newCode
        pairCodeCreatedAt = Date()
        stateLock.unlock()
    }

    func start() {
        startTcpControlListener()
        startTcpAudioListener()
        startCapture()
        startAdbSupervisor()
    }

    func stop() {
        adbTimer?.invalidate()
        tcpListener?.cancel()
        tcpAudioListener?.cancel()
        stateLock.lock()
        let ctrl = activeControlConnection
        activeControlConnection = nil
        let udp = activeUdpConnection
        activeUdpConnection = nil
        let audioTcp = activeAudioTcpConnection
        activeAudioTcpConnection = nil
        sessionKey = nil
        sessionId = nil
        stateLock.unlock()

        ctrl?.cancel()
        udp?.cancel()
        audioTcp?.cancel()
        Task {
            await capture.stop()
        }
    }

    private func findAdbPath() -> String? {
        let home = NSHomeDirectory()
        let candidates = [
            "\(home)/Library/Android/sdk/platform-tools/adb",
            "/opt/homebrew/bin/adb",
            "/usr/local/bin/adb",
            "/usr/bin/adb"
        ]
        for p in candidates {
            if FileManager.default.fileExists(atPath: p) {
                return p
            }
        }
        return nil
    }

    private func startAdbSupervisor() {
        guard let adb = findAdbPath() else {
            os_log("ADB executable not found in standard paths")
            return
        }

        func executeAdbReverse() {
            let p1 = Process()
            p1.executableURL = URL(fileURLWithPath: adb)
            p1.arguments = ["reverse", "tcp:45108", "tcp:45108"]
            try? p1.run()
            p1.waitUntilExit()

            let p2 = Process()
            p2.executableURL = URL(fileURLWithPath: adb)
            p2.arguments = ["reverse", "tcp:45109", "tcp:45109"]
            try? p2.run()
            p2.waitUntilExit()
        }

        DispatchQueue.global().async {
            executeAdbReverse()
        }

        DispatchQueue.main.async { [weak self] in
            self?.adbTimer?.invalidate()
            self?.adbTimer = Timer.scheduledTimer(withTimeInterval: 2.0, repeats: true) { _ in
                DispatchQueue.global().async {
                    executeAdbReverse()
                }
            }
        }
    }

    private var _isCapturing = false
    var isCapturing: Bool {
        get {
            stateLock.lock()
            defer { stateLock.unlock() }
            return _isCapturing
        }
        set {
            stateLock.lock()
            _isCapturing = newValue
            stateLock.unlock()
        }
    }

    func enumerateOutputDevices() -> [[String: Any]] {
        var devices: [[String: Any]] = []
        var propertySize: UInt32 = 0
        var address = AudioObjectPropertyAddress(
            mSelector: kAudioHardwarePropertyDevices,
            mScope: kAudioObjectPropertyScopeGlobal,
            mElement: kAudioObjectPropertyElementMain
        )

        let status = AudioObjectGetPropertyDataSize(AudioObjectID(kAudioObjectSystemObject), &address, 0, nil, &propertySize)
        guard status == noErr else { return devices }

        let count = Int(propertySize / UInt32(MemoryLayout<AudioDeviceID>.size))
        var deviceIDs = [AudioDeviceID](repeating: 0, count: count)
        let getStatus = AudioObjectGetPropertyData(AudioObjectID(kAudioObjectSystemObject), &address, 0, nil, &propertySize, &deviceIDs)
        guard getStatus == noErr else { return devices }

        var defaultOutputDeviceID: AudioDeviceID = 0
        var defaultSize = UInt32(MemoryLayout<AudioDeviceID>.size)
        var defaultAddress = AudioObjectPropertyAddress(
            mSelector: kAudioHardwarePropertyDefaultOutputDevice,
            mScope: kAudioObjectPropertyScopeGlobal,
            mElement: kAudioObjectPropertyElementMain
        )
        AudioObjectGetPropertyData(AudioObjectID(kAudioObjectSystemObject), &defaultAddress, 0, nil, &defaultSize, &defaultOutputDeviceID)

        for devId in deviceIDs {
            // Check if device has output streams
            var streamAddress = AudioObjectPropertyAddress(
                mSelector: kAudioDevicePropertyStreams,
                mScope: kAudioObjectPropertyScopeOutput,
                mElement: kAudioObjectPropertyElementMain
            )
            var streamSize: UInt32 = 0
            if AudioObjectGetPropertyDataSize(devId, &streamAddress, 0, nil, &streamSize) != noErr || streamSize == 0 {
                continue
            }

            // Name
            var nameAddress = AudioObjectPropertyAddress(
                mSelector: kAudioDevicePropertyDeviceNameCFString,
                mScope: kAudioObjectPropertyScopeGlobal,
                mElement: kAudioObjectPropertyElementMain
            )
            var nameCF: CFString = "" as CFString
            var nameSize = UInt32(MemoryLayout<CFString>.size)
            if AudioObjectGetPropertyData(devId, &nameAddress, 0, nil, &nameSize, &nameCF) != noErr {
                continue
            }
            let name = nameCF as String

            // UID
            var uidAddress = AudioObjectPropertyAddress(
                mSelector: kAudioDevicePropertyDeviceUID,
                mScope: kAudioObjectPropertyScopeGlobal,
                mElement: kAudioObjectPropertyElementMain
            )
            var uidCF: CFString = "" as CFString
            var uidSize = UInt32(MemoryLayout<CFString>.size)
            if AudioObjectGetPropertyData(devId, &uidAddress, 0, nil, &uidSize, &uidCF) != noErr {
                continue
            }
            let uid = uidCF as String

            let isDefault = (devId == defaultOutputDeviceID)
            let lower = name.lowercased()
            let isVirtual = lower.contains("blackhole") || lower.contains("vb-audio") ||
                            lower.contains("cable") || lower.contains("soundflower") ||
                            lower.contains("loopback") || lower.contains("virtual")

            devices.append([
                "id": uid,
                "name": name,
                "is_default": isDefault,
                "is_virtual": isVirtual
            ])
        }

        return devices
    }

    func setOutputDevice(id: String) {
        stateLock.lock()
        selectedOutputDeviceUID = id
        stateLock.unlock()
        if streamMode == "microphone" && player.isRunning {
            player.start(deviceUID: id)
        }
    }

    func autoDetectVirtualDevice() -> String? {
        let devices = enumerateOutputDevices()
        for d in devices {
            if let isVirt = d["is_virtual"] as? Bool, isVirt, let uid = d["id"] as? String {
                return uid
            }
        }
        return nil
    }

    func setStreamMode(mode: String) {
        stateLock.lock()
        streamMode = mode
        stateLock.unlock()

        if mode == "microphone" {
            Task {
                await capture.stop()
            }
            setCapturingState(false)
            let targetUID = selectedOutputDeviceUID ?? autoDetectVirtualDevice()
            player.start(deviceUID: targetUID)
        } else if mode == "duplex" {
            let targetUID = selectedOutputDeviceUID ?? autoDetectVirtualDevice()
            player.start(deviceUID: targetUID)
            triggerStartCapture()
        } else {
            player.stop()
            triggerStartCapture()
        }
    }

    private func processIncomingAudioPacket(_ data: Data) {
        guard data.count >= 13 + 16 else { return } // header + tag

        let header = data.subdata(in: 0..<13)
        let ciphertextAndTag = data.subdata(in: 13..<data.count)

        stateLock.lock()
        guard let key = sessionKey, let sessId = sessionId else {
            stateLock.unlock()
            return
        }
        stateLock.unlock()

        let seqBE = header.subdata(in: 1..<5)
        let sequence = seqBE.withUnsafeBytes { $0.load(as: UInt32.self).bigEndian }

        var nonceData = Data(sessId)
        var seqVal = sequence
        // Incoming packet is Phone -> Laptop (Microphone): set Direction Bit (bit 31 = 1)
        seqVal |= 0x80000000
        var dirSeqBE = seqVal.bigEndian
        withUnsafeBytes(of: &dirSeqBE) { nonceData.append(contentsOf: $0) }

        guard let nonce = try? ChaChaPoly.Nonce(data: nonceData) else { return }

        let tagIndex = ciphertextAndTag.count - 16
        let ciphertext = ciphertextAndTag.subdata(in: 0..<tagIndex)
        let tag = ciphertextAndTag.subdata(in: tagIndex..<ciphertextAndTag.count)

        guard let sealedBox = try? ChaChaPoly.SealedBox(nonce: nonce, ciphertext: ciphertext, tag: tag) else {
            return
        }

        guard let decrypted = try? ChaChaPoly.open(sealedBox, using: key, authenticating: header) else {
            return
        }

        if streamMode == "microphone" || streamMode == "duplex" {
            // The Android microphone sender always transmits mono 16-bit PCM.
            let samples = decrypted.withUnsafeBytes { raw -> [Int16] in
                guard let baseAddr = raw.baseAddress else { return [] }
                let p = baseAddr.bindMemory(to: Int16.self, capacity: raw.count / 2)
                return Array(UnsafeBufferPointer(start: p, count: raw.count / 2))
            }
            player.pushPcm(sequence, mono: samples)
        }
    }

    func listenUdpAudio(conn: NWConnection) {
        conn.receiveMessage { [weak self] content, _, isComplete, error in
            guard let self = self else { return }
            if let data = content, !data.isEmpty {
                self.processIncomingAudioPacket(data)
            }
            if error == nil && !isComplete {
                self.listenUdpAudio(conn: conn)
            }
        }
    }

    func listenTcpAudio(conn: NWConnection) {
        conn.receive(minimumIncompleteLength: 2, maximumLength: 2) { [weak self] lenData, _, isComplete, error in
            guard let self = self, let lenData = lenData, lenData.count == 2, error == nil else { return }
            let len = Int(lenData[0]) << 8 | Int(lenData[1])
            conn.receive(minimumIncompleteLength: len, maximumLength: len) { [weak self] bodyData, _, isComp, err in
                guard let self = self, let bodyData = bodyData, bodyData.count == len, err == nil else { return }
                self.processIncomingAudioPacket(bodyData)
                self.listenTcpAudio(conn: conn)
            }
        }
    }

    func triggerStartCapture() {
        if streamMode == "microphone" {
            return
        }
        stateLock.lock()
        if _isCapturing {
            stateLock.unlock()
            return
        }
        _isCapturing = true
        stateLock.unlock()

        capture.onAudioChunk = { [weak self] chunk in
            self?.sendAudioFrame(pcm: chunk)
        }
        Task {
            do {
                try await capture.start()
                os_log("ScreenCaptureKit audio capture started successfully")
            } catch {
                os_log("Failed to start ScreenCaptureKit: %{public}@", error.localizedDescription)
                self.setCapturingState(false)
            }
        }
    }

    private func setCapturingState(_ state: Bool) {
        stateLock.lock()
        _isCapturing = state
        stateLock.unlock()
    }

    private func startCapture() {
        if streamMode != "microphone" {
            triggerStartCapture()
        }
    }

    private func startTcpControlListener() {
        do {
            let params = NWParameters.tcp
            params.allowLocalEndpointReuse = true
            let listener = try NWListener(using: params, on: NWEndpoint.Port(rawValue: port)!)
            let txtDict = [
                "id": deviceId,
                "name": deviceName,
                "protocol_version": "2"
            ]
            let txtData = NetService.data(fromTXTRecord: txtDict.mapValues { $0.data(using: .utf8)! })
            listener.service = NWListener.Service(name: deviceName, type: "_audiorelay._udp", domain: nil, txtRecord: txtData)

            listener.newConnectionHandler = { [weak self] newConn in
                self?.handleNewControlConnection(newConn)
            }
            listener.stateUpdateHandler = { [weak self] state in
                if case .ready = state {
                    os_log("TCP Control Channel listening on port %d with mDNS", self?.port ?? 45108)
                    self?.onStatusChanged?("listening", nil)
                }
            }
            listener.start(queue: .main)
            self.tcpListener = listener
        } catch {
            os_log("Failed to create TCP control listener: %{public}@", error.localizedDescription)
        }
    }

    private func startTcpAudioListener() {
        do {
            let params = NWParameters.tcp
            params.allowLocalEndpointReuse = true
            let listener = try NWListener(using: params, on: NWEndpoint.Port(rawValue: audioTcpPort)!)
            listener.newConnectionHandler = { [weak self] newConn in
                guard let self = self else { return }
                self.stateLock.lock()
                let oldConn = self.activeAudioTcpConnection
                self.activeAudioTcpConnection = newConn
                self.stateLock.unlock()
                oldConn?.cancel()
                newConn.start(queue: .global())
                os_log("TCP Audio Channel connected on port %d", self.audioTcpPort)
                self.listenTcpAudio(conn: newConn)
            }
            listener.start(queue: .main)
            self.tcpAudioListener = listener
        } catch {
            os_log("Failed to create TCP audio listener: %{public}@", error.localizedDescription)
        }
    }

    private func handleNewControlConnection(_ conn: NWConnection) {
        stateLock.lock()
        let oldCtrl = activeControlConnection
        activeControlConnection = conn
        controlBuffer = ""
        stateLock.unlock()
        oldCtrl?.cancel()
        conn.stateUpdateHandler = { [weak self] state in
            switch state {
            case .failed, .cancelled:
                self?.onStatusChanged?("listening", nil)
            default:
                break
            }
        }
        conn.start(queue: .main)
        readNextLine(from: conn)
    }

    private func readNextLine(from conn: NWConnection) {
        conn.receive(minimumIncompleteLength: 1, maximumLength: 4096) { [weak self] content, _, isComplete, error in
            guard let self = self else { return }

            if let data = content, !data.isEmpty, let chunk = String(data: data, encoding: .utf8) {
                self.controlBuffer.append(chunk)
                while let range = self.controlBuffer.range(of: "\n") {
                    let line = String(self.controlBuffer[..<range.lowerBound]).trimmingCharacters(in: .whitespacesAndNewlines)
                    self.controlBuffer.removeSubrange(..<range.upperBound)
                    if !line.isEmpty {
                        self.processControlMessage(line, conn: conn)
                    }
                }
            }

            if error != nil || isComplete {
                self.onStatusChanged?("listening", nil)
                return
            }

            self.readNextLine(from: conn)
        }
    }

    private func processControlMessage(_ line: String, conn: NWConnection) {
        guard let data = line.data(using: .utf8),
              let json = try? JSONSerialization.jsonObject(with: data) as? [String: Any],
              let type = json["type"] as? String else {
            return
        }

        switch type {
        case "HELLO":
            handleHello(json, conn: conn)
        case "PAIR_REQUEST":
            handlePairRequest(json, conn: conn)
        case "REPAIR":
            handleRepair(json, conn: conn)
        case "CAPABILITIES":
            break
        case "PING":
            let t = json["t"] as? Int64 ?? 0
            sendJson(["type": "PONG", "t": t], to: conn)
        case "BYE":
            stateLock.lock()
            let ctrl = activeControlConnection
            activeControlConnection = nil
            let udp = activeUdpConnection
            activeUdpConnection = nil
            let audioTcp = activeAudioTcpConnection
            activeAudioTcpConnection = nil
            sessionKey = nil
            sessionId = nil
            stateLock.unlock()

            ctrl?.cancel()
            udp?.cancel()
            audioTcp?.cancel()
            onStatusChanged?("listening", nil)
        default:
            break
        }
    }

    private func handleHello(_ json: [String: Any], conn: NWConnection) {
        let clientName = json["device_name"] as? String ?? "Android"
        let clientAudioPort = json["audio_port"] as? Int ?? 45108
        let reqDevId = json["device_id"] as? String ?? ""
        if let mode = json["stream_mode"] as? String {
            setStreamMode(mode: mode)
        }

        var nonceBytes = [UInt8](repeating: 0, count: 8)
        _ = SecRandomCopyBytes(kSecRandomDefault, 8, &nonceBytes)
        let generatedNonce = nonceBytes.map { String(format: "%02x", $0) }.joined()

        stateLock.lock()
        clientDeviceId = reqDevId
        currentNonce = generatedNonce
        let isPaired = (pairedKeys[clientDeviceId] != nil)
        stateLock.unlock()

        let ack: [String: Any] = [
            "type": "HELLO_ACK",
            "protocol_version": 2,
            "device_id": deviceId,
            "device_name": deviceName,
            "paired": isPaired,
            "nonce": generatedNonce
        ]
        sendJson(ack, to: conn)

        if case let .hostPort(host, _) = conn.endpoint {
            let udpEndpoint = NWEndpoint.hostPort(host: host, port: NWEndpoint.Port(rawValue: UInt16(clientAudioPort))!)
            let udpConn = NWConnection(to: udpEndpoint, using: .udp)
            udpConn.start(queue: .global())
            stateLock.lock()
            self.activeUdpConnection?.cancel()
            self.activeUdpConnection = udpConn
            stateLock.unlock()
            listenUdpAudio(conn: udpConn)
        }

        onStatusChanged?("pairing", clientName)
    }

    private func handlePairRequest(_ json: [String: Any], conn: NWConnection) {
        stateLock.lock()
        let elapsed = Date().timeIntervalSince(pairCodeCreatedAt)
        if elapsed > 300 || pairCode.isEmpty {
            stateLock.unlock()
            generateNewPairCode()
            sendJson(["type": "PAIR_FAIL", "reason": "code_expired"], to: conn)
            return
        }
        let currentCode = pairCode
        let currentClientDevId = clientDeviceId
        let nonce = currentNonce
        stateLock.unlock()

        let clientProof = json["proof"] as? String ?? ""

        let expectedMsg = (currentClientDevId + nonce).data(using: .utf8)!
        let pairCodeKey = SymmetricKey(data: currentCode.data(using: .utf8)!)

        guard let proofData = dataFromHex(clientProof),
              HMAC<SHA256>.isValidAuthenticationCode(proofData, authenticating: expectedMsg, using: pairCodeKey) else {
            sendJson(["type": "PAIR_FAIL", "reason": "invalid_code"], to: conn)
            return
        }

        var sessIdBytes = [UInt8](repeating: 0, count: 8)
        _ = SecRandomCopyBytes(kSecRandomDefault, 8, &sessIdBytes)
        let newSessionId = Data(sessIdBytes)
        let sessIdHex = sessIdBytes.map { String(format: "%02x", $0) }.joined()

        let salt = (currentClientDevId + deviceId).data(using: .utf8)!
        let codeData = currentCode.data(using: .utf8)!
        let prk = SymmetricKey(data: HKDF<SHA256>.deriveKey(
            inputKeyMaterial: SymmetricKey(data: codeData),
            salt: salt,
            info: "audio-relay-session-v1".data(using: .utf8)!,
            outputByteCount: 32
        ))

        stateLock.lock()
        self.sequence = 0
        self.sessionId = newSessionId
        self.sessionKey = prk
        pairedKeys[currentClientDevId] = prk
        saveConfig()
        stateLock.unlock()

        let ok: [String: Any] = [
            "type": "PAIR_OK",
            "session_id": sessIdHex
        ]
        sendJson(ok, to: conn)

        let caps: [String: Any] = [
            "type": "CAPABILITIES",
            "sample_rate": 48000,
            "channels": 2,
            "stream_mode": streamMode
        ]
        sendJson(caps, to: conn)

        if streamMode == "microphone" {
            let targetUID = selectedOutputDeviceUID ?? autoDetectVirtualDevice()
            player.start(deviceUID: targetUID)
        } else if streamMode == "duplex" {
            let targetUID = selectedOutputDeviceUID ?? autoDetectVirtualDevice()
            player.start(deviceUID: targetUID)
            triggerStartCapture()
        } else {
            triggerStartCapture()
        }
        onStatusChanged?("streaming", "Android 手机")
    }

    private func handleRepair(_ json: [String: Any], conn: NWConnection) {
        stateLock.lock()
        let fallbackDevId = clientDeviceId
        let nonce = currentNonce
        let reqDeviceId = json["device_id"] as? String ?? fallbackDevId
        let savedKey = pairedKeys[reqDeviceId]
        stateLock.unlock()

        guard let key = savedKey else {
            // Key not found or expired; reject repair so client prompts for pairing code!
            sendJson(["type": "PAIR_FAIL", "reason": "key_expired_or_not_found"], to: conn)
            return
        }

        let clientProof = json["proof"] as? String ?? ""
        let expectedMsg = (reqDeviceId + nonce).data(using: .utf8)!

        guard let proofData = dataFromHex(clientProof),
              HMAC<SHA256>.isValidAuthenticationCode(proofData, authenticating: expectedMsg, using: key) else {
            sendJson(["type": "PAIR_FAIL", "reason": "invalid_proof"], to: conn)
            return
        }

        var sessIdBytes = [UInt8](repeating: 0, count: 8)
        _ = SecRandomCopyBytes(kSecRandomDefault, 8, &sessIdBytes)
        let newSessionId = Data(sessIdBytes)
        let sessIdHex = sessIdBytes.map { String(format: "%02x", $0) }.joined()

        stateLock.lock()
        self.sequence = 0
        self.sessionKey = key
        self.sessionId = newSessionId
        stateLock.unlock()

        sendJson(["type": "PAIR_OK", "session_id": sessIdHex], to: conn)
        let caps: [String: Any] = [
            "type": "CAPABILITIES",
            "sample_rate": 48000,
            "channels": 2,
            "stream_mode": streamMode
        ]
        sendJson(caps, to: conn)
        if streamMode == "microphone" {
            let targetUID = selectedOutputDeviceUID ?? autoDetectVirtualDevice()
            player.start(deviceUID: targetUID)
        } else if streamMode == "duplex" {
            let targetUID = selectedOutputDeviceUID ?? autoDetectVirtualDevice()
            player.start(deviceUID: targetUID)
            triggerStartCapture()
        } else {
            triggerStartCapture()
        }
        onStatusChanged?("streaming", "Android 手机")
    }

    private func sendJson(_ dict: [String: Any], to conn: NWConnection) {
        guard let data = try? JSONSerialization.data(withJSONObject: dict) else { return }
        var packet = data
        packet.append(0x0A)
        conn.send(content: packet, completion: .idempotent)
    }

    private func sendAudioFrame(pcm: Data) {
        stateLock.lock()
        guard let key = sessionKey, let sessId = sessionId else {
            stateLock.unlock()
            return
        }

        sequence &+= 1
        let seq = sequence
        let udpConn = activeUdpConnection
        let tcpConn = activeAudioTcpConnection
        stateLock.unlock()

        let tsMs = UInt32(truncatingIfNeeded: UInt64(ProcessInfo.processInfo.systemUptime * 1000))

        var header = Data(count: 13)
        header[0] = 0x00
        var seqBE = seq.bigEndian
        withUnsafeBytes(of: &seqBE) { header.replaceSubrange(1..<5, with: $0) }
        var tsBE = tsMs.bigEndian
        withUnsafeBytes(of: &tsBE) { header.replaceSubrange(5..<9, with: $0) }
        header[9] = 0x01
        header[10] = 0x02
        header[11] = 0x00
        header[12] = 0x00

        var nonceData = Data(sessId)
        withUnsafeBytes(of: &seqBE) { nonceData.append(contentsOf: $0) }

        guard let chachaNonce = try? ChaChaPoly.Nonce(data: nonceData) else { return }

        guard let sealedBox = try? ChaChaPoly.seal(pcm, using: key, nonce: chachaNonce, authenticating: header) else {
            return
        }

        var datagram = header
        datagram.append(sealedBox.ciphertext)
        datagram.append(sealedBox.tag)

        if seq % 100 == 0 {
            os_log("Relay sent frame #%d (%d bytes PCM -> %d bytes encrypted)", seq, pcm.count, datagram.count)
        }

        // 1. Send to UDP client (if LAN / Wi-Fi)
        if let udp = udpConn {
            udp.send(content: datagram, completion: .idempotent)
        }

        // 2. Send to TCP client (if USB cable / ADB reverse) with 2-byte length prefix
        if let tcp = tcpConn {
            var lengthPrefixed = Data(count: 2)
            var lenBE = UInt16(datagram.count).bigEndian
            withUnsafeBytes(of: &lenBE) { lengthPrefixed.replaceSubrange(0..<2, with: $0) }
            lengthPrefixed.append(datagram)
            tcp.send(content: lengthPrefixed, completion: .idempotent)
        }
    }
}
