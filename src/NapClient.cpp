#include "NapClient.h"

#include "Poco/Buffer.h"
#include "Poco/Exception.h"
#include "Poco/Net/HTTPClientSession.h"
#include "Poco/Net/HTTPRequest.h"
#include "Poco/Net/HTTPResponse.h"
#include "Poco/Net/NetException.h"
#include "Poco/Net/WebSocket.h"
#include "Poco/StreamCopier.h"
#include "Poco/Timespan.h"

#include <istream>
#include <ostream>
#include <sstream>
#include <thread>

namespace {

/// How long a receive waits before looping to check the send queue and the stop
/// flag. Short enough that a quit doesn't hang, long enough not to spin.
const Poco::Timespan kReceiveTimeout(0, 200 * 1000); // 200 ms

/// Pause between reconnection attempts.
const int kReconnectDelayMs = 2000;

/// Matches the server's own ceiling for a drawing bound for the Pi. Poco needs a
/// buffer big enough for a whole frame or it drops the frame outright.
const int kMaxFrameBytes = 1024 * 1024;

} // namespace

//--------------------------------------------------------------
NapClient::~NapClient() {
    stop();
}

//--------------------------------------------------------------
void NapClient::setup(const Settings & _settings) {
    settings = _settings;
}

//--------------------------------------------------------------
void NapClient::start() {
    if (isThreadRunning()) return;
    shouldRun = true;
    setStatusText("connecting");
    startThread();
}

//--------------------------------------------------------------
void NapClient::stop() {
    shouldRun = false;
    if (isThreadRunning()) {
        waitForThread(true, 3000);
    }
    connected = false;
}

//--------------------------------------------------------------
void NapClient::setStatusText(const std::string & text) {
    std::lock_guard<std::mutex> lock(statusMutex);
    statusText = text;
}

//--------------------------------------------------------------
std::string NapClient::getStatusText() const {
    std::lock_guard<std::mutex> lock(statusMutex);
    return statusText;
}

//--------------------------------------------------------------
void NapClient::threadedFunction() {
    while (shouldRun.load() && isThreadRunning()) {
        runSession();

        if (!shouldRun.load()) break;

        // The server may simply not be up yet; keep trying rather than making
        // the app's link to it a one-shot at startup.
        setStatusText("reconnecting in " + ofToString(kReconnectDelayMs / 1000) + "s");
        for (int slept = 0; slept < kReconnectDelayMs && shouldRun.load(); slept += 100) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }

    connected = false;
    setStatusText("stopped");
}

//--------------------------------------------------------------
void NapClient::runSession() {
    using Poco::Net::HTTPClientSession;
    using Poco::Net::HTTPRequest;
    using Poco::Net::HTTPResponse;
    using Poco::Net::WebSocket;

    try {
        HTTPClientSession session(settings.host, settings.wsPort);
        HTTPRequest request(HTTPRequest::HTTP_GET, "/", HTTPRequest::HTTP_1_1);
        request.set("origin", "http://" + settings.host);
        HTTPResponse response;

        WebSocket webSocket(session, request, response);
        webSocket.setReceiveTimeout(kReceiveTimeout);
        webSocket.setMaxPayloadSize(kMaxFrameBytes);

        connected = true;
        setStatusText("connected to ws://" + settings.host + ":" + ofToString(settings.wsPort));
        ofLogNotice("NapClient") << "connected to ws://" << settings.host << ":" << settings.wsPort;

        Poco::Buffer<char> buffer(kMaxFrameBytes);

        while (shouldRun.load() && isThreadRunning()) {
            // ~ ~ ~ send ~ ~ ~
            // Drained first so a drawing made while the socket was down goes out
            // as soon as it comes back.
            for (;;) {
                std::string frame;
                {
                    std::lock_guard<std::mutex> lock(outgoingMutex);
                    if (outgoing.empty()) break;
                    frame = outgoing.front();
                    outgoing.pop_front();
                }
                webSocket.sendFrame(frame.data(), (int)frame.size(), WebSocket::FRAME_TEXT);
            }

            // ~ ~ ~ receive ~ ~ ~
            int flags = 0;
            int received = 0;
            try {
                received = webSocket.receiveFrame(buffer.begin(), (int)buffer.size(), flags);
            } catch (const Poco::TimeoutException &) {
                // Nothing waiting; loop round to the send queue and stop flag.
                continue;
            }

            if (received <= 0) {
                ofLogNotice("NapClient") << "server closed the connection";
                break;
            }

            if ((flags & WebSocket::FRAME_OP_BITMASK) == WebSocket::FRAME_OP_CLOSE) {
                ofLogNotice("NapClient") << "server sent close";
                break;
            }

            Message message;
            if (!parseMessage(std::string(buffer.begin(), received), message)) continue;

            std::lock_guard<std::mutex> lock(incomingMutex);
            incoming.push_back(message);
            while (incoming.size() > kMaxIncoming) incoming.pop_front();
        }

        webSocket.close();
    } catch (const Poco::Exception & e) {
        setStatusText("not connected: " + e.displayText());
        ofLogWarning("NapClient") << "websocket: " << e.displayText();
    } catch (const std::exception & e) {
        setStatusText(std::string("not connected: ") + e.what());
        ofLogWarning("NapClient") << "websocket: " << e.what();
    }

    connected = false;
}

//--------------------------------------------------------------
bool NapClient::parseMessage(const std::string & text, Message & out) const {
    if (text.empty()) return false;

    ofxJSONElement json;
    if (!json.parse(text)) return false;

    // The server multiplexes camera relays and errors over the same socket.
    // Only drawings are ours.
    const std::string type = json["type"].asString();
    if (type != "naplps") {
        if (type == "error_message") {
            ofLogWarning("NapClient") << "server rejected message: " << json["error"].asString();
        }
        return false;
    }

    out.naplps = json["naplps"].asString();
    if (out.naplps.empty()) return false;

    out.source = json["source"].asString();
    out.mid = json["mid"].asString();
    out.tokenId = json.isMember("id") ? json["id"].asInt() : -1;
    out.link = json["link"].asString();
    return true;
}

//--------------------------------------------------------------
bool NapClient::getNextMessage(Message & out) {
    std::lock_guard<std::mutex> lock(incomingMutex);
    if (incoming.empty()) return false;

    out = incoming.front();
    incoming.pop_front();
    return true;
}

//--------------------------------------------------------------
std::string NapClient::makeFrame(const std::string & type,
                                 const std::string & napRaw,
                                 const std::string & source) const {
    ofxJSONElement json;
    json["type"] = type;
    json["source"] = source;
    // NAPLPS is 7-bit safe, so the stream survives JSON as text: its control
    // bytes travel as \u00xx escapes and come back whole.
    json["naplps"] = napRaw;
    return json.getRawString(false);
}

//--------------------------------------------------------------
void NapClient::queueOutgoing(const std::string & frame) {
    std::lock_guard<std::mutex> lock(outgoingMutex);
    outgoing.push_back(frame);
    while (outgoing.size() > kMaxOutgoing) outgoing.pop_front();
}

//--------------------------------------------------------------
void NapClient::publish(const std::string & napRaw, const std::string & source) {
    if (napRaw.empty()) return;
    queueOutgoing(makeFrame("naplps", napRaw, source));
}

//--------------------------------------------------------------
void NapClient::sendToRpi(const std::string & napRaw, const std::string & source) {
    if (napRaw.empty()) return;
    queueOutgoing(makeFrame("rpi_naplps", napRaw, source));
}

//--------------------------------------------------------------
void NapClient::rpiCommand(const std::string & command) {
    ofxJSONElement json;
    json["type"] = "rpi_command";
    json["command"] = command;
    queueOutgoing(json.getRawString(false));
}

// ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~
// REST
// ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~

//--------------------------------------------------------------
bool NapClient::httpPostJson(const std::string & path,
                             const std::string & body,
                             std::string & outBody,
                             int & outStatus,
                             std::string & outError) const {
    using Poco::Net::HTTPClientSession;
    using Poco::Net::HTTPRequest;
    using Poco::Net::HTTPResponse;

    try {
        HTTPClientSession session(settings.host, settings.httpPort);
        // Minting waits on a chain operation, which is far slower than a normal
        // request; the default timeout would give up first.
        session.setTimeout(Poco::Timespan(60, 0));

        HTTPRequest request(HTTPRequest::HTTP_POST, path, HTTPRequest::HTTP_1_1);
        request.setContentType("application/json");
        request.setContentLength((std::streamsize)body.size());

        session.sendRequest(request) << body;

        HTTPResponse response;
        std::istream & responseStream = session.receiveResponse(response);

        std::ostringstream collected;
        Poco::StreamCopier::copyStream(responseStream, collected);

        outBody = collected.str();
        outStatus = (int)response.getStatus();
        return true;
    } catch (const Poco::Exception & e) {
        outError = e.displayText();
        return false;
    } catch (const std::exception & e) {
        outError = e.what();
        return false;
    }
}

//--------------------------------------------------------------
bool NapClient::httpGet(const std::string & path,
                        std::string & outBody,
                        int & outStatus,
                        std::string & outError) const {
    using Poco::Net::HTTPClientSession;
    using Poco::Net::HTTPRequest;
    using Poco::Net::HTTPResponse;

    try {
        HTTPClientSession session(settings.host, settings.httpPort);
        session.setTimeout(Poco::Timespan(20, 0));

        HTTPRequest request(HTTPRequest::HTTP_GET, path, HTTPRequest::HTTP_1_1);
        session.sendRequest(request);

        HTTPResponse response;
        std::istream & responseStream = session.receiveResponse(response);

        std::ostringstream collected;
        Poco::StreamCopier::copyStream(responseStream, collected);

        outBody = collected.str();
        outStatus = (int)response.getStatus();
        return true;
    } catch (const Poco::Exception & e) {
        outError = e.displayText();
        return false;
    } catch (const std::exception & e) {
        outError = e.what();
        return false;
    }
}

//--------------------------------------------------------------
bool NapClient::mint(const std::string & napRaw, std::string & outResult) {
    if (napRaw.empty()) {
        outResult = "nothing to mint";
        return false;
    }

    ofxJSONElement request;
    request["naplps"] = napRaw;

    std::string body;
    int status = 0;
    std::string error;

    if (!httpPostJson("/api/tezos/mint", request.getRawString(false), body, status, error)) {
        outResult = "mint failed: " + error;
        return false;
    }

    ofxJSONElement response;
    const bool parsed = response.parse(body);

    if (status < 200 || status >= 300) {
        // The server says why: usually that no TEZOS_SECRET_KEY is configured,
        // in which case minting has to happen in the browser instead.
        const std::string serverError = parsed ? response["error"].asString() : std::string();
        outResult = "mint failed (" + ofToString(status) + "): "
            + (serverError.empty() ? body : serverError);
        return false;
    }

    const std::string hash = parsed ? response["hash"].asString() : std::string();
    outResult = hash.empty() ? "minted" : ("minted: " + hash);
    return true;
}

//--------------------------------------------------------------
void NapClient::mintAsync(const std::string & napRaw) {
    {
        std::lock_guard<std::mutex> lock(mintMutex);
        if (mintState == MintState::Pending) return; // one at a time
        mintState = MintState::Pending;
        mintStatus = "minting...";
    }

    // Detached: the call can take tens of seconds waiting on the chain, and
    // nothing later depends on joining it -- the result is read through
    // getMintState(). The lambda copies what it needs.
    std::thread([this, napRaw]() {
        std::string result;
        const bool ok = mint(napRaw, result);

        std::lock_guard<std::mutex> lock(mintMutex);
        mintState = ok ? MintState::Succeeded : MintState::Failed;
        mintStatus = result;
    }).detach();
}

//--------------------------------------------------------------
NapClient::MintState NapClient::getMintState() const {
    std::lock_guard<std::mutex> lock(mintMutex);
    return mintState;
}

//--------------------------------------------------------------
std::string NapClient::getMintStatus() const {
    std::lock_guard<std::mutex> lock(mintMutex);
    return mintStatus;
}

//--------------------------------------------------------------
void NapClient::fetchLatestAsync() {
    FetchState expected = FetchState::Idle;
    if (!latestState.compare_exchange_strong(expected, FetchState::Pending)) {
        // Already in flight, or already answered. A second request while the
        // first is outstanding would only race it to the same queue.
        if (expected == FetchState::Pending) return;
        latestState.store(FetchState::Pending);
    }

    // Detached, like mintAsync(): a chain read goes out over the network and
    // nothing here waits on the result -- it arrives through the incoming queue.
    std::thread([this]() {
        Message message;
        std::string error;

        if (fetchLatest(message, error)) {
            {
                std::lock_guard<std::mutex> lock(incomingMutex);
                incoming.push_back(message);
                while (incoming.size() > kMaxIncoming) incoming.pop_front();
            }
            latestState.store(FetchState::Succeeded);
        } else {
            ofLogWarning("NapClient") << "latest: " << error;
            latestState.store(FetchState::Failed);
        }
    }).detach();
}

//--------------------------------------------------------------
bool NapClient::fetchLatest(Message & out, std::string & outError) {
    std::string body;
    int status = 0;

    if (!httpGet("/api/tezos/latest", body, status, outError)) return false;

    if (status < 200 || status >= 300) {
        outError = "latest failed (" + ofToString(status) + ")";
        return false;
    }

    ofxJSONElement json;
    if (!json.parse(body)) {
        outError = "latest: response wasn't JSON";
        return false;
    }

    out.naplps = json["naplps"].asString();
    if (out.naplps.empty()) {
        outError = "latest: no drawing on chain yet";
        return false;
    }

    out.source = "chain";
    out.tokenId = json.isMember("id") ? json["id"].asInt() : -1;
    out.link = json["link"].asString();
    return true;
}
