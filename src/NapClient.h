#pragma once

#include "ofMain.h"
#include "ofxJSONElement.h"

#include <deque>
#include <mutex>
#include <string>

// ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~
// Ported from public/js/net/client.js
//
// The app's only link to nap-xtz-server. Like the JS original it knows about
// routes and message shapes and nothing else -- no contract addresses, no TzKT,
// no RPC nodes. Drawings arrive here as messages; where they came from is the
// server's business.
//
// The browser client speaks socket.io. This one uses the plain `ws` server the
// backend runs alongside it (PORT_WS, default 4321), which accepts the same
// three message types -- naplps, rpi_naplps, rpi_command -- and broadcasts the
// same JSON back. Implementing the socket.io handshake in C++ would buy nothing.
// ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~

class NapClient : public ofThread {

    public:

        struct Settings {
            std::string host = "localhost";
            int wsPort = 4321;   // PORT_WS on the server
            int httpPort = 8080; // PORT_HTTP, for the REST calls
        };

        /// One drawing lifted out of a server message.
        struct Message {
            std::string naplps;
            std::string source; // "chain", "slideshow", "peer", "client", ...
            std::string mid;    // the server's dedupe id, carried through
            int tokenId = -1;   // set for source == "chain"
            std::string link;
        };

        ~NapClient() override;

        void setup(const Settings & settings);

        /// Opens the connection and starts reconnecting on failure. Returns
        /// immediately; poll isConnected().
        void start();
        void stop();

        bool isConnected() const { return connected.load(); }
        const Settings & getSettings() const { return settings; }

        /// Pops the next drawing received, if any. Call from the GL thread.
        bool getNextMessage(Message & out);

        // ~ ~ ~ outbound ~ ~ ~
        // All three queue the frame and return; the worker does the sending.

        /// Shares a drawing with every other connected client. No chain involved.
        void publish(const std::string & napRaw, const std::string & source = "client");
        /// Sends to the Raspberry Pi alone, without broadcasting.
        void sendToRpi(const std::string & napRaw, const std::string & source = "client");
        /// "take_photo" saves a file on the Pi; "stream_photo" sends one back.
        void rpiCommand(const std::string & command);

        // ~ ~ ~ REST ~ ~ ~
        // These block on the network, so they run on their own short-lived
        // thread rather than stalling the draw loop -- see mintAsync().

        /// Asks the server to mint headlessly with its own key. The browser
        /// signs with Beacon instead; there's no Beacon for C++, so this is the
        /// one path to chain from here, and it fails cleanly when the server has
        /// no TEZOS_SECRET_KEY set.
        bool mint(const std::string & napRaw, std::string & outResult);

        /// mint() on a detached thread. Poll getMintStatus() for the outcome.
        void mintAsync(const std::string & napRaw);

        enum class MintState { Idle, Pending, Succeeded, Failed };
        MintState getMintState() const;
        std::string getMintStatus() const;

        /// GET /api/tezos/latest -- the newest drawing on chain.
        bool fetchLatest(Message & out, std::string & outError);

        /// fetchLatest() on a detached thread. The drawing it finds is pushed
        /// into the same queue as a broadcast one, so getNextMessage() delivers
        /// it and the caller shows it exactly as it shows any other arrival.
        /// The browser reads the chain this way too -- preload() kicks off
        /// loadLatestToken() and the canvas sits on its placeholder until the
        /// answer lands -- and blocking the draw loop on an HTTP round trip
        /// would freeze the window instead.
        void fetchLatestAsync();

        enum class FetchState { Idle, Pending, Succeeded, Failed };
        FetchState getLatestState() const { return latestState.load(); }

        /// Human-readable connection state for the HUD.
        std::string getStatusText() const;

    private:

        void threadedFunction() override;

        /// One connect-and-pump cycle. Returns when the connection drops.
        void runSession();

        /// Parses a server frame into a Message. Returns false when the frame
        /// wasn't a drawing (an "rpi" relay, an error_message, a keepalive).
        bool parseMessage(const std::string & text, Message & out) const;

        /// Wraps a payload the way the server's ws handler expects.
        std::string makeFrame(const std::string & type,
                              const std::string & napRaw,
                              const std::string & source) const;

        void queueOutgoing(const std::string & frame);

        /// Blocking HTTP POST with a JSON body. Returns false on transport
        /// failure; an HTTP error status still fills outBody.
        bool httpPostJson(const std::string & path,
                          const std::string & body,
                          std::string & outBody,
                          int & outStatus,
                          std::string & outError) const;

        bool httpGet(const std::string & path,
                     std::string & outBody,
                     int & outStatus,
                     std::string & outError) const;

        Settings settings;

        std::atomic<bool> connected { false };
        std::atomic<bool> shouldRun { false };

        mutable std::mutex incomingMutex;
        std::deque<Message> incoming;
        /// A cap, so a burst while the app is busy can't grow without bound.
        static constexpr size_t kMaxIncoming = 16;

        mutable std::mutex outgoingMutex;
        std::deque<std::string> outgoing;
        static constexpr size_t kMaxOutgoing = 32;

        std::atomic<FetchState> latestState { FetchState::Idle };

        mutable std::mutex mintMutex;
        MintState mintState = MintState::Idle;
        std::string mintStatus;

        mutable std::mutex statusMutex;
        std::string statusText = "not started";
        void setStatusText(const std::string & text);

};
