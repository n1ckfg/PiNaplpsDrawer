#include "ofApp.h"

#include <cctype>

#include "Pinopticon.hpp"
#include "Pinopticon_Http.hpp"

using namespace Pinopticon;

// Walk a TzKT JSON value and collect strings that could be NAPLPS data.
// Tezos `bytes` values arrive hex-encoded; plain `string` values arrive as-is.
static void collectNaplpsStrings(const ofJson& j, std::vector<std::string>& out, int maxBytes) {
    if (j.is_string()) {
        std::string val = j.get<std::string>();
        if (val.size() < 10) return;

        bool allHex = (val.size() % 2 == 0);
        if (allHex) {
            for (char c : val) {
                if (!std::isxdigit(static_cast<unsigned char>(c))) { allHex = false; break; }
            }
        }

        if (allHex && (int)val.size() >= 20) {
            std::string decoded;
            decoded.reserve(val.size() / 2);
            for (std::size_t i = 0; i < val.size(); i += 2) {
                unsigned int byte = 0;
                std::sscanf(val.c_str() + i, "%02x", &byte);
                decoded.push_back(static_cast<char>(byte));
            }
            if ((int)decoded.size() <= maxBytes) out.push_back(std::move(decoded));
        } else if ((int)val.size() <= maxBytes) {
            out.push_back(val);
        }
    } else if (j.is_object()) {
        for (auto& el : j.items()) collectNaplpsStrings(el.value(), out, maxBytes);
    } else if (j.is_array()) {
        for (auto& el : j) collectNaplpsStrings(el, out, maxBytes);
    }
}

//--------------------------------------------------------------
void ofApp::setup() {
    ofSetWindowTitle("PiNaplpsPlayer");
    ofSetFrameRate(60);
    //ofSetVerticalSync(true);
    //ofEnableAntiAliasing();
    //ofEnableAlphaBlending(); // Alpha disabled for performance
    ofBackground(0);
    ofHideCursor();

    settings.loadFile("settings.xml");
    slideTimeout = settings.getValue("settings:slide_timeout", 30000);
    slideInterval = settings.getValue("settings:slide_interval", 10000);

    fboWidth = settings.getValue("settings:fbo_width", 640);
    fboHeight = settings.getValue("settings:fbo_height", 480);

    // the sample files in bin/data, cycled through with the arrow keys and
    // drawn from at random by the dead man's switch
    scanSamples();
    sampleIndex = 0;

    debugView = (bool) settings.getValue("settings:debug_view", 0);
    progressiveDraw = true;
    labelPoints = debugView;
    showInfo = debugView;
    bFboDirty = true;

    updateLayout();
    if (!samples.empty()) loadNap(samples[sampleIndex]);
    napSource = "file";

    // The websocket server starts listening the moment it's set up, so
    // everything a frame touches has to be ready first.
    hasIncoming = false;
    received = 0;
    connections = 0;
    hostName = Pinopticon::getHostName();

    // Nothing has arrived yet, so the clock starts now: an app that comes up
    // with no server on the other end falls back after one timeout.
    ofSeedRandom();
    lastMessageTime = ofGetElapsedTimeMillis();
    lastSlideTime = lastMessageTime;
    slideshowActive = false;

    Pinopticon::setupWsServer(this, wsServer, WS_PORT, MAX_NAP_BYTES);

    fbo.allocate(fboWidth, fboHeight, GL_RGB);

    shaderName = settings.getValue("settings:shader_name", "vhsc");

#ifdef TARGET_OPENGLES
    shader.load("shaders/" + shaderName + "_es3");
#else
    if (ofIsGLProgrammableRenderer()) {
        shader.load("shaders/" + shaderName + "_gl3");
    } else {
        shader.load("shaders/" + shaderName + "_gl2");
    }
#endif

    if (!shader.isLoaded()) {
        ofLogWarning("PiNaplpsPlayer") << "shader " << shaderName << " didn't load, drawing without it";
    }

    tezosContract = settings.getValue("settings:tezos_contract", "KT1DypSEV87pwiw6swdYqhDKWRBZ7xfqeS3c");
    tzktBase = settings.getValue("settings:tzkt_base", "https://api.shadownet.tzkt.io/v1");
    tezosPollSeconds = settings.getValue("settings:tezos_poll_seconds", 30);
    tezosMaxBytes = settings.getValue("settings:tezos_max_bytes", 30000);
    tezosDrawingIndex = 0;
    slideshowFromChain = false;

    if (!tezosContract.empty()) {
        // No bin/data/ssl/cacert.pem ships with the app, and ofSSLManager's fallback
        // context trusts nothing. Use the OS trust store instead.
        ofSSLManager::initializeClient(new Poco::Net::Context(
            Poco::Net::Context::TLS_CLIENT_USE, "",
            Poco::Net::Context::VERIFY_RELAXED, 9, true /* loadDefaultCAs */));

        tezosRunning = true;
        tezosThread = std::thread(&ofApp::tezosThreadFunc, this);
    }

    // ~ ~ ~ outbound link to nap-xtz-server ~ ~ ~
    // Overridable from bin/data/settings.json, so a Pi in an installation can
    // point at the server without a rebuild.
    NapClient::Settings clientSettings;

    ofxJSONElement settingsJson;
    if (settingsJson.open(ofToDataPath("settings.json"))) {
        if (settingsJson.isMember("server_host")) {
            clientSettings.host = settingsJson["server_host"].asString();
        }
        if (settingsJson.isMember("server_ws_port")) {
            clientSettings.wsPort = settingsJson["server_ws_port"].asInt();
        }
        if (settingsJson.isMember("server_http_port")) {
            clientSettings.httpPort = settingsJson["server_http_port"].asInt();
        }
    } else {
        ofLogNotice("PiNaplpsPlayer") << "no settings.json, using defaults";
    }

    client.setup(clientSettings);
    client.start();

    // The browser's preload() asks the chain for the newest drawing before it
    // shows anything, and sits on the drag-and-drop placeholder until it
    // answers. This does the same, off the draw loop; update() falls back to a
    // local sample if the read fails, which is what the browser's "Chain read
    // failed -- using local samples" status is telling the user.
    client.fetchLatestAsync();

    // The camera and the gesture model both take seconds to come up, so live
    // drawing is prepared now rather than when the user asks for it.
    drawingMode.setup();
}

//--------------------------------------------------------------
void ofApp::loadNap(const std::string & filePath) {
    // 1. decode the file
    //naplps.setVerbose(true); // uncomment to log every command and point
    if (!naplps.load(filePath)) return;

    // Keep the raw bytes: publishing or minting what's on screen needs them.
    pendingNapRaw = naplps.napRaw;

    // 2. hand the decoded commands to the renderer
    startDrawing();
}

//--------------------------------------------------------------
// Whatever .nap files are in bin/data, in name order -- reading the directory
// rather than a hardcoded list means a drawing dropped in there is picked up
// by both the arrow keys and the dead man's switch.
void ofApp::scanSamples() {
    samples.clear();

    ofDirectory dir(ofToDataPath("", true));
    dir.allowExt("nap");
    dir.sort();
    dir.listDir();

    for (std::size_t i = 0; i < dir.size(); i++) {
        samples.push_back(dir.getName(i));
    }

    if (samples.empty()) {
        ofLogWarning("PiNaplpsPlayer") << "no .nap files in bin/data";
    }
}

//--------------------------------------------------------------
// The same thing for a drawing that arrived over the network: the bytes are
// already in hand, so they go straight to the decoder without touching a file.
void ofApp::showNap(const std::string & napRaw, const std::string & label) {
    naplps.decode(napRaw);

    if (!naplps.isLoaded()) {
        ofLogWarning("PiNaplpsPlayer") << "nothing to draw in " << label;
        return;
    }

    // decode() doesn't set a file name, and the old one would be a lie.
    naplps.fileName = label;
    pendingNapRaw = napRaw;

    startDrawing();
}

//--------------------------------------------------------------
void ofApp::startDrawing() {
    telidon.setup(naplps, drawSize, drawSize);
    telidon.setProgressiveDraw(progressiveDraw);
    telidon.setLabelPoints(labelPoints);
    bFboDirty = true;
}

//--------------------------------------------------------------
void ofApp::updateLayout() {
    drawSize = fboWidth; //MIN(ofGetWidth(), ofGetHeight());
    drawOffset = glm::vec2(0, fboHeight - fboWidth); //glm::vec2((ofGetWidth() - drawSize) / 2.0f, (ofGetHeight() - drawSize) / 2.0f);
    bFboDirty = true;
}

//--------------------------------------------------------------
void ofApp::update() {
    // Live drawing takes the whole window; the NAPLPS canvas idles behind it.
    if (drawingMode.isActive()) {
        drawingMode.update();

        // A two-handed Thumb_Up asks to leave. The mode doesn't know what
        // happens to the drawing afterwards, so it only raises the flag.
        if (drawingMode.isExitRequested()) {
            drawingMode.clearExitRequested();
            leaveDrawingMode();
        }
        return;
    }

    // ~ ~ ~ drawings pushed to us by nap-xtz-server (inbound server) ~ ~ ~
    NapFrame frame;
    bool gotOne = false;
    {
        std::lock_guard<std::mutex> lock(incomingMutex);
        if (hasIncoming) {
            frame = incoming;
            hasIncoming = false;
            gotOne = true;
        }
    }

    if (gotOne) {
        // Every path into the browser's canvas runs through loadTelidonFromText(),
        // which stops the slideshow first: content someone sent deliberately
        // outranks it.
        stopSlideshow();
        napSource = frame.source.empty() ? "network" : frame.source;
        showNap(frame.nap, "(" + napSource + ")");
    }

    // ~ ~ ~ drawings broadcast to us as a client (outbound link) ~ ~ ~
    // Only the newest is kept: a player shows one at a time, so anything older
    // that arrived in the same window has already been superseded.
    NapClient::Message message;
    bool gotMessage = false;
    while (client.getNextMessage(message)) gotMessage = true;

    if (gotMessage) {
        stopSlideshow(); // live content takes over
        napSource = message.source.empty() ? "server" : message.source;

        // A drawing read off the chain names its token, the way the browser's
        // status line says "Token #N loaded from chain".
        const std::string label = (message.tokenId >= 0)
            ? "(token " + ofToString(message.tokenId) + ")"
            : "(" + napSource + ")";

        showNap(message.naplps, label);
    }

    // ~ ~ ~ the startup chain read gave up ~ ~ ~
    // The browser leaves its placeholder standing here. A player on a wall with
    // no reachable server would then show nothing at all, so it falls back to
    // the first local sample instead -- once, and only if nothing else has
    // arrived in the meantime.
    if (!triedChainFallback && client.getLatestState() == NapClient::FetchState::Failed) {
        triedChainFallback = true;
        if (!hasContent) {
            loadNap(samples[sampleIndex]);
            napSource = "file";
        }
    }

    // ~ ~ ~ slideshow ~ ~ ~
    if (slideshowActive && ofGetElapsedTimef() - lastSlideTime >= slideInterval) {
        loadRandomNap();
    }

    telidon.update();

    if (showInfo) {
        static std::string lastState = "";
        std::string currentState = ofToString(connections) + "_" + ofToString(received) + "_"
            + (telidon.isFinished() ? "1" : "0") + "_" + napSource + "_" + naplps.fileName + "_"
            + ofToString(progressiveDraw) + "_" + ofToString(labelPoints) + "_"
            + client.getStatusText() + "_" + client.getMintStatus() + "_"
            + ofToString(slideshowActive);
        if (currentState != lastState) {
            updateInfoText();
            lastState = currentState;
        }
    }
}

//--------------------------------------------------------------
void ofApp::draw() {
    if (drawingMode.isActive()) {
        drawingMode.draw();
        return;
    }

    ofBackground(0);

    if (!hasContent) {
        // index.html's empty state: nothing loaded, so the canvas is just the
        // prompt. The browser stops its draw loop here; there's no equivalent
        // in OF and a static string costs nothing to redraw.
        const std::string prompt = "\\\\ DRAG ' n ' DROP //";
        ofSetColor(255);
        if (promptFont.isLoaded()) {
            const ofRectangle box = promptFont.getStringBoundingBox(prompt, 0, 0);
            promptFont.drawString(prompt,
                                  canvasOffset.x + (canvasSize.x - box.width) * 0.5f - box.x,
                                  canvasOffset.y + canvasSize.y * 0.5f);
        } else {
            ofDrawBitmapString(prompt,
                               canvasOffset.x + canvasSize.x * 0.5f - prompt.size() * 4.0f,
                               canvasOffset.y + canvasSize.y * 0.5f);
        }
    } else {
        if (!telidon.isFinished() || bFboDirty) {
            fbo.begin();
            ofBackground(0);

            ofPushMatrix();
            ofTranslate(drawOffset.x, drawOffset.y);
            telidon.draw();
            ofPopMatrix();
            fbo.end();

            if (telidon.isFinished()) {
                bFboDirty = false;
            }
        }

        // Straight through at its own size, into the centred 4:3 box.
        fbo.draw(canvasOffset.x, canvasOffset.y, canvasSize.x, canvasSize.y);
    }

    if (showInfo) {
        ofDrawBitmapStringHighlight(infoText, 10, 20);
    }
}

//--------------------------------------------------------------
void ofApp::checkDeadMansSwitch() {
    if (slideTimeout <= 0) return;

    bool hasChain;
    {
        std::lock_guard<std::mutex> lock(tezosMutex);
        hasChain = !tezosDrawings.empty();
    }

    if (samples.empty() && !hasChain) return;

    const uint64_t now = ofGetElapsedTimeMillis();

    if (!slideshowActive) {
        if (now - lastMessageTime < (uint64_t)slideTimeout) return;

        ofLogNotice("PiNaplpsPlayer") << "no drawing in " << slideTimeout
                                      << "ms, falling back to slideshow";
        slideshowActive = true;
        slideshowFromChain = false;
        if (!samples.empty()) {
            loadRandomNap();
        } else {
            loadChainNap();
        }
        return;
    }

    if (slideInterval > 0 && now - lastSlideTime >= (uint64_t)slideInterval) {
        if (slideshowFromChain) {
            if (!loadChainNap()) {
                if (!samples.empty()) loadRandomNap();
            }
            slideshowFromChain = false;
        } else {
            if (!samples.empty()) {
                loadRandomNap();
            } else {
                loadChainNap();
            }
            slideshowFromChain = hasChain;
        }
    }
}

//--------------------------------------------------------------
// A random file from bin/data, never the one already on screen -- repeating a
// drawing reads as a frozen player, which is the thing the switch exists to
// avoid.
void ofApp::loadRandomNap() {
    const int count = (int)samples.size();
    int index = (int)ofRandom(count);
    if (index >= count) index = count - 1; // ofRandom's top end is inclusive

    if (count > 1 && index == sampleIndex) index = (index + 1) % count;

    sampleIndex = index;
    lastSlideTime = ofGetElapsedTimeMillis();

    loadNap(samples[sampleIndex]);
    napSource = "random";
}

//--------------------------------------------------------------
bool ofApp::loadChainNap() {
    std::string nap;
    {
        std::lock_guard<std::mutex> lock(tezosMutex);
        if (tezosDrawings.empty()) return false;

        int count = (int)tezosDrawings.size();
        int index = (int)ofRandom(count);
        if (index >= count) index = count - 1;
        if (count > 1 && index == tezosDrawingIndex) index = (index + 1) % count;
        tezosDrawingIndex = index;
        nap = tezosDrawings[index];
    }

    lastSlideTime = ofGetElapsedTimeMillis();
    showNap(nap, "(chain)");
    napSource = "chain";
    return true;
}

//--------------------------------------------------------------
void ofApp::tezosThreadFunc() {
    for (int i = 0; i < 5 && tezosRunning; i++) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    while (tezosRunning) {
        std::vector<std::string> found;

        try {
            ofxHTTP::Client client;

            {
                std::string url = tzktBase + "/contracts/" + tezosContract + "/bigmaps";
                ofxHTTP::GetRequest req(url);
                auto resp = client.execute(req);

                if (resp->isSuccess()) {
                    ofJson bigmaps = resp->json();
                    if (bigmaps.is_array()) {
                        for (auto& bm : bigmaps) {
                            if (!tezosRunning) break;
                            if (!bm.contains("ptr") || bm.value("activeKeys", 0) == 0) continue;

                            int ptr = bm["ptr"].get<int>();
                            std::string keysUrl = tzktBase + "/bigmaps/" + ofToString(ptr)
                                                  + "/keys?active=true&limit=100";
                            ofxHTTP::GetRequest keysReq(keysUrl);
                            auto keysResp = client.execute(keysReq);

                            if (keysResp->isSuccess()) {
                                ofJson keys = keysResp->json();
                                if (keys.is_array()) {
                                    for (auto& entry : keys) {
                                        if (entry.contains("value")) {
                                            collectNaplpsStrings(entry["value"], found, tezosMaxBytes);
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
            }

            if (found.empty() && tezosRunning) {
                std::string url = tzktBase + "/contracts/" + tezosContract + "/storage";
                ofxHTTP::GetRequest req(url);
                auto resp = client.execute(req);

                if (resp->isSuccess()) {
                    collectNaplpsStrings(resp->json(), found, tezosMaxBytes);
                }
            }

        } catch (const Poco::Exception& e) {
            ofLogWarning("Tezos") << "poll failed: " << e.displayText();
        } catch (const std::exception& e) {
            ofLogWarning("Tezos") << "poll failed: " << e.what();
        } catch (...) {
            ofLogWarning("Tezos") << "poll failed";
        }

        if (!found.empty()) {
            std::lock_guard<std::mutex> lock(tezosMutex);
            tezosDrawings = std::move(found);
            ofLogNotice("Tezos") << "cached " << tezosDrawings.size() << " drawings from chain";
        }

        for (int i = 0; i < tezosPollSeconds && tezosRunning; i++) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
    }
}

//--------------------------------------------------------------
void ofApp::exit() {
    tezosRunning = false;
    if (tezosThread.joinable()) tezosThread.join();
    
    drawingMode.exit();
    client.stop();
}

//--------------------------------------------------------------
// Frames from nap-xtz-server arrive in one of three shapes, set by
// RPI_NAPLPS_FORMAT on that side:
//
//   json    {"type":"naplps","source":"slideshow","encoding":"text","naplps":"..."}
//   base64  the same envelope, with the stream base64'd
//   raw     the NAPLPS stream on its own, no envelope
//
// Anything else on this port -- a camera command meant for one of the other
// Pinopticon apps, a keepalive -- is not a drawing and is left alone.
ofApp::NapFrame ofApp::parseNapFrame(const std::string & text) const {
    NapFrame frame;

    if (text.empty()) return frame;

    // A .nap stream opens with a control byte, never with '{'.
    if (text[0] != '{') {
        if (text == "take_photo" || text == "stream_photo" || text == "keepalive") return frame;
        frame.nap = text;
        frame.source = "raw";
        return frame;
    }

    ofxJSONElement json;
    if (!json.parse(text)) {
        ofLogWarning("PiNaplpsPlayer") << "frame wasn't valid JSON";
        return frame;
    }

    if (json["type"].asString() != "naplps") return frame;

    frame.source = json["source"].asString();

    // NAPLPS is a 7-bit-safe format, so "text" carries the stream through JSON
    // intact -- the control bytes travel as \u00xx escapes and come back whole.
    // "base64" is there for a payload that uses the high half anyway.
    const std::string payload = json["naplps"].asString();
    frame.nap = (json["encoding"].asString() == "base64")
        ? ofxCrypto::base64_decode(payload)
        : payload;

    return frame;
}

//--------------------------------------------------------------
void ofApp::onWebSocketOpenEvent(ofxHTTP::WebSocketEventArgs & evt) {
    connections++;
    ofLogNotice("PiNaplpsPlayer") << "websocket opened: " << evt.connection().clientAddress().toString();
}

//--------------------------------------------------------------
void ofApp::onWebSocketCloseEvent(ofxHTTP::WebSocketCloseEventArgs & evt) {
    if (connections > 0) connections--;
    ofLogNotice("PiNaplpsPlayer") << "websocket closed: " << evt.connection().clientAddress().toString();
}

//--------------------------------------------------------------
void ofApp::onWebSocketFrameReceivedEvent(ofxHTTP::WebSocketFrameEventArgs & evt) {
    std::string payload;
    evt.frame().readBytes(payload);
    const NapFrame frame = parseNapFrame(payload);
    if (frame.nap.empty()) return;

    ofLogNotice("PiNaplpsPlayer") << "received " << frame.nap.size() << " bytes of NAPLPS"
                                  << (frame.source.empty() ? "" : " from " + frame.source);

    // Hand it to update(); this is a server thread, not the GL thread.
    std::lock_guard<std::mutex> lock(incomingMutex);
    incoming = frame;
    hasIncoming = true;
    received++;
}

//--------------------------------------------------------------
void ofApp::onWebSocketFrameSentEvent(ofxHTTP::WebSocketFrameEventArgs & evt) {
    // nothing to do -- the player only listens
}

//--------------------------------------------------------------
void ofApp::onWebSocketErrorEvent(ofxHTTP::WebSocketErrorEventArgs & evt) {
    ofLogWarning("PiNaplpsPlayer") << "websocket error: " << evt.connection().clientAddress().toString();
}

//--------------------------------------------------------------
void ofApp::keyPressed(int key) {
    if (drawingMode.isActive()) {
        drawingMode.keyPressed(key);
        return;
    }

    switch (key) {
        case ' ':
            telidon.reset();
            bFboDirty = true;
            break;
        case OF_KEY_RIGHT:
        case OF_KEY_DOWN:
            if (samples.empty()) break;
            sampleIndex = (sampleIndex + 1) % (int)samples.size();
            loadNap(samples[sampleIndex]);
            napSource = "file";
            // A hand on the keys outranks the switch: hold this drawing for a
            // full timeout before the slideshow takes over again.
            lastMessageTime = ofGetElapsedTimeMillis();
            slideshowActive = false;
            break;
        case OF_KEY_LEFT:
        case OF_KEY_UP:
            if (samples.empty()) break;
            sampleIndex = (sampleIndex + (int)samples.size() - 1) % (int)samples.size();
            loadNap(samples[sampleIndex]);
            napSource = "file";
            lastMessageTime = ofGetElapsedTimeMillis();
            slideshowActive = false;
            break;
        case 'p':
            progressiveDraw = !progressiveDraw;
            telidon.setProgressiveDraw(progressiveDraw);
            bFboDirty = true;
            break;
        case 'l':
            labelPoints = !labelPoints;
            telidon.setLabelPoints(labelPoints);
            bFboDirty = true;
            break;
        case 'i':
            showInfo = !showInfo;
            if (showInfo) updateInfoText();
            break;
        case 'f':
            ofToggleFullscreen();
            break;
        case 'd':
            // 'd' for drawing mode
            enterDrawingMode();
            break;
        case 's':
            // 's' for slideshow
            if (slideshowActive) stopSlideshow(); else startSlideshow();
            break;
        case 'c':
            // 'c' for publish current drawing
            publishCurrent();
            break;
        case 'm':
            // 'm' for mint current drawing
            mintCurrent();
            break;
        default:
            break;
    }
}

//--------------------------------------------------------------
void ofApp::windowResized(int w, int h) {
    updateLayout();
    telidon.setSize(drawSize, drawSize);
    
    if (drawingMode.isActive()) {
        // DrawingMode handles its own viewport in getViewport()
    }
}

//--------------------------------------------------------------
void ofApp::dragEvent(ofDragInfo dragInfo) {
    if (dragInfo.files.size() < 1) return;

    loadNap(dragInfo.files[0]);
    napSource = "file";
    lastMessageTime = ofGetElapsedTimeMillis();
    slideshowActive = false;
}

//--------------------------------------------------------------
void ofApp::updateInfoText() {
    infoText = naplps.fileName + "\n";
    infoText += "Telidon " + ofToString(naplps.version) + ", " + ofToString(naplps.cmds.size()) + " commands\n";
    infoText += telidon.isFinished() ? "finished\n" : "drawing...\n";
    infoText += "source: " + napSource + "\n";
    if (slideshowActive) {
        infoText += "no signal: slideshow every " + ofToString(slideInterval) + "ms\n";
    }
    {
        std::lock_guard<std::mutex> lock(tezosMutex);
        infoText += "chain: " + ofToString(tezosDrawings.size()) + " cached";
        if (!tezosContract.empty()) infoText += " (polling)";
        infoText += "\n";
    }
    infoText += "\n";
    infoText += "ws://" + hostName + ":" + ofToString(WS_PORT) + "\n";
    infoText += ofToString(connections) + " connected, " + ofToString(received) + " received\n";
    infoText += "\n";
    infoText += "arrows: next/prev file\n";
    infoText += "space:  redraw\n";
    infoText += "p:      progressive draw " + std::string(progressiveDraw ? "on" : "off") + "\n";
    infoText += "l:      label points " + std::string(labelPoints ? "on" : "off") + "\n";
    infoText += "i:      hide this\n";
    infoText += "d:      enter drawing mode\n";
    infoText += "s:      toggle slideshow\n";
    infoText += "c:      publish current drawing\n";
    infoText += "m:      mint current drawing\n";
    infoText += "(or drop a .nap file on the window)";
}

//--------------------------------------------------------------
void ofApp::clearCanvas() {
    stopSlideshow();
    hasContent = false;
    pendingNapRaw.clear();
    napSource = "none";
    naplps.fileName = "";
    bFboDirty = true;
}

//--------------------------------------------------------------
void ofApp::enterDrawingMode() {
    // The browser's Live Drawing button empties the canvas (`telidon = []`) on
    // the way in, so leaving without drawing anything lands back on the
    // placeholder rather than on whatever was there before.
    clearCanvas();
    ofShowCursor();
    drawingMode.start();
}

//--------------------------------------------------------------
void ofApp::leaveDrawingMode() {
    drawingMode.stop(); // encodes whatever was drawn

    const std::string encoded = drawingMode.getEncodedNaplps();
    if (encoded.empty()) {
        ofLogNotice("PiNaplpsPlayer") << "left drawing mode with nothing drawn";
        bFboDirty = true;
        return;
    }

    // Show it on the NAPLPS canvas, then share it. This is the round trip the
    // whole app exists for: hands to vectors to every other client.
    showNap(encoded, "(live drawing)");
    napSource = "drawing";

    client.publish(encoded, "drawing");
    drawingMode.clearEncodedNaplps();
}

//--------------------------------------------------------------
void ofApp::startSlideshow() {
    if (drawingMode.isActive() || slideshowActive) return;
    slideshowActive = true;
    loadRandomNap();
}

//--------------------------------------------------------------
void ofApp::stopSlideshow() {
    slideshowActive = false;
}

//--------------------------------------------------------------
void ofApp::publishCurrent() {
    if (pendingNapRaw.empty()) {
        ofLogNotice("PiNaplpsPlayer") << "nothing on screen to publish";
        return;
    }
    client.publish(pendingNapRaw, "client");
    ofLogNotice("PiNaplpsPlayer") << "published " << pendingNapRaw.size() << " bytes";
}

//--------------------------------------------------------------
void ofApp::mintCurrent() {
    if (pendingNapRaw.empty()) {
        ofLogNotice("PiNaplpsPlayer") << "nothing on screen to mint";
        return;
    }

    // The browser signs with a Beacon wallet. There is no Beacon for C++, so
    // this asks the server to sign with its own key; it answers with a clear
    // error when TEZOS_SECRET_KEY isn't configured there.
    client.mintAsync(pendingNapRaw);
    showInfo = true; // so the result is visible when it lands
    updateInfoText();
}