// Shark Jack Controller for the WiFi Pineapple Pager.
//
// Drives a Hak5 Shark Jack Cable attached to the Pager's USB host port over its
// serial console, and gives it an on-screen console on the Pager:
//
//	Tier 1  status spine  - read-only live panel (port, firmware, switch/mode,
//	                        power, ethernet link, active payload).
//	Tier 2  payload browse + arm - list the Cable's payload library, pick one
//	                        with the D-pad, and ACTIVATE (stage) it.
//	Live output         - passively watch the Cable's serial console, so the
//	                        payload's SERIAL_WRITE narration shows on the Pager
//	                        when you flip the Cable switch to ATTACK to run it.
//
// The hard parts, learned from the device survey (see the sharkjack-cable field
// notes): the Pager's gpsd grabs the same port, so we stop it while we hold the
// link and restart it on exit; and the CP2102's DTR line resets the Shark Jack
// on open, so we open the port ONCE, hold it, and sync to the shell prompt
// (tolerating the reset's boot banner) before issuing commands.
//
// Build: GOOS=linux GOARCH=mipsle GOMIPS=softfloat CGO_ENABLED=0 (see Makefile).
// Run on device from the payload wrapper (framebuffer UI), or `--probe` for a
// text dump over SSH.
package main

import (
	"encoding/base64"
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"regexp"
	"strings"
	"syscall"
	"time"
)

const (
	baud      = "57600"
	fbPath    = "/dev/fb0"
	inputPath = "/dev/input/event0"
	fbPixW    = 222 // native framebuffer is 222x480 portrait...
	landW     = 480 // ...mounted landscape 480x222, rotated 90 deg
	landH     = 222
)

// ---------------------------------------------------------------------------
// Serial link to the Shark Jack
// ---------------------------------------------------------------------------

type link struct {
	fd       int
	dev      string
	gpsdBack bool // gpsd was running before we stopped it -> restart on close
}

// findPort locates the Cable's CP2102 serial node (VID:PID 10c4:ea60). Falls
// back to the known by-path symlink, then /dev/ttyUSB0.
func findPort() string {
	ttys, _ := filepath.Glob("/dev/ttyUSB*")
	for _, t := range ttys {
		name := filepath.Base(t)
		dev, err := filepath.EvalSymlinks("/sys/class/tty/" + name + "/device")
		if err != nil {
			continue
		}
		parent := filepath.Dir(dev) // interface dir -> USB device dir
		vid, _ := os.ReadFile(parent + "/idVendor")
		pid, _ := os.ReadFile(parent + "/idProduct")
		if strings.TrimSpace(string(vid)) == "10c4" && strings.TrimSpace(string(pid)) == "ea60" {
			return t
		}
	}
	if p, err := filepath.EvalSymlinks("/dev/serial/by-path/1.1_1-1.1:1.0"); err == nil {
		return p
	}
	return "/dev/ttyUSB0"
}

func sh(cmd string) string {
	out, _ := exec.Command("/bin/sh", "-c", cmd).CombinedOutput()
	return strings.TrimSpace(string(out))
}

// stty applies the Shark Jack line discipline (57600 8N1 raw, hold modem lines)
// with the given VMIN/VTIME. time is in deciseconds (20 = 2.0s idle read wait).
func (l *link) setLine(vtime int) {
	cmd := "stty -F " + l.dev + " " + baud +
		" cs8 -cstopb -parenb -echo -crtscts clocal -hupcl raw; " +
		"stty -F " + l.dev + " min 0 time " + fmt.Sprint(vtime)
	exec.Command("/bin/sh", "-c", cmd).Run()
}

// open stops gpsd, configures the port, opens it once, and syncs to the prompt.
func open() (*link, error) {
	l := &link{}
	// gpsd is pinned to this exact port, procd-managed and enabled by default,
	// so it respawns. Stop it UNCONDITIONALLY (a `pgrep` check races with gpsd's
	// respawn and lets it come back and corrupt our serial bytes) and clear any
	// process still holding the node. Restore gpsd on close.
	l.gpsdBack = true
	exec.Command("/etc/init.d/gpsd", "stop").Run()
	exec.Command("/bin/sh", "-c",
		`for p in $(ls -l /proc/*/fd/* 2>/dev/null | grep ttyUSB | `+
			`sed -E 's#.*/proc/([0-9]+)/fd.*#\1#' | sort -u); do kill -9 "$p" 2>/dev/null; done`).Run()
	time.Sleep(1 * time.Second)

	l.dev = findPort()

	// min 0 time 20 => read() returns as soon as bytes arrive, else after ~2s.
	l.setLine(20)

	fd, err := syscall.Open(l.dev, syscall.O_RDWR|syscall.O_NOCTTY, 0)
	if err != nil {
		l.restoreGpsd()
		return nil, fmt.Errorf("open %s: %v", l.dev, err)
	}
	l.fd = fd

	if !l.sync(15 * time.Second) {
		// one retry: the DTR reset may still be booting
		if !l.sync(15 * time.Second) {
			l.Close()
			return nil, fmt.Errorf("no prompt from %s (switch in arming?)", l.dev)
		}
	}
	return l, nil
}

func (l *link) restoreGpsd() {
	if l.gpsdBack {
		exec.Command("/etc/init.d/gpsd", "start").Run()
	}
}

func (l *link) Close() {
	if l.fd > 0 {
		syscall.Close(l.fd)
		l.fd = 0
	}
	l.restoreGpsd()
}

func (l *link) read(buf []byte) int {
	n, err := syscall.Read(l.fd, buf)
	if err != nil || n < 0 {
		return 0
	}
	return n
}

func (l *link) write(s string) {
	b := []byte(s)
	for len(b) > 0 {
		n, err := syscall.Write(l.fd, b)
		if err != nil || n <= 0 {
			return
		}
		b = b[n:]
	}
}

func (l *link) drain() {
	buf := make([]byte, 512)
	// a couple of short non-blocking-ish passes to clear pending bytes
	deadline := time.Now().Add(400 * time.Millisecond)
	for time.Now().Before(deadline) {
		if l.read(buf) == 0 {
			break
		}
	}
}

var reReady = regexp.MustCompile(`(?m)^__RDY__`)

// sync waits until the Shark Jack shell is actually executing commands, not just
// until a boot banner appears (a DTR-reset on open reboots the device, so the
// banner can precede a usable prompt). It confirms readiness by round-tripping
// an `echo __RDY__` and seeing the marker come back on its own line.
func (l *link) sync(timeout time.Duration) bool {
	buf := make([]byte, 512)
	deadline := time.Now().Add(timeout)
	for time.Now().Before(deadline) {
		l.drain()
		l.write("echo __RDY__\r\n")
		var acc strings.Builder
		probeEnd := time.Now().Add(2 * time.Second)
		for time.Now().Before(probeEnd) {
			n := l.read(buf)
			if n > 0 {
				acc.Write(buf[:n])
				if reReady.MatchString(acc.String()) {
					l.drain()
					return true
				}
			}
		}
	}
	return false
}

var reBetween = regexp.MustCompile(`(?s)__B__\r?\n(.*?)\r?\n__E__`)

// run executes a command and returns its trimmed stdout, delimited by markers
// so the echoed command line and shell prompt are stripped reliably.
func (l *link) run(cmd string) string { return l.runT(cmd, 8*time.Second) }

// runT is run with an explicit deadline, for commands whose output can be large
// or slow over the 57600 link (e.g. base64-ing a loot file).
func (l *link) runT(cmd string, timeout time.Duration) string {
	l.drain()
	l.write("echo __B__; " + cmd + "; echo __E__\r\n")
	buf := make([]byte, 512)
	var acc strings.Builder
	deadline := time.Now().Add(timeout)
	for time.Now().Before(deadline) {
		n := l.read(buf)
		if n > 0 {
			acc.Write(buf[:n])
			// Contains is a cheap guard before the full regex scan.
			if s := acc.String(); strings.Contains(s, "__E__") && reBetween.MatchString(s) {
				break
			}
		}
	}
	raw := acc.String()
	if m := reBetween.FindStringSubmatch(raw); m != nil {
		return strings.TrimSpace(m[1])
	}
	// looser fallback: take text between the LAST __B__ and the next __E__
	if b := strings.LastIndex(raw, "__B__"); b >= 0 {
		rest := raw[b+5:]
		if e := strings.Index(rest, "__E__"); e >= 0 {
			return strings.TrimSpace(strings.Trim(rest[:e], "\r\n"))
		}
	}
	return ""
}

// runRaw returns the unparsed accumulator for a command (debugging).
func (l *link) runRaw(cmd string) string {
	l.drain()
	l.write("echo __B__; " + cmd + "; echo __E__\r\n")
	buf := make([]byte, 512)
	var acc strings.Builder
	deadline := time.Now().Add(6 * time.Second)
	for time.Now().Before(deadline) {
		n := l.read(buf)
		if n > 0 {
			acc.Write(buf[:n])
			if reBetween.MatchString(acc.String()) {
				break
			}
		}
	}
	return acc.String()
}

// ---------------------------------------------------------------------------
// Status model
// ---------------------------------------------------------------------------

type status struct {
	connected bool
	errMsg    string
	port      string
	firmware  string
	switchPos string
	mode      string
	power     string
	active    string
	ethLink   string
}

func modeFor(sw string) string {
	switch sw {
	case "switch3":
		return "ATTACK"
	case "switch2":
		return "ARMING"
	case "switch1":
		return "OFF"
	default:
		return "UNKNOWN"
	}
}

func gather(l *link) status {
	s := status{connected: true, port: l.dev}
	s.firmware = firstLine(l.run("cat /root/VERSION 2>/dev/null"))
	s.switchPos = firstLine(l.run("SWITCH 2>/dev/null"))
	s.mode = modeFor(s.switchPos)
	s.power = firstLine(l.run("BATTERY 2>/dev/null"))
	title := l.run(`grep -m1 -i 'Title' /root/payload/payload.sh 2>/dev/null | sed 's/.*[Tt]itle:[[:space:]]*//'`)
	if title == "" {
		title = l.run("basename $(readlink -f /root/payload/payload.sh 2>/dev/null) 2>/dev/null")
	}
	s.active = firstLine(title)
	el := l.run("swconfig dev switch0 port 0 get link 2>/dev/null")
	s.ethLink = "DOWN"
	if strings.Contains(el, "link:up") {
		s.ethLink = "UP"
	}
	return s
}

func firstLine(s string) string {
	s = strings.TrimSpace(s)
	if i := strings.IndexAny(s, "\r\n"); i >= 0 {
		s = s[:i]
	}
	if s == "" {
		return "?"
	}
	return s
}

// ---------------------------------------------------------------------------
// Payload library (Tier 2)
// ---------------------------------------------------------------------------

type entry struct{ cat, name string }

func (e entry) path() string  { return e.cat + "/" + e.name }
func (e entry) label() string { return e.cat + "/" + e.name }

// listPayloads enumerates the Cable's library as <category>/<name>, reading the
// directory tree directly rather than parsing LIST (whose output carries the
// basename-usage noise documented in the field notes).
func listPayloads(l *link) []entry {
	out := l.run(`for d in /root/payload/library/*/*/; do [ -f "$d/payload.sh" ] && echo "${d#/root/payload/library/}"; done`)
	var es []entry
	for _, ln := range strings.Split(out, "\n") {
		ln = strings.TrimSpace(ln)
		ln = strings.TrimSuffix(ln, "/")
		if ln == "" || strings.Contains(ln, "*") { // unexpanded glob = empty library
			continue
		}
		parts := strings.SplitN(ln, "/", 2)
		if len(parts) != 2 || parts[0] == "" || parts[1] == "" {
			continue
		}
		es = append(es, entry{parts[0], parts[1]})
	}
	return es
}

// arm stages a payload as the Cable's active payload. Returns ACTIVATE's output
// (usually empty on success).
func arm(l *link, e entry) string {
	return l.run("ACTIVATE " + e.path() + " 2>&1")
}

// ---------------------------------------------------------------------------
// Loot (Tier 3)
// ---------------------------------------------------------------------------

const (
	lootCableDir = "/root/loot"           // on the Shark Jack
	lootPagerDir = "/root/loot/sharkjack" // where we drop pulled loot on the Pager
)

type lootFile struct {
	rel  string // path under /root/loot
	size string // bytes
}

// lootList enumerates files under the Cable's /root/loot. `while read` handles
// paths with spaces; the `|` delimiter keeps size and path unambiguous.
func lootList(l *link) []lootFile {
	out := l.run(`find ` + lootCableDir + ` -type f 2>/dev/null | while IFS= read -r f; do echo "$(wc -c < "$f" 2>/dev/null)|$f"; done`)
	var fs []lootFile
	for _, ln := range strings.Split(out, "\n") {
		ln = strings.TrimSpace(ln)
		i := strings.Index(ln, "|")
		if i < 0 {
			continue
		}
		size, full := ln[:i], ln[i+1:]
		rel := strings.TrimPrefix(full, lootCableDir+"/")
		if rel == "" || strings.Contains(rel, "*") {
			continue
		}
		fs = append(fs, lootFile{rel: rel, size: strings.TrimSpace(size)})
	}
	return fs
}

func stripWS(s string) string {
	return strings.Map(func(r rune) rune {
		if r == ' ' || r == '\t' || r == '\n' || r == '\r' {
			return -1
		}
		return r
	}, s)
}

// pullTimeout scales with the file size: base64 is ~4/3 the bytes, and the link
// is ~5.7 KB/s, so allow generous headroom plus a floor.
func pullTimeout(sizeBytes int) time.Duration {
	secs := 12 + sizeBytes/2500
	if secs > 90 {
		secs = 90
	}
	return time.Duration(secs) * time.Second
}

// lootPull copies one loot file off the Cable to the Pager's loot dir and returns
// its bytes. It transfers base64 (binary-safe) and falls back to plain cat if the
// Cable has no base64. Returns the saved Pager path too.
func lootPull(l *link, lf lootFile) (data []byte, saved string, err error) {
	full := lootCableDir + "/" + lf.rel
	size := 0
	fmt.Sscanf(lf.size, "%d", &size)
	to := pullTimeout(size)

	b64 := stripWS(l.runT(`base64 "`+full+`" 2>/dev/null`, to))
	if b64 != "" {
		if d, e := base64.StdEncoding.DecodeString(b64); e == nil {
			data = d
		}
	}
	if data == nil {
		// fallback: plain text read (loses exact bytes for binary files)
		data = []byte(l.runT(`cat "`+full+`" 2>/dev/null`, to))
	}

	saved = lootPagerDir + "/" + lf.rel
	if e := os.MkdirAll(filepath.Dir(saved), 0755); e != nil {
		return data, "", e
	}
	if e := os.WriteFile(saved, data, 0644); e != nil {
		return data, "", e
	}
	return data, saved, nil
}

// isBinary reports whether data looks non-textual (NUL or many control bytes).
func isBinary(data []byte) bool {
	n := len(data)
	if n > 2048 {
		n = 2048
	}
	ctrl := 0
	for i := 0; i < n; i++ {
		c := data[i]
		if c == 0 {
			return true
		}
		if c < 0x09 || (c > 0x0d && c < 0x20) {
			ctrl++
		}
	}
	return n > 0 && ctrl*20 > n // >5% control bytes
}

// textLines turns file bytes into display lines (printable, wrapped to width).
func textLines(data []byte, width int) []string {
	var lines []string
	cur := make([]byte, 0, width)
	flush := func() { lines = append(lines, string(cur)); cur = cur[:0] }
	for _, c := range data {
		switch {
		case c == '\n':
			flush()
		case c == '\r' || c == '\t':
			// tab -> single space, CR dropped
			if c == '\t' {
				cur = append(cur, ' ')
			}
		case c >= 0x20 && c < 0x7f:
			cur = append(cur, c)
		default:
			cur = append(cur, '.')
		}
		if len(cur) >= width {
			flush()
		}
	}
	if len(cur) > 0 {
		flush()
	}
	return lines
}

// ---------------------------------------------------------------------------
// Live serial output view
// ---------------------------------------------------------------------------

type outView struct {
	lines   []string
	partial string
}

func (o *outView) push(s string) {
	o.lines = append(o.lines, s)
	if len(o.lines) > 300 {
		o.lines = o.lines[len(o.lines)-300:]
	}
}

// feed folds raw serial bytes into printable lines (CR dropped, control bytes
// skipped, overlong lines wrapped so the display stays bounded).
func (o *outView) feed(b []byte) {
	for _, c := range b {
		switch {
		case c == '\n':
			o.push(o.partial)
			o.partial = ""
		case c == '\r':
			// drop
		case c >= 0x20 && c < 0x7f:
			o.partial += string(rune(c))
			if len(o.partial) > 78 {
				o.push(o.partial)
				o.partial = ""
			}
		}
	}
}

// ---------------------------------------------------------------------------
// Framebuffer UI
// ---------------------------------------------------------------------------

func rgb(r, g, b uint32) uint16 {
	return uint16((r>>3)<<11 | (g>>2)<<5 | (b >> 3))
}

var (
	colBG     = rgb(14, 16, 22)
	colHeader = rgb(20, 150, 180)
	colHdrTxt = rgb(240, 250, 255)
	colLabel  = rgb(130, 140, 155)
	colValue  = rgb(235, 238, 245)
	colGood   = rgb(70, 200, 110)
	colWarn   = rgb(240, 190, 70)
	colBad    = rgb(230, 90, 80)
	colFoot   = rgb(110, 120, 135)
	colSelBG  = rgb(30, 60, 72)
)

type fb struct{ mem []byte }

func openFB() (*fb, error) {
	f, err := os.OpenFile(fbPath, os.O_RDWR, 0)
	if err != nil {
		return nil, err
	}
	defer f.Close()
	mem, err := syscall.Mmap(int(f.Fd()), 0, fbPixW*480*2,
		syscall.PROT_READ|syscall.PROT_WRITE, syscall.MAP_SHARED)
	if err != nil {
		return nil, err
	}
	return &fb{mem: mem}, nil
}

func (f *fb) px(lx, ly int, c uint16) {
	if lx < 0 || lx >= landW || ly < 0 || ly >= landH {
		return
	}
	idx := lx*fbPixW + (fbPixW - 1 - ly) // 90-degree rotation
	off := idx * 2
	if off+1 >= len(f.mem) {
		return
	}
	f.mem[off] = byte(c)
	f.mem[off+1] = byte(c >> 8)
}

func (f *fb) rect(x, y, w, h int, c uint16) {
	for dy := 0; dy < h; dy++ {
		for dx := 0; dx < w; dx++ {
			f.px(x+dx, y+dy, c)
		}
	}
}

func (f *fb) clear(c uint16) { f.rect(0, 0, landW, landH, c) }

func (f *fb) text(x, y int, s string, scale int, c uint16) int {
	cx := x
	for _, r := range s {
		g := glyph(r)
		for row := 0; row < 7; row++ {
			bits := g[row]
			for col := 0; col < 5; col++ {
				if bits&(1<<(4-col)) != 0 {
					f.rect(cx+col*scale, y+row*scale, scale, scale, c)
				}
			}
		}
		cx += 6 * scale
	}
	return cx
}

func (f *fb) header(title string) {
	f.rect(0, 0, landW, 26, colHeader)
	f.text(8, 5, title, 2, colHdrTxt)
}

func (f *fb) footer(s string) {
	f.text(8, landH-14, s, 1, colFoot)
}

func clip(s string, n int) string {
	if len(s) > n {
		return s[:n]
	}
	return s
}

// --- Status screen ---------------------------------------------------------

func (f *fb) renderConnecting() {
	f.clear(colBG)
	f.header("SHARK JACK CTRL")
	f.text(10, 70, "CONNECTING TO CABLE...", 2, colWarn)
	f.text(10, 110, "STOPPING GPS, OPENING SERIAL", 1, colLabel)
	f.text(10, 124, "CONSOLE AT 57600 8N1.", 1, colLabel)
}

func (f *fb) renderStatus(s status, msg string) {
	f.clear(colBG)
	f.header("SHARK JACK CTRL")

	if !s.connected {
		f.text(8, 60, "NOT CONNECTED", 2, colBad)
		y := 95
		for _, ln := range wrap(s.errMsg, 36) {
			f.text(8, y, ln, 1, colWarn)
			y += 12
		}
		f.footer("GREEN:RETRY   RED:EXIT")
		return
	}

	rows := []struct {
		label, val string
		col        uint16
	}{
		{"PORT", s.port, colValue},
		{"FIRMWARE", s.firmware, colValue},
		{"MODE", s.mode + " (" + s.switchPos + ")", modeColor(s.mode)},
		{"POWER", s.power, colValue},
		{"ETH LINK", s.ethLink, linkColor(s.ethLink)},
		{"ACTIVE", clip(s.active, 20), colValue},
	}
	y := 36
	for _, r := range rows {
		f.text(10, y, r.label, 2, colLabel)
		f.text(150, y, r.val, 2, r.col)
		y += 24
	}
	if msg != "" {
		f.text(10, landH-30, clip(msg, 58), 1, colGood)
	}
	f.footer("GRN:REFRESH DN:PAYLOADS UP:LOOT RT:WATCH RED:EXIT")
}

func modeColor(m string) uint16 {
	switch m {
	case "ARMING":
		return colGood
	case "ATTACK":
		return colWarn
	case "OFF":
		return colBad
	}
	return colValue
}

func linkColor(l string) uint16 {
	if l == "UP" {
		return colGood
	}
	return colFoot
}

// --- Payload menu ----------------------------------------------------------

const menuRows = 11

func (f *fb) renderMenu(es []entry, sel, top int) {
	f.clear(colBG)
	f.header(fmt.Sprintf("PAYLOADS (%d)", len(es)))
	if len(es) == 0 {
		f.text(8, 60, "NO PAYLOADS FOUND", 2, colWarn)
		f.text(8, 90, "LIBRARY EMPTY OR UNREADABLE", 1, colLabel)
		f.footer("RED:BACK")
		return
	}
	y := 32
	for i := top; i < len(es) && i < top+menuRows; i++ {
		c := colValue
		if i == sel {
			f.rect(3, y-2, landW-6, 14, colSelBG)
			c = colHdrTxt
		}
		f.text(7, y, clip(es[i].label(), 76), 1, c)
		y += 15
	}
	f.footer("GRN:ARM  UP/DN:MOVE  RED:BACK")
}

// --- Confirm arm -----------------------------------------------------------

func (f *fb) renderConfirm(e entry) {
	f.clear(colBG)
	f.header("ARM PAYLOAD")
	f.text(10, 44, "STAGE THIS PAYLOAD?", 1, colLabel)
	y := 62
	for _, ln := range wrap(e.label(), 36) {
		f.text(10, y, ln, 2, colGood)
		y += 20
	}
	f.text(10, 118, "IT RUNS WHEN YOU FLIP THE", 1, colLabel)
	f.text(10, 132, "CABLE SWITCH TO ATTACK.", 1, colLabel)
	f.footer("GREEN:ARM   RED:CANCEL")
}

// --- Live output -----------------------------------------------------------

const outRows = 12

func (f *fb) renderOutput(o *outView) {
	f.clear(colBG)
	f.header("LIVE OUTPUT")

	disp := o.lines
	if o.partial != "" {
		disp = append(append([]string{}, o.lines...), o.partial)
	}
	if len(disp) == 0 {
		f.text(8, 50, "WAITING FOR SERIAL...", 1, colWarn)
		f.text(8, 70, "FLIP THE CABLE SWITCH TO", 1, colLabel)
		f.text(8, 84, "ATTACK TO RUN THE ARMED", 1, colLabel)
		f.text(8, 98, "PAYLOAD; ITS OUTPUT SHOWS", 1, colLabel)
		f.text(8, 112, "HERE.", 1, colLabel)
		f.footer("RED:BACK")
		return
	}
	start := 0
	if len(disp) > outRows {
		start = len(disp) - outRows
	}
	y := 32
	for _, ln := range disp[start:] {
		f.text(6, y, clip(ln, 78), 1, colValue)
		y += 14
	}
	f.footer("RED:BACK")
}

// --- Loot list ---------------------------------------------------------------

const lootRows = 11

func (f *fb) renderLoot(fs []lootFile, sel, top int) {
	f.clear(colBG)
	f.header(fmt.Sprintf("LOOT (%d)", len(fs)))
	if len(fs) == 0 {
		f.text(8, 60, "NO LOOT ON CABLE", 2, colWarn)
		f.text(8, 90, "/ROOT/LOOT IS EMPTY - RUN A", 1, colLabel)
		f.text(8, 104, "PAYLOAD IN ATTACK MODE FIRST.", 1, colLabel)
		f.footer("GRN:REFRESH   RED:BACK")
		return
	}
	y := 32
	for i := top; i < len(fs) && i < top+lootRows; i++ {
		c := colValue
		if i == sel {
			f.rect(3, y-2, landW-6, 14, colSelBG)
			c = colHdrTxt
		}
		sz := fs[i].size
		f.text(7, y, clip(sz, 8), 1, colFoot)
		f.text(7+9*6, y, clip(fs[i].rel, 68), 1, c)
		y += 15
	}
	f.footer("GRN:PULL+VIEW  UP/DN:MOVE  RED:BACK")
}

func (f *fb) renderPulling(name string) {
	f.clear(colBG)
	f.header("LOOT")
	f.text(10, 70, "PULLING OVER SERIAL...", 1, colWarn)
	f.text(10, 92, clip(name, 46), 2, colValue)
	f.text(10, 130, "57600 BAUD - PLEASE WAIT", 1, colLabel)
}

// --- Loot view ---------------------------------------------------------------

const lootViewRows = 12

func (f *fb) renderLootView(name string, lines []string, top int, note string) {
	f.clear(colBG)
	f.header(clip("LOOT: "+name, 24))
	if note != "" {
		f.text(6, 30, clip(note, 78), 1, colGood)
	}
	y := 46
	for i := top; i < len(lines) && i < top+lootViewRows; i++ {
		f.text(6, y, clip(lines[i], 78), 1, colValue)
		y += 14
	}
	f.footer("UP/DN:SCROLL  RED:BACK")
}

func wrap(s string, n int) []string {
	var out []string
	for len(s) > n {
		out = append(out, s[:n])
		s = s[n:]
	}
	if s != "" {
		out = append(out, s)
	}
	return out
}

// ---------------------------------------------------------------------------
// Input
// ---------------------------------------------------------------------------

const (
	keyUp    = 103
	keyDown  = 108
	keyLeft  = 105
	keyRight = 106
	keyPower = 116
	keyRed   = 304 // A: back / exit
	keyGreen = 305 // B: select / confirm
)

// keyReader streams EV_KEY press codes from the Pager buttons onto ch. It runs
// for the life of the program; the blocking read parks it when idle.
func keyReader(fd int, ch chan<- int) {
	ev := make([]byte, 16)
	for {
		n, err := syscall.Read(fd, ev)
		if err != nil || n < 16 {
			close(ch)
			return
		}
		typ := uint16(ev[8]) | uint16(ev[9])<<8
		code := uint16(ev[10]) | uint16(ev[11])<<8
		val := int32(ev[12]) | int32(ev[13])<<8 | int32(ev[14])<<16 | int32(ev[15])<<24
		if typ == 1 && val == 1 { // EV_KEY press
			ch <- int(code)
		}
	}
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

type screen int

const (
	scStatus screen = iota
	scMenu
	scConfirm
	scOutput
	scLoot
	scLootView
)

func main() {
	probe := len(os.Args) > 1 && os.Args[1] == "--probe"
	raw := len(os.Args) > 1 && os.Args[1] == "--raw"

	if raw {
		l, err := open()
		if err != nil {
			fmt.Println("CONNECT FAILED:", err)
			return
		}
		defer l.Close()
		fmt.Printf("%q\n", l.runRaw("cat /root/VERSION"))
		fmt.Printf("%q\n", l.runRaw("SWITCH"))
		fmt.Println("payloads:", listPayloads(l))
		return
	}
	if probe {
		l, err := open()
		if err != nil {
			fmt.Println("CONNECT FAILED:", err)
			return
		}
		defer l.Close()
		s := gather(l)
		fmt.Printf("port=%s\nfirmware=%s\nswitch=%s mode=%s\npower=%s\neth_link=%s\nactive=%s\n",
			s.port, s.firmware, s.switchPos, s.mode, s.power, s.ethLink, s.active)
		fmt.Println("payloads:")
		for _, e := range listPayloads(l) {
			fmt.Println("  ", e.path())
		}
		return
	}

	// framebuffer mode
	f, ferr := openFB()
	if ferr != nil {
		fmt.Println("fb open failed:", ferr)
		os.Exit(1)
	}
	in, _ := syscall.Open(inputPath, syscall.O_RDONLY, 0)
	keys := make(chan int, 16)
	go keyReader(in, keys)

	// Draw a frame BEFORE the (blocking, up to ~30s) serial connect, so the UI
	// takeover is honoured immediately and the firmware doesn't reclaim the
	// display during a silent connect.
	f.renderConnecting()

	l, err := open()

	cleanup := func() {
		if l != nil {
			l.Close()
		}
		if in > 0 {
			syscall.Close(in)
		}
	}

	var s status
	if err != nil {
		s = status{connected: false, errMsg: err.Error()}
	} else {
		s = gather(l)
	}

	var (
		entries   []entry
		sel, top  int
		confirm   entry
		ov        outView
		msg       string
		loot      []lootFile
		lsel, ltp int
		lvName    string
		lvLines   []string
		lvTop     int
		lvNote    string
		scr       = scStatus
	)

	draw := func() {
		switch scr {
		case scStatus:
			f.renderStatus(s, msg)
		case scMenu:
			f.renderMenu(entries, sel, top)
		case scConfirm:
			f.renderConfirm(confirm)
		case scOutput:
			f.renderOutput(&ov)
		case scLoot:
			f.renderLoot(loot, lsel, ltp)
		case scLootView:
			f.renderLootView(lvName, lvLines, lvTop, lvNote)
		}
	}
	draw()

	buf := make([]byte, 1024)
	for {
		// Live output: interleave passive serial reads with key polling.
		if scr == scOutput {
			if l != nil {
				if n := l.read(buf); n > 0 {
					ov.feed(buf[:n])
					f.renderOutput(&ov)
				}
			}
			select {
			case k, ok := <-keys:
				if !ok || k == keyRed {
					if l != nil {
						l.setLine(20) // restore the idle read timeout
						if l.sync(5 * time.Second) {
							s = gather(l)
							msg = ""
						} else {
							msg = "LINK BUSY - SET SWITCH TO ARMING, PRESS GREEN"
						}
					}
					scr = scStatus
					draw()
				}
			default:
			}
			continue
		}

		// Blocking screens: wait for the next key.
		k, ok := <-keys
		if !ok {
			cleanup()
			return
		}
		switch scr {
		case scStatus:
			switch k {
			case keyRed:
				cleanup()
				return
			case keyGreen:
				msg = ""
				if !s.connected {
					if l != nil {
						l.Close()
					}
					l, err = open()
					if err != nil {
						s = status{connected: false, errMsg: err.Error()}
					} else {
						s = gather(l)
					}
				} else {
					s = gather(l)
				}
				draw()
			case keyDown:
				if s.connected {
					entries = listPayloads(l)
					sel, top = 0, 0
					scr = scMenu
					draw()
				}
			case keyUp:
				if s.connected {
					loot = lootList(l)
					lsel, ltp = 0, 0
					scr = scLoot
					draw()
				}
			case keyRight:
				if s.connected {
					ov = outView{}
					l.setLine(2) // snappier read timeout (~0.2s) for live polling
					scr = scOutput
					f.renderOutput(&ov)
				}
			}
		case scMenu:
			switch k {
			case keyRed:
				scr = scStatus
				draw()
			case keyUp:
				if sel > 0 {
					sel--
					if sel < top {
						top = sel
					}
				}
				draw()
			case keyDown:
				if sel < len(entries)-1 {
					sel++
					if sel >= top+menuRows {
						top = sel - menuRows + 1
					}
				}
				draw()
			case keyGreen:
				if len(entries) > 0 {
					confirm = entries[sel]
					scr = scConfirm
					draw()
				}
			}
		case scConfirm:
			switch k {
			case keyRed:
				scr = scMenu
				draw()
			case keyGreen:
				out := arm(l, confirm)
				if out == "" {
					msg = "ARMED " + confirm.path()
				} else {
					msg = "ARM: " + firstLine(out)
				}
				s = gather(l)
				scr = scStatus
				draw()
			}
		case scLoot:
			switch k {
			case keyRed:
				scr = scStatus
				draw()
			case keyUp:
				if lsel > 0 {
					lsel--
					if lsel < ltp {
						ltp = lsel
					}
				}
				draw()
			case keyDown:
				if lsel < len(loot)-1 {
					lsel++
					if lsel >= ltp+lootRows {
						ltp = lsel - lootRows + 1
					}
				}
				draw()
			case keyGreen:
				if len(loot) == 0 {
					// empty list: Green re-scans the Cable
					if s.connected {
						loot = lootList(l)
						lsel, ltp = 0, 0
					}
					draw()
					break
				}
				lf := loot[lsel]
				f.renderPulling(lf.rel)
				data, saved, err := lootPull(l, lf)
				lvName, lvTop = lf.rel, 0
				switch {
				case err != nil:
					lvNote = "SAVE FAILED: " + err.Error()
					lvLines = textLines(data, 78)
				case isBinary(data):
					lvNote = fmt.Sprintf("BINARY %d B -> %s", len(data), saved)
					lvLines = []string{"(binary file; saved to the Pager loot dir -", "open it in the Loot Browser)"}
				default:
					lvNote = fmt.Sprintf("SAVED %d B -> sharkjack/%s", len(data), lf.rel)
					lvLines = textLines(data, 78)
				}
				scr = scLootView
				draw()
			}
		case scLootView:
			switch k {
			case keyRed:
				scr = scLoot
				draw()
			case keyUp:
				if lvTop > 0 {
					lvTop -= lootViewRows
					if lvTop < 0 {
						lvTop = 0
					}
				}
				draw()
			case keyDown:
				if lvTop+lootViewRows < len(lvLines) {
					lvTop += lootViewRows
				}
				draw()
			}
		}
	}
}
