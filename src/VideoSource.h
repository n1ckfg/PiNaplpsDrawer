#pragma once

#include "ofMain.h"

#include <cstdio>
#include <string>
#include <vector>

/// Picks whatever video input this machine actually has and presents it as a
/// single stream of ofPixels.
///
/// Raspberry Pi makes this awkward: a ribbon-cable (CSI) camera is reachable
/// only through libcamera, which openFrameworks' ofVideoGrabber cannot open,
/// while /dev/video* on a Pi is mostly bcm2835 codec and ISP nodes rather than
/// real capture devices. So each backend is probed in turn and the first one
/// that produces a frame wins.
class VideoSource {
public:
	enum class Backend {
		None,
		Csi, ///< Pi camera module, read from an rpicam-vid subprocess.
		Webcam, ///< USB / UVC camera through ofVideoGrabber.
		VideoFile, ///< A movie in bin/data, looped.
		Image, ///< A still in bin/data, repeated. Handy for testing detection.
		Synthetic ///< Generated frames, so the app still runs with no input.
	};

	struct Settings {
		int width = 640;
		int height = 480;
		int frameRate = 30;
		/// Backends are tried in this order; the first that works is used.
		std::vector<Backend> order {
			Backend::Csi, Backend::Webcam, Backend::VideoFile,
			Backend::Image, Backend::Synthetic
		};
	};

	~VideoSource();

	/// The no-argument form uses Settings' defaults.
	bool setup();
	bool setup(const Settings & settings);
	void close();

	/// Switches to one specific backend, keeping the current source if the new
	/// one cannot be opened. Returns true only if `wanted` is now running;
	/// getLastError() says why not otherwise. Never leaves the app without a
	/// source: if even the previous backend can no longer be reopened, the
	/// normal probe order runs and lands on Synthetic at worst.
	bool switchTo(Backend wanted);

	/// Why the last setup()/switchTo() could not do what was asked. Empty once
	/// something succeeds outright.
	const std::string & getLastError() const { return lastError; }

	/// Pulls the next frame if one is ready.
	void update();

	/// True on frames where update() produced new pixels.
	bool isFrameNew() const { return frameIsNew; }

	const ofPixels & getPixels() const { return pixels; }
	int getWidth() const { return (int)pixels.getWidth(); }
	int getHeight() const { return (int)pixels.getHeight(); }

	Backend getBackend() const { return backend; }
	std::string getBackendName() const;
	/// True when the source yields a single unchanging frame, so there is
	/// nothing to gain from re-running inference on it.
	bool isStatic() const { return backend == Backend::Image; }
	/// Human-readable detail about the chosen source, for the on-screen HUD.
	const std::string & getDescription() const { return description; }

	/// Lists why each backend was skipped, for troubleshooting.
	const std::vector<std::string> & getProbeLog() const { return probeLog; }

	static std::string toString(Backend backend);

private:
	/// Runs one backend's probe, turning any exception into a probe-log line
	/// rather than letting it out: a source that fails to open is a normal
	/// outcome here, not an error condition.
	bool tryBackend(Backend candidate);
	/// Re-probes the normal order with one backend left out, for when a source
	/// dies while running. Excluding it is what stops this from looping.
	bool fallbackExcluding(Backend excluded);

	bool tryCsi();
	bool tryWebcam();
	bool tryVideoFile();
	bool tryImage();
	bool trySynthetic();

	void updateCsi();
	void updateSynthetic();

	/// True if any /dev/video* node reports plain (non-codec) capture support.
	static bool hasV4l2CaptureDevice(std::string & outDescription);
	/// True if rpicam reports at least one attached camera.
	static bool hasCsiCamera(std::string & outDescription);
	/// Finds the first file in bin/data with one of `extensions`.
	static std::string findDataFile(const std::vector<std::string> & extensions);

	Settings settings;
	Backend backend = Backend::None;
	std::string description;
	std::string lastError;
	std::vector<std::string> probeLog;

	ofPixels pixels;
	bool frameIsNew = false;

	ofVideoGrabber grabber;
	ofVideoPlayer player;

	// CSI: raw I420 frames streamed from rpicam-vid over a pipe.
	FILE * csiPipe = nullptr;
	std::vector<unsigned char> csiFrame;
	int csiFrameBytes = 0;
	// The negotiated capture size, which is the requested size rounded up to
	// the alignment rpicam-vid needs to emit tightly packed planes.
	int csiWidth = 0;
	int csiHeight = 0;

	float syntheticPhase = 0.f;
	/// When the still stops being re-delivered. Both tasks run in MediaPipe's
	/// VIDEO mode, which refines its answer across frames, so a single
	/// cold-start pass understates what the models do; a short warmup lets
	/// tracking settle without pinning a core forever.
	float stillWarmupEndTime = 0.f;
	static constexpr float kStillWarmupSeconds = 6.f;
};
