import AppKit
import AVFoundation

private let canvasSize = NSSize(width: 360, height: 510)

final class AudioController {
    private let audioEngine = AVAudioEngine()
    private var sourceNode: AVAudioSourceNode!
    let handle: OpaquePointer

    init() throws {
        let deviceFormat = audioEngine.outputNode.inputFormat(forBus: 0)
        let hardwareRate = deviceFormat.sampleRate > 0 ? deviceFormat.sampleRate : 48_000
        let sampleRate = min(max(hardwareRate, 44_100), 192_000)
        guard let created = dl_create(Float(sampleRate)) else {
            throw NSError(domain: "DelayLama", code: 1,
                          userInfo: [NSLocalizedDescriptionKey: "Could not initialise the synthesis engine."])
        }
        handle = created

        guard let renderFormat = AVAudioFormat(standardFormatWithSampleRate: sampleRate, channels: 2) else {
            throw NSError(domain: "DelayLama", code: 2,
                          userInfo: [NSLocalizedDescriptionKey: "Could not create the audio format."])
        }

        let renderHandle = created
        sourceNode = AVAudioSourceNode(format: renderFormat) { _, _, frameCount, audioBufferList in
            let buffers = UnsafeMutableAudioBufferListPointer(audioBufferList)
            guard buffers.count >= 2,
                  let leftData = buffers[0].mData,
                  let rightData = buffers[1].mData else {
                for buffer in buffers {
                    if let data = buffer.mData {
                        memset(data, 0, Int(buffer.mDataByteSize))
                    }
                }
                return noErr
            }
            let left = leftData.assumingMemoryBound(to: Float.self)
            let right = rightData.assumingMemoryBound(to: Float.self)
            dl_process(renderHandle, left, right, UInt32(frameCount))
            return noErr
        }

        audioEngine.attach(sourceNode)
        audioEngine.connect(sourceNode, to: audioEngine.mainMixerNode, format: renderFormat)
        audioEngine.mainMixerNode.outputVolume = 0.8
        audioEngine.prepare()
        try audioEngine.start()
    }

    deinit {
        audioEngine.stop()
        dl_destroy(handle)
    }

    func noteOn(_ note: UInt8) { dl_note_on(handle, note, 1.0) }
    func noteOff(_ note: UInt8) { dl_note_off(handle, note) }
    func allNotesOff() { dl_all_notes_off(handle) }
}

final class DelayLamaView: NSView {
    private enum DragMode { case none, xy, glide, voice, delay }

    private let audio: AudioController
    private let background: NSImage
    private let faces: NSImage
    private var dragMode: DragMode = .none
    private var glide: Float = 0.5
    private var voice: Float = 0.5
    private var delay: Float = 0.42
    private var dragStart = NSPoint.zero
    private var dragStartValue: Float = 0
    private var xyPoint = NSPoint(x: 0.5, y: 0.5)
    private var keyboardNotes: [String: UInt8] = [:]
    private var animationTimer: Timer?
    private var idleTick = 0

    private let xyRect = NSRect(x: 98, y: 365, width: 164, height: 61)
    private let glideRect = NSRect(x: 17, y: 443, width: 67, height: 62)
    private let voiceRect = NSRect(x: 279, y: 443, width: 66, height: 62)
    private let delayRect = NSRect(x: 82, y: 458, width: 196, height: 43)
    private let helpRect = NSRect(x: 280, y: 295, width: 48, height: 43)

    override var isFlipped: Bool { true }
    override var acceptsFirstResponder: Bool { true }

    init(frame: NSRect, audio: AudioController) {
        self.audio = audio
        guard let resourceURL = Bundle.main.resourceURL,
              let bg = NSImage(contentsOf: resourceURL.appendingPathComponent("background.png")),
              let faceSheet = NSImage(contentsOf: resourceURL.appendingPathComponent("faces.png")) else {
            fatalError("Delay Lama artwork is missing from the application bundle")
        }
        background = bg
        faces = faceSheet
        super.init(frame: frame)
        toolTip = "Drag the Tibetan flag to sing. A–K plays notes. Left knob: glide. Right knob: voice. Bottom slider: delay."
        animationTimer = Timer.scheduledTimer(withTimeInterval: 0.06, repeats: true) { [weak self] _ in
            self?.idleTick += 1
            self?.needsDisplay = true
        }
        if let animationTimer {
            RunLoop.main.add(animationTimer, forMode: .common)
        }
    }

    required init?(coder: NSCoder) { fatalError("init(coder:) has not been implemented") }

    deinit { animationTimer?.invalidate() }

    private func drawImage(_ image: NSImage, in destination: NSRect, from source: NSRect) {
        image.draw(in: destination, from: source, operation: .sourceOver, fraction: 1,
                   respectFlipped: true, hints: [.interpolation: NSImageInterpolation.high])
    }

    private func activeFrame() -> Int {
        if dl_is_active(audio.handle) != 0 {
            let vowel = max(0, min(1, dl_get_vowel(audio.handle)))
            return 6 + Int((vowel * 23).rounded())
        }
        let cycle = idleTick % 150
        if (cycle >= 68 && cycle < 72) || (cycle >= 78 && cycle < 81) { return 4 }
        return 5
    }

    override func draw(_ dirtyRect: NSRect) {
        super.draw(dirtyRect)
        drawImage(background, in: bounds, from: NSRect(origin: .zero, size: canvasSize))

        let frame = activeFrame()
        let column = frame / 6
        let row = frame % 6
        let frameWidth: CGFloat = 314
        let frameHeight: CGFloat = 311
        let sourceY = faces.size.height - CGFloat(row + 1) * frameHeight
        let source = NSRect(x: CGFloat(column) * frameWidth, y: sourceY,
                            width: frameWidth, height: frameHeight)
        drawImage(faces, in: NSRect(x: 23, y: 0, width: frameWidth, height: frameHeight), from: source)

        drawKnob(center: NSPoint(x: 50, y: 473), value: glide)
        drawKnob(center: NSPoint(x: 311, y: 473), value: voice)
        drawDelayHandle()

        if dragMode == .xy {
            let point = NSPoint(x: xyRect.minX + xyPoint.x * xyRect.width,
                                y: xyRect.minY + xyPoint.y * xyRect.height)
            NSColor.white.withAlphaComponent(0.72).setStroke()
            let cross = NSBezierPath()
            cross.lineWidth = 1
            cross.move(to: NSPoint(x: xyRect.minX, y: point.y))
            cross.line(to: NSPoint(x: xyRect.maxX, y: point.y))
            cross.move(to: NSPoint(x: point.x, y: xyRect.minY))
            cross.line(to: NSPoint(x: point.x, y: xyRect.maxY))
            cross.stroke()
            NSColor.white.withAlphaComponent(0.9).setFill()
            NSBezierPath(ovalIn: NSRect(x: point.x - 4, y: point.y - 4, width: 8, height: 8)).fill()
        }
    }

    private func drawKnob(center: NSPoint, value: Float) {
        let angle = CGFloat(-2.35 + Double(value) * 4.70)
        // The original interface used a compact white position mark set into
        // the face of each knob, rather than a full-radius coloured pointer.
        let innerRadius: CGFloat = 5
        let outerRadius: CGFloat = 16
        let start = NSPoint(x: center.x + sin(angle) * innerRadius,
                            y: center.y - cos(angle) * innerRadius)
        let endpoint = NSPoint(x: center.x + sin(angle) * outerRadius,
                               y: center.y - cos(angle) * outerRadius)
        let indicator = NSBezierPath()
        indicator.move(to: start)
        indicator.line(to: endpoint)
        indicator.lineWidth = 4
        indicator.lineCapStyle = .round
        NSColor.white.setStroke()
        indicator.stroke()
    }

    private func drawDelayHandle() {
        let x = delayRect.minX + 12 + CGFloat(delay) * (delayRect.width - 24)
        let rect = NSRect(x: x - 8, y: 474, width: 16, height: 16)
        NSColor.black.withAlphaComponent(0.65).setFill()
        NSBezierPath(ovalIn: rect.insetBy(dx: -1, dy: -1)).fill()
        NSColor(calibratedRed: 0.72, green: 0.48, blue: 0.18, alpha: 1).setFill()
        NSBezierPath(ovalIn: rect).fill()
        NSColor(calibratedRed: 0.95, green: 0.78, blue: 0.32, alpha: 1).setStroke()
        NSBezierPath(ovalIn: rect.insetBy(dx: 2, dy: 2)).stroke()
    }

    private func updateXY(with point: NSPoint, begin: Bool) {
        let x = max(0, min(1, (point.x - xyRect.minX) / xyRect.width))
        let screenY = max(0, min(1, (point.y - xyRect.minY) / xyRect.height))
        xyPoint = NSPoint(x: x, y: screenY)
        let vowel = Float(1 - screenY)
        if begin {
            dl_xy_begin(audio.handle, Float(x), vowel)
        } else {
            dl_xy_move(audio.handle, Float(x), vowel)
        }
        needsDisplay = true
    }

    private func updateDrag(with point: NSPoint) {
        switch dragMode {
        case .xy:
            updateXY(with: point, begin: false)
        case .glide:
            glide = max(0, min(1, dragStartValue + Float((dragStart.y - point.y) / 100)))
            dl_set_glide(audio.handle, glide)
            needsDisplay = true
        case .voice:
            voice = max(0, min(1, dragStartValue + Float((dragStart.y - point.y) / 100)))
            dl_set_voice(audio.handle, voice)
            needsDisplay = true
        case .delay:
            delay = max(0, min(1, Float((point.x - delayRect.minX - 12) / (delayRect.width - 24))))
            dl_set_delay(audio.handle, delay)
            needsDisplay = true
        case .none:
            break
        }
    }

    override func mouseDown(with event: NSEvent) {
        window?.makeFirstResponder(self)
        let point = convert(event.locationInWindow, from: nil)
        dragStart = point
        if helpRect.contains(point) {
            (NSApp.delegate as? AppDelegate)?.showHelp(nil)
        } else if xyRect.contains(point) {
            dragMode = .xy
            updateXY(with: point, begin: true)
        } else if glideRect.contains(point) {
            dragMode = .glide
            dragStartValue = glide
        } else if voiceRect.contains(point) {
            dragMode = .voice
            dragStartValue = voice
        } else if delayRect.contains(point) {
            dragMode = .delay
            updateDrag(with: point)
        }
    }

    override func mouseDragged(with event: NSEvent) {
        updateDrag(with: convert(event.locationInWindow, from: nil))
    }

    override func mouseUp(with event: NSEvent) {
        if dragMode == .xy { dl_xy_end(audio.handle) }
        dragMode = .none
        needsDisplay = true
    }

    private static let keyMap: [String: UInt8] = [
        "a": 48, "w": 49, "s": 50, "e": 51, "d": 52, "f": 53,
        "t": 54, "g": 55, "y": 56, "h": 57, "u": 58, "j": 59,
        "k": 60, "o": 61, "l": 62
    ]

    override func keyDown(with event: NSEvent) {
        guard !event.isARepeat,
              let key = event.charactersIgnoringModifiers?.lowercased(),
              let note = Self.keyMap[key] else {
            super.keyDown(with: event)
            return
        }
        keyboardNotes[key] = note
        audio.noteOn(note)
        needsDisplay = true
    }

    override func keyUp(with event: NSEvent) {
        guard let key = event.charactersIgnoringModifiers?.lowercased(),
              let note = keyboardNotes.removeValue(forKey: key) else {
            super.keyUp(with: event)
            return
        }
        audio.noteOff(note)
        needsDisplay = true
    }

    override func resignFirstResponder() -> Bool {
        audio.allNotesOff()
        keyboardNotes.removeAll()
        return super.resignFirstResponder()
    }
}

final class AppDelegate: NSObject, NSApplicationDelegate {
    private var window: NSWindow?
    private var audio: AudioController?

    func applicationDidFinishLaunching(_ notification: Notification) {
        buildMenu()
        do {
            let controller = try AudioController()
            audio = controller
            let view = DelayLamaView(frame: NSRect(origin: .zero, size: canvasSize), audio: controller)
            let window = NSWindow(contentRect: NSRect(origin: .zero, size: canvasSize),
                                  styleMask: [.titled, .closable, .miniaturizable],
                                  backing: .buffered, defer: false)
            window.title = "Delay Lama Standalone"
            window.contentView = view
            window.isReleasedWhenClosed = false
            window.center()
            window.makeKeyAndOrderFront(nil)
            window.makeFirstResponder(view)
            self.window = window
            NSApp.activate(ignoringOtherApps: true)
        } catch {
            let alert = NSAlert(error: error)
            alert.messageText = "Delay Lama could not start audio"
            alert.runModal()
            NSApp.terminate(nil)
        }
    }

    func applicationShouldTerminateAfterLastWindowClosed(_ sender: NSApplication) -> Bool { true }

    private func buildMenu() {
        let mainMenu = NSMenu()
        let appItem = NSMenuItem()
        mainMenu.addItem(appItem)
        let appMenu = NSMenu()
        appItem.submenu = appMenu
        appMenu.addItem(withTitle: "About Delay Lama Standalone",
                        action: #selector(showHelp(_:)), keyEquivalent: "")
        appMenu.addItem(.separator())
        appMenu.addItem(withTitle: "Quit Delay Lama Standalone",
                        action: #selector(NSApplication.terminate(_:)), keyEquivalent: "q")
        NSApp.mainMenu = mainMenu
    }

    @objc func showHelp(_ sender: Any?) {
        let midiCount = audio.map { dl_midi_source_count($0.handle) } ?? 0
        let alert = NSAlert()
        alert.messageText = "Delay Lama Standalone"
        alert.informativeText = """
        Drag across the Tibetan flag to sing: horizontal controls pitch and vertical controls the vowel. Drag the left and right knobs vertically for Glide and Voice. Drag the bottom handle for Delay.

        Computer keyboard: A W S E D F T G Y H U J K O L

        MIDI: notes, pitch wheel→vowel, CC1 vibrato, CC5 glide, CC7 volume, CC12 delay and CC13 voice. Connected MIDI sources: \(midiCount).

        Native standalone reconstruction for modern macOS. Vocal DSP derived from the MIT-licensed MonkSynth clean-room project. Classic artwork and manual were recovered from the supplied 2004 freeware archive.
        """
        if let url = Bundle.main.resourceURL?.appendingPathComponent("about.png"),
           let image = NSImage(contentsOf: url) {
            image.size = NSSize(width: 126.5, height: 137.5)
            alert.icon = image
        }
        alert.addButton(withTitle: "OK")
        alert.addButton(withTitle: "Open Original Manual")
        if alert.runModal() == .alertSecondButtonReturn,
           let manual = Bundle.main.resourceURL?.appendingPathComponent("DelayLama-Original-Manual.pdf") {
            NSWorkspace.shared.open(manual)
        }
    }
}

let application = NSApplication.shared
let delegate = AppDelegate()
application.setActivationPolicy(.regular)
application.delegate = delegate
application.run()
