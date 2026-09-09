#pragma once

#include <mutex>

#include "ofMain.h"

#include "ofxNaplps.h"

#include "ofxHTTP.h"
#include "ofxJSONElement.h"
#include "ofxCrypto.h"

#include "DrawingMode.h"
#include "NapClient.h"

// The largest drawing the player will accept over a websocket, matching
// RPI_MAX_BYTES on the server. ofxHTTP defaults its websocket buffer to 8 KB,
// which is smaller than every .nap file in bin/data -- and Poco drops a frame
// that won't fit the buffer, so at the default the player would simply never
// see a drawing arrive.
#define MAX_NAP_BYTES (1024 * 1024)

// The port nap-xtz-server connects to (its RPI_PORT), and the one the
// Pinopticon apps use for websockets.
#define WS_PORT 7112

class ofApp : public ofBaseApp {

    public:

        void setup();
        void update();
        void draw();
        void exit();

        void keyPressed(int key);
        void keyReleased(int key);
        void mouseMoved(int x, int y);
        void mouseDragged(int x, int y, int button);
        void mousePressed(int x, int y, int button);
        void mouseReleased(int x, int y, int button);
        void mouseScrolled(int x, int y, float scrollX, float scrollY);
        void windowResized(int w, int h);
        void dragEvent(ofDragInfo dragInfo);

        void loadNap(const std::string & filePath);
        void showNap(const std::string & napRaw, const std::string & label);
        void startDrawing();
        void updateLayout();

        /// The browser's "clear" link: empties the canvas back to its
        /// drag-and-drop placeholder.
        void clearCanvas();

        Naplps naplps;   // the decoder,  ported from naplps.js
        Telidon telidon; // the renderer, ported from TelidonP5.js

        ofFbo fbo;

        std::vector<std::string> samples;
        int sampleIndex;

        // The browser's canvas is 640x480 (index.html: sW, sH), scaled to fill
        // the window while keeping that aspect ratio and centred in it.
        static constexpr float kCanvasW = 640.0f;
        static constexpr float kCanvasH = 480.0f;

        // Where that canvas lands in this window, in pixels.
        glm::vec2 canvasSize;
        glm::vec2 canvasOffset;

        // The NAPLPS unit screen runs from (0,0) to (1,1) and is rendered into a
        // SQUARE the width of the canvas, then shifted up by the quarter that
        // overhangs it -- index.html's `scale(scaleFactor); translate(0, sH-sW)`.
        // So what shows is the bottom three quarters of that square, which is
        // the convention the SVG importer and convertToNaplps() both encode to.
        float drawSize;
        glm::vec2 drawOffset;

        /// False when nothing is loaded, which is the browser's empty state:
        /// black, with the drag-and-drop prompt in the middle.
        bool hasContent;
        ofTrueTypeFont promptFont;
        float promptFontSize;

        /// Set once the startup chain read has been given up on, so the local
        /// samples are only substituted in the once.
        bool triedChainFallback;

        bool progressiveDraw;
        bool labelPoints;
        bool showInfo;

        bool bFboDirty;
        std::string infoText;
        void updateInfoText();

        // ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~
        // LIVE DRAWING
        //
        // The port of the browser's live drawing overlay. It runs instead of the
        // NAPLPS canvas rather than over it: there is one window here, and the
        // two views never made sense at once.
        // ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~

        DrawingMode drawingMode;

        void enterDrawingMode();
        void leaveDrawingMode();

        // ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~
        // SLIDESHOW
        //
        // Ported from index.html: plays a random .nap from bin/data on an
        // interval. Loading anything deliberately, or entering live drawing,
        // takes over from it.
        // ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~

        void startSlideshow();
        void stopSlideshow();
        void loadRandomNap();

        bool slideshowActive;
        float slideshowInterval; // seconds
        float lastSlideTime;

        /// The browser's slideshow pushes every frame it plays to the Pi. This
        /// app may itself be the Pi that nap-xtz-server pushes to, in which case
        /// doing the same would feed drawings straight back to us -- so it is
        /// off unless deliberately turned on.
        bool sendSlideshowToRpi;

        // ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~
        // NETWORK (OUTBOUND) -- the port of js/net/client.js
        //
        // Connects out to nap-xtz-server: receives drawings from every other
        // client, publishes the ones made here, and reaches the chain through
        // the server's REST API.
        // ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~

        NapClient client;

        /// The drawing currently on screen, kept so it can be published or
        /// minted. The browser calls this window.pendingNapRaw.
        std::string pendingNapRaw;

        void publishCurrent();
        void mintCurrent();

        // ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~
        // NETWORK (INBOUND)
        //
        // nap-xtz-server also opens a websocket *to* this app and pushes
        // drawings as its own canvas draws them. Both directions are live at
        // once: this app is a client of the server and, at the same time, the
        // Pinopticon player the server pushes to.
        // ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~

        // One drawing lifted out of a websocket frame. nap is empty when the
        // frame wasn't a drawing at all.
        struct NapFrame {
            std::string nap;
            std::string source; // "slideshow", "chain", ... when the sender says
        };

        NapFrame parseNapFrame(const std::string & text) const;

        ofxHTTP::SimpleWebSocketServer wsServer;

        void onWebSocketOpenEvent(ofxHTTP::WebSocketEventArgs & evt);
        void onWebSocketCloseEvent(ofxHTTP::WebSocketCloseEventArgs & evt);
        void onWebSocketFrameReceivedEvent(ofxHTTP::WebSocketFrameEventArgs & evt);
        void onWebSocketFrameSentEvent(ofxHTTP::WebSocketFrameEventArgs & evt);
        void onWebSocketErrorEvent(ofxHTTP::WebSocketErrorEventArgs & evt);

        // Frames arrive on one of the server's own threads, so a drawing waits
        // here until update() collects it: decoding and rendering both belong
        // to the GL thread.
        std::mutex incomingMutex;
        NapFrame incoming;
        bool hasIncoming;

        std::string hostName;   // read once; it shells out to `hostname`
        std::string napSource;  // where the drawing on screen came from
        int received;
        int connections;

};
