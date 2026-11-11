// lagrec: record the Mac camera's view of the Launchkey display and the Launchkey's incoming MIDI
// on ONE clock (mach host time, in nanoseconds: the same clock as Python's time.monotonic_ns()).
//
//   swiftc -O tools/lagrec.swift -o <somewhere>/lagrec
//   lagrec --list                                   camera formats and MIDI sources
//   lagrec --out DIR [--box X,Y,W,H] [--no-camera] [--duration S] [--camera NAME] [--format N]
//
//   --box        where the display is, in 1920x1080 picture coordinates (scaled to the format in use)
//   --no-camera  MIDI only
//   --format     index from --list (default: the highest frame rate, 16:9, widest)
//
// Writes into DIR:
//   midi.csv     host_ns,source,hex        every packet from every source whose name contains "Launchkey"
//   frames.csv   index,pts_ns,arrival_ns   one line per camera frame (pts = capture time on the host clock)
//   frames.bin   128x64 bytes per frame: the box, averaged down, luma only
//   full.png     one whole frame (grey) taken about 0.7 s after the start
//   snap-NN.png  the box at camera resolution (x3), one per SIGUSR1  (`pkill -USR1 lagrec`)
//   meta.txt     format, frame rate, box, clock info
// Stops cleanly on SIGINT / SIGTERM (or after --duration).
import AVFoundation
import CoreMIDI
import Foundation
import ImageIO

var tb = mach_timebase_info_data_t()
mach_timebase_info(&tb)
func ticksToNs(_ t: UInt64) -> UInt64 { return t / UInt64(tb.denom) * UInt64(tb.numer) + (t % UInt64(tb.denom)) * UInt64(tb.numer) / UInt64(tb.denom) }
func nowNs() -> UInt64 { return ticksToNs(mach_absolute_time()) }

func writePNG(_ pixels: [UInt8], _ w: Int, _ h: Int, _ path: String) {
    let cs = CGColorSpaceCreateDeviceGray()
    guard let provider = CGDataProvider(data: Data(pixels) as CFData),
          let img = CGImage(width: w, height: h, bitsPerComponent: 8, bitsPerPixel: 8, bytesPerRow: w, space: cs,
                            bitmapInfo: CGBitmapInfo(rawValue: 0), provider: provider, decode: nil,
                            shouldInterpolate: false, intent: .defaultIntent),
          let dest = CGImageDestinationCreateWithURL(URL(fileURLWithPath: path) as CFURL, "public.png" as CFString, 1, nil)
    else { return }
    CGImageDestinationAddImage(dest, img, nil)
    CGImageDestinationFinalize(dest)
}

// ---- arguments
var args = Array(CommandLine.arguments.dropFirst())
func flag(_ name: String) -> Bool { if let i = args.firstIndex(of: name) { args.remove(at: i); return true }; return false }
func option(_ name: String) -> String? {
    if let i = args.firstIndex(of: name), i + 1 < args.count { let v = args[i + 1]; args.removeSubrange(i...i + 1); return v }
    return nil
}
let listOnly = flag("--list")
let noCamera = flag("--no-camera")
let outDir = option("--out") ?? "."
let cameraName = option("--camera") ?? "FaceTime HD Camera"
let duration = Double(option("--duration") ?? "0") ?? 0
let formatIndex = Int(option("--format") ?? "-1") ?? -1
var box = [565, 80, 240, 130]
if let b = option("--box") { let v = b.split(separator: ",").compactMap { Int($0) }; if v.count == 4 { box = v } }
let OUT_W = 128, OUT_H = 64

// ---- MIDI
func midiName(_ obj: MIDIObjectRef) -> String {
    var s: Unmanaged<CFString>?
    if MIDIObjectGetStringProperty(obj, kMIDIPropertyDisplayName, &s) == noErr, let v = s { return v.takeRetainedValue() as String }
    return "?"
}
var sources: [(MIDIEndpointRef, String)] = []
for i in 0..<MIDIGetNumberOfSources() {
    let e = MIDIGetSource(i)
    sources.append((e, midiName(e)))
}

// ---- camera
func findCamera() -> AVCaptureDevice? {
    var types: [AVCaptureDevice.DeviceType] = [.builtInWideAngleCamera]
    if #available(macOS 14.0, *) { types.append(.external) }
    let all = AVCaptureDevice.DiscoverySession(deviceTypes: types, mediaType: .video, position: .unspecified).devices
    return all.first { $0.localizedName == cameraName } ?? all.first { $0.localizedName.contains(cameraName) }
}
func describe(_ f: AVCaptureDevice.Format) -> (Int, Int, Double) {
    let d = CMVideoFormatDescriptionGetDimensions(f.formatDescription)
    let fps = f.videoSupportedFrameRateRanges.map { $0.maxFrameRate }.max() ?? 0
    return (Int(d.width), Int(d.height), fps)
}

if listOnly {
    print("MIDI sources:")
    for (i, s) in sources.enumerated() { print("  \(i): \(s.1)") }
    print("Camera authorisation status: \(AVCaptureDevice.authorizationStatus(for: .video).rawValue) (3 = authorised, 0 = not asked)")
    if let cam = findCamera() {
        print("Camera \"\(cam.localizedName)\" formats:")
        for (i, f) in cam.formats.enumerated() {
            let (w, h, fps) = describe(f)
            let sub = CMFormatDescriptionGetMediaSubType(f.formatDescription)
            let cc = String(format: "%c%c%c%c", (sub >> 24) & 255, (sub >> 16) & 255, (sub >> 8) & 255, sub & 255)
            print("  \(i): \(w)x\(h) up to \(fps) fps  \(cc)")
        }
    } else { print("Camera \"\(cameraName)\" not found") }
    exit(0)
}

try? FileManager.default.createDirectory(atPath: outDir, withIntermediateDirectories: true)
let meta = fopen(outDir + "/meta.txt", "w")!
setlinebuf(meta)
fputs("start_ns \(nowNs())\ntimebase \(tb.numer)/\(tb.denom)\n", meta)

let midiFile = fopen(outDir + "/midi.csv", "w")!
setlinebuf(midiFile)
fputs("host_ns,source,hex\n", midiFile)
var client = MIDIClientRef()
MIDIClientCreateWithBlock("lagrec" as CFString, &client, nil)
var inPort = MIDIPortRef()
let dataOffset = MemoryLayout<MIDIPacket>.offset(of: \MIDIPacket.data)!
MIDIInputPortCreateWithBlock(client, "lagrec-in" as CFString, &inPort) { pktList, refCon in
    let src = Int(bitPattern: refCon) - 1
    let arrival = nowNs()
    var p = UnsafeRawPointer(pktList).advanced(by: MemoryLayout<MIDIPacketList>.offset(of: \MIDIPacketList.packet)!)
        .assumingMemoryBound(to: MIDIPacket.self)
    for _ in 0..<pktList.pointee.numPackets {
        // MIDIPacket is packed (not aligned): read the fields byte-wise.
        let raw = UnsafeRawPointer(p)
        let ts = raw.loadUnaligned(as: UInt64.self)
        let len = Int(raw.loadUnaligned(fromByteOffset: 8, as: UInt16.self))
        let bytes = raw.advanced(by: dataOffset).assumingMemoryBound(to: UInt8.self)
        var hex = ""
        hex.reserveCapacity(len * 2)
        for i in 0..<len { hex += String(format: "%02X", bytes[i]) }
        fputs("\(ts == 0 ? arrival : ticksToNs(ts)),\(src),\(hex)\n", midiFile)
        p = UnsafePointer(MIDIPacketNext(UnsafeMutablePointer(mutating: p)))
    }
}
var connected = 0
for (i, s) in sources.enumerated() where s.1.contains("Launchkey") {
    MIDIPortConnectSource(inPort, s.0, UnsafeMutableRawPointer(bitPattern: i + 1))
    fputs("midi_source \(i) \(s.1)\n", meta)
    connected += 1
}
print("MIDI: listening to \(connected) Launchkey source(s)")
if connected == 0 { print("WARNING: no Launchkey MIDI source found") }

final class Recorder: NSObject, AVCaptureVideoDataOutputSampleBufferDelegate {
    let bin = fopen(outDir + "/frames.bin", "wb")!
    let csv = fopen(outDir + "/frames.csv", "w")!
    var count = 0, dropped = 0, snaps = 0
    var snapPending = false
    var firstPts: UInt64 = 0, lastPts: UInt64 = 0
    var clock: CMClock?
    override init() { super.init(); fputs("index,pts_ns,arrival_ns\n", csv) }

    func captureOutput(_ output: AVCaptureOutput, didDrop sampleBuffer: CMSampleBuffer, from connection: AVCaptureConnection) { dropped += 1 }

    func captureOutput(_ output: AVCaptureOutput, didOutput sb: CMSampleBuffer, from connection: AVCaptureConnection) {
        let arrival = nowNs()
        guard let pb = CMSampleBufferGetImageBuffer(sb) else { return }
        var pts = CMSampleBufferGetPresentationTimeStamp(sb)
        if let c = clock { pts = CMSyncConvertTime(pts, from: c, to: CMClockGetHostTimeClock()) }
        let ptsNs = UInt64(max(0, CMTimeConvertScale(pts, timescale: 1_000_000_000, method: .default).value))
        CVPixelBufferLockBaseAddress(pb, .readOnly)
        defer { CVPixelBufferUnlockBaseAddress(pb, .readOnly) }
        let planar = CVPixelBufferIsPlanar(pb)
        let w = planar ? CVPixelBufferGetWidthOfPlane(pb, 0) : CVPixelBufferGetWidth(pb)
        let h = planar ? CVPixelBufferGetHeightOfPlane(pb, 0) : CVPixelBufferGetHeight(pb)
        let bpr = planar ? CVPixelBufferGetBytesPerRowOfPlane(pb, 0) : CVPixelBufferGetBytesPerRow(pb)
        guard let basePtr = planar ? CVPixelBufferGetBaseAddressOfPlane(pb, 0) : CVPixelBufferGetBaseAddress(pb) else { return }
        let base = basePtr.assumingMemoryBound(to: UInt8.self)
        if count == 0 {
            fputs("frame_size \(w)x\(h) planar \(planar) pixel_format \(CVPixelBufferGetPixelFormatType(pb))\n", meta)
            firstPts = ptsNs
        }
        lastPts = ptsNs
        let bx = max(0, box[0] * w / 1920), by = max(0, box[1] * h / 1080)
        let bw = min(w - bx, box[2] * w / 1920), bh = min(h - by, box[3] * h / 1080)
        var out = [UInt8](repeating: 0, count: OUT_W * OUT_H)
        for oy in 0..<OUT_H {
            let y0 = by + oy * bh / OUT_H, y1 = max(y0 + 1, by + (oy + 1) * bh / OUT_H)
            for ox in 0..<OUT_W {
                let x0 = bx + ox * bw / OUT_W, x1 = max(x0 + 1, bx + (ox + 1) * bw / OUT_W)
                var s = 0
                for y in y0..<y1 { let row = base + y * bpr; for x in x0..<x1 { s += Int(row[x]) } }
                out[oy * OUT_W + ox] = UInt8(s / ((y1 - y0) * (x1 - x0)))
            }
        }
        fwrite(out, 1, out.count, bin)
        fputs("\(count),\(ptsNs),\(arrival)\n", csv)
        if count == 20 {
            var full = [UInt8](repeating: 0, count: w * h)
            for y in 0..<h { for x in 0..<w { full[y * w + x] = base[y * bpr + x] } }
            writePNG(full, w, h, outDir + "/full.png")
        }
        if snapPending {
            snapPending = false
            snaps += 1
            let mx = bw / 8, my = bh / 8
            let sx = max(0, bx - mx), sy = max(0, by - my), sw = min(w - sx, bw + 2 * mx), sh = min(h - sy, bh + 2 * my)
            let k = 3
            var img = [UInt8](repeating: 0, count: sw * k * sh * k)
            for y in 0..<sh * k { for x in 0..<sw * k { img[y * sw * k + x] = base[(sy + y / k) * bpr + sx + x / k] } }
            writePNG(img, sw * k, sh * k, outDir + String(format: "/snap-%02d.png", snaps))
            fputs("snap \(snaps) pts_ns \(ptsNs)\n", meta)
        }
        count += 1
    }
    func finish() {
        fclose(bin); fclose(csv)
        let secs = Double(lastPts - firstPts) / 1e9
        let fps = secs > 0 ? Double(count - 1) / secs : 0
        fputs("frames \(count) dropped \(dropped) seconds \(secs) fps \(fps)\n", meta)
        print(String(format: "camera: %d frames, %d dropped, %.2f s, %.2f fps", count, dropped, secs, fps))
    }
}

var session: AVCaptureSession?
var recorder: Recorder?
var cameraOK = false
if !noCamera {
    var status = AVCaptureDevice.authorizationStatus(for: .video)
    if status == .notDetermined {
        let sem = DispatchSemaphore(value: 0)
        AVCaptureDevice.requestAccess(for: .video) { _ in sem.signal() }
        _ = sem.wait(timeout: .now() + 8)
        status = AVCaptureDevice.authorizationStatus(for: .video)
    }
    if status != .authorized {
        print("CAMERA NOT AUTHORISED (status \(status.rawValue)): recording MIDI only")
        fputs("camera not_authorised\n", meta)
    } else if let cam = findCamera(), let input = try? AVCaptureDeviceInput(device: cam) {
        let s = AVCaptureSession()
        s.beginConfiguration()
        s.addInput(input)
        let r = Recorder()
        let output = AVCaptureVideoDataOutput()
        output.videoSettings = [kCVPixelBufferPixelFormatTypeKey as String: kCVPixelFormatType_420YpCbCr8BiPlanarFullRange]
        output.alwaysDiscardsLateVideoFrames = false
        output.setSampleBufferDelegate(r, queue: DispatchQueue(label: "lagrec.video"))
        s.addOutput(output)
        // Highest frame rate; then 16:9; then widest.
        var best = -1
        var bestKey = (0.0, 0, 0)
        for (i, f) in cam.formats.enumerated() {
            let (w, h, fps) = describe(f)
            let key = (fps, (w * 9 == h * 16) ? 1 : 0, w)
            if best < 0 || key > bestKey { best = i; bestKey = key }
        }
        if formatIndex >= 0 && formatIndex < cam.formats.count { best = formatIndex }
        let f = cam.formats[best]
        let (fw, fh, ffps) = describe(f)
        do {
            try cam.lockForConfiguration()
            cam.activeFormat = f
            if let range = f.videoSupportedFrameRateRanges.max(by: { $0.maxFrameRate < $1.maxFrameRate }) {
                cam.activeVideoMinFrameDuration = range.minFrameDuration
                cam.activeVideoMaxFrameDuration = range.minFrameDuration
            }
        } catch { print("could not configure the camera: \(error)") }
        s.commitConfiguration()
        s.startRunning()
        cam.unlockForConfiguration()
        if #available(macOS 12.3, *) { r.clock = s.synchronizationClock } else { r.clock = s.masterClock }
        let (aw, ah, _) = describe(cam.activeFormat)
        fputs("camera \(cam.localizedName)\nformat_asked \(best) \(fw)x\(fh)@\(ffps)\nformat_active \(aw)x\(ah)\nbox_1920x1080 \(box)\n", meta)
        print("camera: \(cam.localizedName) format \(best) \(fw)x\(fh) @ \(ffps) fps (active \(aw)x\(ah)), box \(box)")
        session = s; recorder = r; cameraOK = true
    } else {
        print("CAMERA \"\(cameraName)\" NOT AVAILABLE: recording MIDI only")
        fputs("camera not_found\n", meta)
    }
}
print("recording to \(outDir)  (camera \(cameraOK ? "on" : "off"))  — stop with Ctrl-C / SIGINT")
fflush(stdout)

func stop() {
    session?.stopRunning()
    recorder?.finish()
    MIDIPortDispose(inPort)
    fputs("end_ns \(nowNs())\n", meta)
    fclose(midiFile); fclose(meta)
    fflush(stdout)
    exit(0)
}
signal(SIGINT, SIG_IGN); signal(SIGTERM, SIG_IGN); signal(SIGUSR1, SIG_IGN)
var signalSources: [DispatchSourceSignal] = []
for sig in [SIGINT, SIGTERM] {
    let src = DispatchSource.makeSignalSource(signal: sig, queue: .main)
    src.setEventHandler { stop() }
    src.resume(); signalSources.append(src)
}
let usr = DispatchSource.makeSignalSource(signal: SIGUSR1, queue: .main)
usr.setEventHandler { recorder?.snapPending = true }
usr.resume(); signalSources.append(usr)
if duration > 0 { DispatchQueue.main.asyncAfter(deadline: .now() + duration) { stop() } }
dispatchMain()
