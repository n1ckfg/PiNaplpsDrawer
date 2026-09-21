#pragma once

#include <atomic>
#include <mutex>
#include <thread>

#include "ofMain.h"

#include "ofxNaplps.h"

#include "ofxHTTP.h"
#include "ofxJSONElement.h"
#include "ofxCrypto.h"
#include "ofxXmlSettings.h"

#include "NapClient.h"
#include "DrawingMode.h"


// The largest drawing the player will accept over a websocket, matching
// RPI_MAX_BYTES on the server. ofxHTTP defaults its websocket buffer to 8 KB,
// which is smaller than every .nap file in bin/data -- and Poco drops a frame
// that won't fit the buffer, so at the default the player would simply never
// see a drawing arrive.
#define MAX_NAP_BYTES (1024 * 1024)

// The port nap-xtz-server connects to (its RPI_PORT), and the one the
// Pinopticon apps use for websockets.
#define WS_PORT 7112

// Canvas constants matching the web client.
const int kCanvasW = 640;
const int kCanvasH = 480;
const float kDrawAspect = (float)kCanvasW / (float)kCanvasH; // 4:3


class ofApp : public ofBaseApp {

    public:

        void setup();
        void update();
        void draw();
        void exit();

        void keyPressed(int key);
        void windowResized(int w, int h);
        void dragEvent(ofDragInfo dragInfo);

        void loadNap(const std::string & filePath);
        void scanSamples();
        void showNap(const std::string & napRaw, const std::string & label);
        void startDrawing();
        void updateLayout();

        Naplps naplps;   // the decoder,  ported from naplps.js
        Telidon telidon; // the renderer, ported from TelidonP5.js

        ofFbo fbo;

        ofxXmlSettings settings;

        std::vector<std::string> samples;
        int sampleIndex;

        int fboWidth;
        int fboHeight;

        // ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~
        // DEAD MAN'S SWITCH
        //
        // A player with nothing to play is indistinguishable from a broken
        // one, so when the network goes quiet for slideTimeout ms the app
        // falls back to the .nap files sitting in bin/data and shuffles
        // through them every slideInterval ms. The first network drawing to
        // arrive takes the screen back.
        // ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~

        void checkDeadMansSwitch();
        void loadRandomNap();

        int slideTimeout;   // ms of silence before the fallback kicks in; 0 disables it
        int slideInterval;  // ms between random drawings while it's running

        uint64_t lastMessageTime; // when the last network drawing landed
        uint64_t lastSlideTime;   // when the last random drawing was loaded
        bool slideshowActive;

        // The NAPLPS unit screen runs from (0,0) to (1,1), so it gets a square
        // of the window, centered.
        float drawSize;
        glm::vec2 drawOffset;

        bool debugView;
        bool progressiveDraw;
        bool labelPoints;
        bool showInfo;
        bool hasContent = false;
        bool triedChainFallback = false;

        bool bFboDirty;
        std::string infoText;
        void updateInfoText();

        // ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~
        // NETWORK (INBOUND - websocket server for nap-xtz-server)
        //
        // nap-xtz-server opens a websocket to this app and pushes drawings as
        // its own canvas draws them -- slideshow mode sends every frame it
        // plays. See that project's OTHER SERVERS section in app.js.
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

        ofShader shader;
        string shaderName;

        // ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~
        // TEZOS CHAIN READ
        //
        // A background thread polls TzKT for NAPLPS drawings stored
        // on-chain. During Slideshow Mode the player alternates
        // between local files and chain drawings; a failed or empty
        // poll is handled silently and the local slideshow continues.
        // ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~

        std::string tzktBase;
        std::string tezosContract;
        int tezosPollSeconds;
        int tezosMaxBytes;

        std::thread tezosThread;
        std::atomic<bool> tezosRunning{false};
        std::mutex tezosMutex;
        std::vector<std::string> tezosDrawings;
        int tezosDrawingIndex;
        bool slideshowFromChain;

        void tezosThreadFunc();
        bool loadChainNap();

        // ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~
        // NETWORK (OUTBOUND - client to nap-xtz-server)
        //
        // Connects to the nap-xtz-server backend for chain reads, minting,
        // and sharing drawings with other clients.
        // ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~

        NapClient client;
        std::string pendingNapRaw; // raw NAPLPS bytes of what's on screen

        // ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~
        // LIVE DRAWING MODE
        // ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~

        DrawingMode drawingMode;
        ofTrueTypeFont promptFont;
        float promptFontSize;

        // The browser's canvas is a 4:3 box centred in the window.
        // The NAPLPS artwork is a square shifted up by the quarter that
        // doesn't fit (translate(0, sH - sW)).
        glm::vec2 canvasSize;
        glm::vec2 canvasOffset;

        void clearCanvas();
        void enterDrawingMode();
        void leaveDrawingMode();

        void startSlideshow();
        void stopSlideshow();

        void publishCurrent();
        void mintCurrent();
};