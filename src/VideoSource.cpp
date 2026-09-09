#include "VideoSource.h"

#include <fcntl.h>
#include <linux/videodev2.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace {

/// A /dev/video* node is only useful to us if it does plain capture. The Pi's
/// bcm2835 codec and ISP nodes also advertise capture, but as memory-to-memory
/// devices, so those are filtered out explicitly.
bool isPlainCaptureDevice(const std::string & path, std::string & outName) {
	const int fd = ::open(path.c_str(), O_RDWR | O_NONBLOCK);
	if (fd < 0) {
		return false;
	}
	v4l2_capability cap {};
	const bool queried = ::ioctl(fd, VIDIOC_QUERYCAP, &cap) == 0;
	::close(fd);
	if (!queried) {
		return false;
	}

	const uint32_t caps = (cap.capabilities & V4L2_CAP_DEVICE_CAPS)
		? cap.device_caps
		: cap.capabilities;

	const bool captures = (caps & V4L2_CAP_VIDEO_CAPTURE) != 0;
	const bool isM2m = (caps & (V4L2_CAP_VIDEO_M2M | V4L2_CAP_VIDEO_M2M_MPLANE)) != 0;
	if (!captures || isM2m) {
		return false;
	}

	const std::string driver(reinterpret_cast<const char *>(cap.driver));
	if (driver.find("bcm2835") != std::string::npos) {
		return false;
	}

	outName = std::string(reinterpret_cast<const char *>(cap.card)) + " (" + path + ")";
	return true;
}

/// Runs a command and returns its stdout, or "" if it could not be run.
std::string runCommand(const std::string & command) {
	FILE * pipe = popen(command.c_str(), "r");
	if (pipe == nullptr) {
		return "";
	}
	std::string output;
	char buffer[512];
	while (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
		output += buffer;
	}
	pclose(pipe);
	return output;
}

/// Converts one tightly packed I420 (YUV420 planar) frame to RGB.
void i420ToRgb(const unsigned char * src, int width, int height, ofPixels & out) {
	if (!out.isAllocated() || (int)out.getWidth() != width || (int)out.getHeight() != height
		|| out.getPixelFormat() != OF_PIXELS_RGB) {
		out.allocate(width, height, OF_PIXELS_RGB);
	}

	const unsigned char * planeY = src;
	const unsigned char * planeU = planeY + (size_t)width * height;
	const unsigned char * planeV = planeU + (size_t)(width / 2) * (height / 2);
	unsigned char * dst = out.getData();

	for (int y = 0; y < height; ++y) {
		const int chromaRow = (y / 2) * (width / 2);
		for (int x = 0; x < width; ++x) {
			const int yy = planeY[y * width + x] - 16;
			const int uu = planeU[chromaRow + x / 2] - 128;
			const int vv = planeV[chromaRow + x / 2] - 128;

			// BT.601 limited-range YUV -> RGB, in fixed point.
			const int c = 298 * yy;
			const int r = (c + 409 * vv + 128) >> 8;
			const int g = (c - 100 * uu - 208 * vv + 128) >> 8;
			const int b = (c + 516 * uu + 128) >> 8;

			const size_t i = ((size_t)y * width + x) * 3;
			dst[i + 0] = (unsigned char)ofClamp(r, 0, 255);
			dst[i + 1] = (unsigned char)ofClamp(g, 0, 255);
			dst[i + 2] = (unsigned char)ofClamp(b, 0, 255);
		}
	}
}

} // namespace

VideoSource::~VideoSource() {
	close();
}

std::string VideoSource::toString(Backend backend) {
	switch (backend) {
	case Backend::Csi: return "CSI camera (rpicam-vid)";
	case Backend::Webcam: return "USB webcam";
	case Backend::VideoFile: return "video file";
	case Backend::Image: return "still image";
	case Backend::Synthetic: return "synthetic";
	case Backend::None: break;
	}
	return "none";
}

std::string VideoSource::getBackendName() const {
	return toString(backend);
}

bool VideoSource::setup() {
	return setup(Settings());
}

bool VideoSource::tryBackend(Backend candidate) {
	// Opening a camera reaches a long way outside this process -- a subprocess,
	// a V4L2 driver, a file on disk -- so a throw is treated the same as a
	// polite "no": it goes in the probe log and the next backend gets its turn.
	try {
		switch (candidate) {
		case Backend::Csi: return tryCsi();
		case Backend::Webcam: return tryWebcam();
		case Backend::VideoFile: return tryVideoFile();
		case Backend::Image: return tryImage();
		case Backend::Synthetic: return trySynthetic();
		case Backend::None: break;
		}
	} catch (const std::exception & e) {
		probeLog.push_back(toString(candidate) + ": threw - " + e.what());
	} catch (...) {
		probeLog.push_back(toString(candidate) + ": threw an unknown exception");
	}
	return false;
}

bool VideoSource::setup(const Settings & s) {
	close();
	settings = s;
	probeLog.clear();
	lastError.clear();

	for (const Backend candidate : settings.order) {
		if (tryBackend(candidate)) {
			backend = candidate;
			ofLogNotice("VideoSource") << "using " << getBackendName() << ": " << description;
			return true;
		}
	}

	backend = Backend::None;
	lastError = "no video source could be opened";
	return false;
}

bool VideoSource::switchTo(Backend wanted) {
	if (wanted == Backend::None) {
		lastError = "no such video source";
		return false;
	}
	if (wanted == backend) {
		lastError.clear();
		return true;
	}

	const Backend previous = backend;
	// The previous source has to be let go first: a CSI pipe and a webcam can
	// both be holding the same sensor, and rpicam-vid will not start a second
	// time on one that is already streaming.
	close();
	probeLog.clear();
	lastError.clear();

	if (tryBackend(wanted)) {
		backend = wanted;
		ofLogNotice("VideoSource") << "switched to " << getBackendName() << ": " << description;
		return true;
	}

	lastError = probeLog.empty()
		? toString(wanted) + " unavailable"
		: probeLog.front();
	ofLogWarning("VideoSource") << "could not switch to " << toString(wanted)
		<< ": " << lastError;

	// Asking for something that is not there must not cost the source that was
	// already working, so put it back.
	if (previous != Backend::None && tryBackend(previous)) {
		backend = previous;
		ofLogNotice("VideoSource") << "kept " << getBackendName();
		return false;
	}

	// It would not reopen either -- unplugged mid-session, say. Rather than run
	// on with no frames at all, fall back through the normal order, which ends
	// at Synthetic.
	const std::string switchError = lastError;
	setup(settings);
	lastError = switchError;
	return false;
}

bool VideoSource::fallbackExcluding(Backend excluded) {
	const Settings s = settings;
	close();
	probeLog.clear();

	for (const Backend candidate : s.order) {
		if (candidate == excluded) {
			continue;
		}
		if (tryBackend(candidate)) {
			backend = candidate;
			ofLogNotice("VideoSource") << "fell back to " << getBackendName()
				<< ": " << description;
			return true;
		}
	}

	backend = Backend::None;
	return false;
}

void VideoSource::close() {
	if (csiPipe != nullptr) {
		pclose(csiPipe);
		csiPipe = nullptr;
	}
	if (grabber.isInitialized()) {
		grabber.close();
	}
	if (player.isLoaded()) {
		player.close();
	}
	backend = Backend::None;
	frameIsNew = false;
	stillWarmupEndTime = 0.f;
}

bool VideoSource::hasCsiCamera(std::string & outDescription) {
	// rpicam-hello exits non-zero and prints "No cameras available!" when the
	// ribbon connector is empty, which is the common case on a dev machine.
	const std::string output = runCommand("rpicam-hello --list-cameras 2>&1");
	if (output.empty()) {
		outDescription = "rpicam-hello not installed";
		return false;
	}
	if (output.find("No cameras available") != std::string::npos) {
		outDescription = "no CSI camera attached";
		return false;
	}
	if (output.find("Available cameras") == std::string::npos) {
		outDescription = "rpicam-hello reported no camera list";
		return false;
	}
	outDescription = "rpicam-hello reports a camera";
	return true;
}

bool VideoSource::tryCsi() {
	std::string detail;
	if (!hasCsiCamera(detail)) {
		probeLog.push_back("CSI: " + detail);
		return false;
	}

	// I420 keeps framing trivial: every frame is exactly w*h*3/2 bytes, so
	// there are no markers to scan for, unlike an MJPEG stream.
	//
	// This assumes rpicam-vid writes the planes tightly packed, which holds
	// when the width is a multiple of 32 and the height a multiple of 16 (640x480
	// is). Unaligned sizes get row padding that this reader does not account
	// for, and the picture comes out skewed, so round the request up first.
	const int width = ((settings.width + 31) / 32) * 32;
	const int height = ((settings.height + 15) / 16) * 16;
	csiWidth = width;
	csiHeight = height;
	csiFrameBytes = width * height * 3 / 2;
	csiFrame.assign(csiFrameBytes, 0);

	const std::string command = "rpicam-vid --timeout 0 --nopreview"
		" --codec yuv420"
		" --width " + ofToString(width)
		+ " --height " + ofToString(height)
		+ " --framerate " + ofToString(settings.frameRate)
		+ " --output - 2>/dev/null";

	csiPipe = popen(command.c_str(), "r");
	if (csiPipe == nullptr) {
		probeLog.push_back("CSI: could not start rpicam-vid");
		return false;
	}

	pixels.allocate(width, height, OF_PIXELS_RGB);
	pixels.set(0);
	description = ofToString(width) + "x" + ofToString(height) + " via rpicam-vid";
	return true;
}

bool VideoSource::hasV4l2CaptureDevice(std::string & outDescription) {
	// Probed by index rather than by listing /dev, so the scan stays ordered
	// and cheap. 64 covers far more nodes than any real machine exposes.
	int nodesSeen = 0;
	for (int i = 0; i < 64; ++i) {
		const std::string path = "/dev/video" + ofToString(i);
		if (!ofFile::doesFileExist(path)) {
			continue;
		}
		++nodesSeen;
		std::string name;
		if (isPlainCaptureDevice(path, name)) {
			outDescription = name;
			return true;
		}
	}
	outDescription = (nodesSeen == 0)
		? "no /dev/video* nodes"
		: "only codec/ISP nodes, no capture device";
	return false;
}

bool VideoSource::tryWebcam() {
	// Check V4L2 directly first. Handing ofVideoGrabber a Pi with only bcm2835
	// codec nodes makes it enumerate devices it cannot open, which is slow and
	// noisy, so this guard keeps the common no-webcam case quiet.
	std::string detail;
	if (!hasV4l2CaptureDevice(detail)) {
		probeLog.push_back("Webcam: " + detail);
		return false;
	}

	grabber.setDesiredFrameRate(settings.frameRate);
	grabber.setUseTexture(false);
	if (!grabber.setup(settings.width, settings.height, false)) {
		probeLog.push_back("Webcam: " + detail + ", but ofVideoGrabber could not open it");
		return false;
	}

	description = detail + " at " + ofToString(grabber.getWidth(), 0) + "x"
		+ ofToString(grabber.getHeight(), 0);
	return true;
}

std::string VideoSource::findDataFile(const std::vector<std::string> & extensions) {
	ofDirectory dir(ofToDataPath("", true));
	if (!dir.exists()) {
		return "";
	}
	for (const auto & extension : extensions) {
		dir.allowExt(extension);
	}
	dir.listDir();
	dir.sort();
	return dir.size() > 0 ? dir.getPath(0) : "";
}

bool VideoSource::tryVideoFile() {
	const std::string path = findDataFile({"mp4", "mov", "avi", "mkv", "m4v", "webm"});
	if (path.empty()) {
		probeLog.push_back("Video file: no movie in bin/data");
		return false;
	}

	player.setUseTexture(false);
	if (!player.load(path)) {
		probeLog.push_back("Video file: could not load " + path);
		return false;
	}
	player.setLoopState(OF_LOOP_NORMAL);
	player.play();

	description = ofFilePath::getFileName(path);
	return true;
}

bool VideoSource::tryImage() {
	const std::string path = findDataFile({"png", "jpg", "jpeg", "bmp", "tif", "tiff"});
	if (path.empty()) {
		probeLog.push_back("Still image: no image in bin/data");
		return false;
	}
	if (!ofLoadImage(pixels, path)) {
		probeLog.push_back("Still image: could not load " + path);
		return false;
	}
	// MediaPipe wants RGB or RGBA; drop a palette/greyscale source to RGB.
	if (pixels.getPixelFormat() != OF_PIXELS_RGB && pixels.getPixelFormat() != OF_PIXELS_RGBA) {
		pixels.setImageType(OF_IMAGE_COLOR);
	}
	stillWarmupEndTime = ofGetElapsedTimef() + kStillWarmupSeconds;
	description = ofFilePath::getFileName(path) + " (still)";
	return true;
}

bool VideoSource::trySynthetic() {
	pixels.allocate(settings.width, settings.height, OF_PIXELS_RGB);
	pixels.set(0);
	description = "no camera, movie or image found - drop one in bin/data";
	return true;
}

void VideoSource::update() {
	frameIsNew = false;

	switch (backend) {
	case Backend::Csi:
		updateCsi();
		break;

	case Backend::Webcam:
		grabber.update();
		if (grabber.isFrameNew()) {
			pixels = grabber.getPixels();
			frameIsNew = true;
		}
		break;

	case Backend::VideoFile:
		player.update();
		if (player.isFrameNew()) {
			pixels = player.getPixels();
			if (pixels.getPixelFormat() != OF_PIXELS_RGB
				&& pixels.getPixelFormat() != OF_PIXELS_RGBA) {
				pixels.setImageType(OF_IMAGE_COLOR);
			}
			frameIsNew = true;
		}
		break;

	case Backend::Image:
		// Re-delivered only until the warmup expires. Feeding it forever would
		// re-run both models indefinitely to reach the same answer, pinning a
		// core for nothing.
		frameIsNew = ofGetElapsedTimef() < stillWarmupEndTime;
		break;

	case Backend::Synthetic:
		updateSynthetic();
		break;

	case Backend::None:
		break;
	}
}

void VideoSource::updateCsi() {
	if (csiPipe == nullptr) {
		return;
	}
	// One blocking read per update keeps this in step with the camera's frame
	// rate; rpicam-vid throttles the pipe to --framerate.
	const size_t read = fread(csiFrame.data(), 1, csiFrameBytes, csiPipe);
	if (read != (size_t)csiFrameBytes) {
		// rpicam-vid missing, or the camera taken away mid-session. Nothing is
		// detectable at popen() time -- the shell starts either way -- so this
		// short read is the first news of it, and the app carries on with
		// whatever else this machine has.
		ofLogWarning("VideoSource") << "CSI stream ended";
		lastError = "CSI stream ended";
		fallbackExcluding(Backend::Csi);
		return;
	}
	i420ToRgb(csiFrame.data(), csiWidth, csiHeight, pixels);
	frameIsNew = true;
}

void VideoSource::updateSynthetic() {
	// Deliberately not a person, so MediaPipe finds nothing: this exists to
	// prove the capture -> inference -> draw path runs end to end.
	syntheticPhase += 0.02f;

	const int width = (int)pixels.getWidth();
	const int height = (int)pixels.getHeight();
	unsigned char * data = pixels.getData();

	for (int y = 0; y < height; ++y) {
		for (int x = 0; x < width; ++x) {
			const float u = x / (float)width;
			const float v = y / (float)height;
			const float wave = std::sin((u * 6.f) + syntheticPhase)
				* std::cos((v * 6.f) - syntheticPhase);
			const size_t i = ((size_t)y * width + x) * 3;
			data[i + 0] = (unsigned char)ofMap(wave, -1.f, 1.f, 20.f, 90.f);
			data[i + 1] = (unsigned char)ofMap(wave, -1.f, 1.f, 30.f, 120.f);
			data[i + 2] = (unsigned char)ofMap(wave, -1.f, 1.f, 60.f, 150.f);
		}
	}
	frameIsNew = true;
}
