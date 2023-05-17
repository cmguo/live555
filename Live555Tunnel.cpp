#include "Live555Tunnel.h"
#include "BasicUsageEnvironment.hh"
#include "GroupsockHelper.hh"
#include "RTSPClient.hh"

#include <queue>
#include <mutex>
#include <functional>
#include <sstream>

#include <assert.h>

// Implementation of "Live555TunnelSink":
// Even though we're not going to be doing anything with the incoming data, we
// still need to receive it. Define the size of the buffer that we'll use:
#define BBL_SINK_RECEIVE_BUFFER_SIZE 100000

// If you don't want to see debugging output for each received frame, then
// comment out the following line: #define DEBUG_PRINT_EACH_RECEIVED_FRAME 1

char const *kProgName = "BambuLabStudio";
char const *clientProtocolName = "RTSP";
static void periodicQOSMeasurement(void *clientData);  // forward
static unsigned nextQOSMeasurementUSecs;

bool areAlreadyShuttingDown = false;
int shutdownExitCode;

RTSPClient* ourRTSPClient = nullptr;
UsageEnvironment *env = nullptr;
TaskScheduler *scheduler = nullptr;

bool madeProgress = false;
bool allowProxyServers = false;
Authenticator *ourAuthenticator = nullptr;
char const *streamURL = nullptr;
MediaSession *session = nullptr;
TaskToken sessionTimerTask = nullptr;
TaskToken sessionTimeoutBrokenServerTask = nullptr;
TaskToken arrivalCheckTimerTask = nullptr;
TaskToken interPacketGapCheckTimerTask = nullptr;
TaskToken qosMeasurementTimerTask = nullptr;
bool createReceivers = true;
bool audioOnly = false;
bool videoOnly = false;
char const *singleMedium = nullptr;
int verbosityLevel = 1; // by default, print verbose output
double duration = 0;
double durationSlop = -1.0; // extra seconds to play at the end
double initialSeekTime = 0.0f;
char *initialAbsoluteSeekTime = nullptr;
char *initialAbsoluteSeekEndTime = nullptr;
float scale = 1.0f;
double endTime;
unsigned interPacketGapMaxTime = 0;
unsigned totNumPacketsReceived = ~0; // used if checking inter-packet gaps
bool playContinuously = false;
int simpleRTPoffsetArg = -1;
bool sendOptionsRequest = true;
bool sendOptionsRequestOnly = false;
bool notifyOnPacketArrival = false;
bool sendKeepAlivesToBrokenServers = false;
unsigned sessionTimeoutParameter = 0;
bool streamUsingTCP = false;
bool forceMulticastOnUnspecified = false;
unsigned short desiredPortNum = 0;
portNumBits tunnelOverHTTPPortNum = 0;
unsigned BblSinkBufferSize = BBL_SINK_RECEIVE_BUFFER_SIZE;
unsigned socketInputBufferSize = 0;
bool syncStreams = false;
bool waitForResponseToTEARDOWN = true;
unsigned qosMeasurementIntervalMS = 0; // 0 means: Don't output QOS data
char *userAgent = "BambuLabStudio_RTSP_Client"; // specify a user agent name to use in outgoing requests
bool createHandlerServerForREGISTERCommand = false;
portNumBits handlerServerForREGISTERCommandPortNum = 0;
HandlerServerForREGISTERCommand *handlerServerForREGISTERCommand;
UserAuthenticationDatabase *authDBForREGISTER = nullptr;
struct timeval startTime;

struct Live555Tunnel
{
    Live555Logger logger = nullptr;
    void* log_context = nullptr;
    Live555BlockArrive callback = nullptr;
    void* context = nullptr;

} * tunnel = nullptr;

void assignClient(Medium *client);
void getOptions(RTSPClient::responseHandler *afterFunc);
void getSDPDescription(RTSPClient::responseHandler *afterFunc);
void setupSubsession(MediaSubsession *subsession, bool streamUsingTCP, bool forceMulticastOnUnspecified, RTSPClient::responseHandler *afterFunc);
void startPlayingSession(MediaSession *session, double start, double end, float scale, RTSPClient::responseHandler *afterFunc);
// For playing by 'absolute' time (using strings of the form "YYYYMMDDTHHMMSSZ" or "YYYYMMDDTHHMMSS.<frac>Z"
void startPlayingSession(MediaSession *session, char const *absStartTime, char const *absEndTime, float scale, RTSPClient::responseHandler *afterFunc);
void tearDownSession(MediaSession *session, RTSPClient::responseHandler *afterFunc);
void setUserAgentString(char const *userAgentString);

RTSPClient *createClient(UsageEnvironment &env, char const *URL, int verbosityLevel,
                     char const *applicationName);
// Forward function definitions:
void continueAfterClientCreation0(RTSPClient *client, bool requestStreamingOverTCP);
void continueAfterClientCreation1();
void continueAfterOPTIONS(RTSPClient *client, int resultCode, char *resultString);
void continueAfterDESCRIBE(RTSPClient *client, int resultCode, char *resultString);
void continueAfterSETUP(RTSPClient *client, int resultCode, char *resultString);
void continueAfterPLAY(RTSPClient *client, int resultCode, char *resultString);
void continueAfterTEARDOWN(RTSPClient *client, int resultCode, char *resultString);

void createOutputSink();
void setupStreams();
void closeMediaSinks();
void subsessionAfterPlaying(void *clientData);
void subsessionByeHandler(void *clientData, char const *reason);
void sessionAfterPlaying(void *clientData = nullptr);
void sessionTimerHandler(void *clientData);
void shutdown(int exitCode);
void signalHandlerShutdown(int sig);
void checkForPacketArrival(void *clientData);
void checkInterPacketGaps(void *clientData);
void checkSessionTimeoutBrokenServer(void *clientData);
void beginQOSMeasurement();

class Live555TunnelSink : public MediaSink {
   public:
    /**
     * @brief Create a New Live555TunnelSink object
     *
     * @param env live555 env
     * @param subsession identifies the kind of data that's being received
     * @param streamId  identifies the stream itself (optional)
     * @return Live555TunnelSink* sink pointer
     */
    static Live555TunnelSink *createNew(UsageEnvironment &env, MediaSubsession &subsession,
        char const *streamId);

   private:
    Live555TunnelSink(UsageEnvironment &env, MediaSubsession &subsession,
                  char const *streamId);
    // called only by "createNew()"
    virtual ~Live555TunnelSink();

    static void afterGettingFrame(void *clientData, unsigned frameSize,
                                  unsigned numTruncatedBytes,
                                  struct timeval presentationTime,
                                  unsigned durationInMicroseconds);
    void afterGettingFrame(unsigned frameSize, unsigned numTruncatedBytes,
                           struct timeval presentationTime,
                           unsigned durationInMicroseconds);

   private:
    // redefined virtual functions:
    virtual bool continuePlaying();

   private:
    u_int8_t *fReceiveBuffer;
    MediaSubsession &fSubsession;
    char *fStreamId;

    std::mutex buf_queue_mutex;
};

class qosMeasurementRecord {
   public:
    qosMeasurementRecord(struct timeval const &startTime, RTPSource *src)
        : fSource(src),
          fNext(nullptr),
          kbits_per_second_min(1e20),
          kbits_per_second_max(0),
          kBytesTotal(0.0),
          packet_loss_fraction_min(1.0),
          packet_loss_fraction_max(0.0),
          totNumPacketsReceived(0),
          totNumPacketsExpected(0) {
        measurementEndTime = measurementStartTime = startTime;

        RTPReceptionStatsDB::Iterator statsIter(src->receptionStatsDB());
        // Assume that there's only one SSRC source (usually the case):
        RTPReceptionStats *stats = statsIter.next(true);
        if (stats != nullptr) {
            kBytesTotal = stats->totNumKBytesReceived();
            totNumPacketsReceived = stats->totNumPacketsReceived();
            totNumPacketsExpected = stats->totNumPacketsExpected();
        }
    }
    virtual ~qosMeasurementRecord() { delete fNext; }

    void periodicQOSMeasurement(struct timeval const &timeNow);

   public:
    RTPSource *fSource;
    qosMeasurementRecord *fNext;

   public:
    struct timeval measurementStartTime, measurementEndTime;
    double kbits_per_second_min, kbits_per_second_max;
    double kBytesTotal;
    double packet_loss_fraction_min, packet_loss_fraction_max;
    unsigned totNumPacketsReceived, totNumPacketsExpected;
};

class TunnelUsageEnvironment : public BasicUsageEnvironment
{
public:
    TunnelUsageEnvironment(TaskScheduler& taskScheduler)
        : BasicUsageEnvironment(taskScheduler) {}
    std::ostringstream oss;
    virtual UsageEnvironment& operator<<(char const* str)
    {
        oss << str;
        if (oss.str().back() == '\n' && tunnel->logger) {
            tunnel->logger(tunnel->context, 1, oss.str().c_str());
            oss.str("");
            oss.clear();
        }
        return *this;
    }
    virtual UsageEnvironment& operator<<(int i)
    {
        oss << i;
        return *this;
    }
    virtual UsageEnvironment& operator<<(unsigned u)
    {
        oss << u;
        return *this;
    }
    virtual UsageEnvironment& operator<<(double d)
    {
        oss << d;
        return *this;
    }
    virtual UsageEnvironment& operator<<(void* p)
    {
        oss << p;
        return *this;
    }
};

static qosMeasurementRecord *qosRecordHead = nullptr;

LIVE555_EXPORT int LIVE555_FUNC(Live555Tunnel_Create)(Live555Tunnel** tunnel)
{
    int ret = Rtsp_success;

    if (audioOnly && videoOnly) {
        *env << "audioOnly and videoOnly cannot both be used!\n";
        return Rtsp_stream_end;
    }
    if (sendOptionsRequestOnly && !sendOptionsRequest) {
        *env << "The sendOptionsRequestOnly and sendOptionsRequest options cannot both be used!\n";
        return Rtsp_stream_end;
    }
    if (initialAbsoluteSeekTime != nullptr && initialSeekTime != 0.0f) {
        *env << "The initialAbsoluteSeekTime and initialSeekTime options cannot both be used!\n";
        return Rtsp_stream_end;
    }
    if (initialAbsoluteSeekTime == nullptr &&
        initialAbsoluteSeekEndTime != nullptr) {
        *env << "The initialAbsoluteSeekTime option requires the initialAbsoluteSeekEndTime option!\n";
        return Rtsp_stream_end;
    }
    if (authDBForREGISTER != nullptr &&
        !createHandlerServerForREGISTERCommand) {
        *env << "If authDBForREGISTER is used, then createHandlerServerForREGISTERCommand must also be used!\n";
        return Rtsp_stream_end;
    }
    if (tunnelOverHTTPPortNum > 0) {
        if (streamUsingTCP) {
            *env << "The tunnelOverHTTPPortNum and streamUsingTCP options cannot both be used!\n";
            return Rtsp_stream_end;
        } else {
            streamUsingTCP = true;
        }
    }
    if (!createReceivers && notifyOnPacketArrival) {
        *env << "Warning: Because we're not receiving stream data, the -n flag "
                "has no effect\n";
        return Rtsp_stream_end;
    }
    if (durationSlop < 0) {
        // This parameter wasn't set, so use a default value.
        // If we're measuring QOS stats, then don't add any slop, to avoid
        // having 'empty' measurement intervals at the end.
        durationSlop = qosMeasurementIntervalMS > 0 ? 0.0 : 5.0;
    }

    // Begin by setting up our usage environment:
    scheduler = BasicTaskScheduler::createNew();
    env = new TunnelUsageEnvironment(*scheduler);

    // TODO: chunmao.guo check: need passed by caller
    ourAuthenticator = new Authenticator("username1", "password1");

    assert(::tunnel == nullptr);
    *tunnel = ::tunnel = new Live555Tunnel;

    return ret;
}

LIVE555_EXPORT void LIVE555_FUNC(Live555Tunnel_SetLogger)(Live555Tunnel* tunnel, Live555Logger logger, void* context)
{
    assert(tunnel == ::tunnel);
    tunnel->logger = logger;
    tunnel->log_context = context;
}

LIVE555_EXPORT void LIVE555_FUNC(Live555Tunnel_SetBlockCallback)(Live555Tunnel* tunnel, Live555BlockArrive callback, void* context)
{
    assert(tunnel == ::tunnel);
    tunnel->callback = callback;
    tunnel->context = context;
}

LIVE555_EXPORT int LIVE555_FUNC(Live555Tunnel_Loop)(Live555Tunnel* tunnel, char* watch_variable)
{
    assert(tunnel == ::tunnel);
    // All subsequent activity takes place within the event loop:
    env->taskScheduler().doEventLoop(watch_variable);
    // This function call does not return, unless, at some point in time,
    // "eventLoopWatchVariable" gets set to something non-zero.
    return 0;
}

LIVE555_EXPORT void LIVE555_FUNC(Live555Tunnel_FreeBuffer)(Live555Tunnel* tunnel, unsigned char const* buffer)
{
    assert(tunnel == ::tunnel);
    delete [] buffer;
}

LIVE555_EXPORT int LIVE555_FUNC(Live555Tunnel_GetLastError)(Live555Tunnel* tunnel)
{
    assert(tunnel == ::tunnel);
    return 0;
}

LIVE555_EXPORT tchar const* LIVE555_FUNC(Live555Tunnel_GetLastErrorMsg)(Live555Tunnel* tunnel)
{
    assert(tunnel == ::tunnel);
    return nullptr;
}

LIVE555_EXPORT int Live555Tunnel_Open(Live555Tunnel* tunnel, char const* url) {
    // TODO: chunmao.guo check: need passed by caller
    assert(tunnel == ::tunnel);
    streamURL = url;

    // Create (or arrange to create) our client object:
    if (createHandlerServerForREGISTERCommand) {
        handlerServerForREGISTERCommand =
            HandlerServerForREGISTERCommand::createNew(
                *env, continueAfterClientCreation0,
                handlerServerForREGISTERCommandPortNum, authDBForREGISTER,
                verbosityLevel, kProgName);
        if (handlerServerForREGISTERCommand == nullptr) {
            *env << "Failed to create a server for handling incoming "
                    "\"REGISTER\" commands: "
                 << env->getResultMsg() << "\n";
            return 1;
        } else {
            *env << "Awaiting an incoming \"REGISTER\" command on port "
                 << handlerServerForREGISTERCommand->serverPortNum() << "\n";
        }
    } else {
        ourRTSPClient = createClient(*env, streamURL, verbosityLevel, kProgName);
        if (ourRTSPClient == nullptr) {
            *env << "Failed to create " << clientProtocolName
                 << " client: " << env->getResultMsg() << "\n";
            return 1;
        }
        continueAfterClientCreation1();
    }
    return 0;
}

LIVE555_EXPORT void Live555Tunnel_Close(Live555Tunnel* tunnel)
{
    assert(tunnel == ::tunnel);
    // to ensure that we end, even if the server does not respond to our TEARDOWN
    waitForResponseToTEARDOWN = false;
    shutdown(0);
}

LIVE555_EXPORT void Live555Tunnel_Destroy(Live555Tunnel* tunnel)
{
    assert(tunnel == ::tunnel);
    env->reclaim();
    env = nullptr;
    delete scheduler;
    scheduler = nullptr;
    ::tunnel = nullptr;
    return;
}

Live555TunnelSink *Live555TunnelSink::createNew(UsageEnvironment &env,
                                        MediaSubsession &subsession,
                                        char const *streamId) {
    return new Live555TunnelSink(env, subsession, streamId);
}

Live555TunnelSink::Live555TunnelSink(UsageEnvironment &env, MediaSubsession &subsession,
                             char const *streamId)
    : MediaSink(env), fSubsession(subsession) {
    fStreamId = strDup(streamId);
    fReceiveBuffer = new u_int8_t[BBL_SINK_RECEIVE_BUFFER_SIZE];
    fReceiveBuffer[0] = 0;
    fReceiveBuffer[1] = 0;
    fReceiveBuffer[2] = 0;
    fReceiveBuffer[3] = 1;
}

Live555TunnelSink::~Live555TunnelSink() {
    delete[] fReceiveBuffer;
    delete[] fStreamId;
}

void Live555TunnelSink::afterGettingFrame(void *clientData, unsigned frameSize,
                                      unsigned numTruncatedBytes,
                                      struct timeval presentationTime,
                                      unsigned durationInMicroseconds) {
    Live555TunnelSink *sink = (Live555TunnelSink *)clientData;
    sink->afterGettingFrame(frameSize, numTruncatedBytes, presentationTime,
                            durationInMicroseconds);
}

void Live555TunnelSink::afterGettingFrame(unsigned frameSize,
                                      unsigned numTruncatedBytes,
                                      struct timeval presentationTime,
                                      unsigned /*durationInMicroseconds*/) {
    // We've just received a frame of data.  (Optionally) print out information
    // about it:
#ifdef DEBUG_PRINT_EACH_RECEIVED_FRAME
    if (fStreamId != nullptr) envir() << "Stream \"" << fStreamId << "\"; ";

    envir() << fSubsession.mediumName() << "/" << fSubsession.codecName()
            << ":\tReceived " << frameSize << " bytes";
    if (numTruncatedBytes > 0)
        envir() << " (with " << numTruncatedBytes << " bytes truncated)";

    char uSecsStr[6 + 1];  // used to output the 'microseconds' part of the
                           // presentation time
    sprintf(uSecsStr, "%06u", (unsigned)presentationTime.tv_usec);
    envir() << ".\tPresentation time: " << (int)presentationTime.tv_sec << "."
            << uSecsStr;

    if (fSubsession.rtpSource() != nullptr &&
        !fSubsession.rtpSource()->hasBeenSynchronizedUsingRTCP()) {
        envir() << "!";  // mark the debugging output to indicate that this
                         // presentation time is not RTCP-synchronized
    }
#ifdef DEBUG_PRINT_NPT
    envir() << "\tNPT: " << fSubsession.getNormalPlayTime(presentationTime);
#endif
    envir() << "\n";
#endif

    Live555Block block;

    block.track = 0;
    block.decode_time =
        (presentationTime.tv_sec * 1000000 + presentationTime.tv_usec) * 10;
    block.size = frameSize + 4;
    block.flags = 0x1;
    block.buffer = fReceiveBuffer;
    fReceiveBuffer = new u_int8_t[BBL_SINK_RECEIVE_BUFFER_SIZE];
    fReceiveBuffer[0] = 0;
    fReceiveBuffer[1] = 0;
    fReceiveBuffer[2] = 0;
    fReceiveBuffer[3] = 1;

    tunnel->callback(tunnel->context, &block);

    // Then continue, to request the next frame of data:
    continuePlaying();
}

bool Live555TunnelSink::continuePlaying() {
    if (fSource == nullptr) return false;  // sanity check (should not happen)

    // Request the next frame of data from our input source.
    // "afterGettingFrame()" will get called later, when it arrives:
    fSource->getNextFrame(fReceiveBuffer + 4, BBL_SINK_RECEIVE_BUFFER_SIZE,
                          afterGettingFrame, this, onSourceClosure, this);
    return true;
}

RTSPClient *createClient(UsageEnvironment &env, char const *url, int verbosityLevel,
                     char const *applicationName) {
    extern portNumBits tunnelOverHTTPPortNum;
    return RTSPClient::createNew(env, url, verbosityLevel, applicationName,
                                 tunnelOverHTTPPortNum);
}

void assignClient(Medium *client) {
    ourRTSPClient = (RTSPClient *)client;
}

void getOptions(RTSPClient::responseHandler *afterFunc) {
    ourRTSPClient->sendOptionsCommand(afterFunc, ourAuthenticator);
}

void getSDPDescription(
    RTSPClient::responseHandler *afterFunc) {
    ourRTSPClient->sendDescribeCommand(afterFunc, ourAuthenticator);
}

void setupSubsession(MediaSubsession *subsession, bool streamUsingTCP,
    bool forceMulticastOnUnspecified, RTSPClient::responseHandler *afterFunc) {
    ourRTSPClient->sendSetupCommand(*subsession, afterFunc, false,
                                    streamUsingTCP, forceMulticastOnUnspecified,
                                    ourAuthenticator);
}

void startPlayingSession(
    MediaSession *session, double start, double end, float scale,
    RTSPClient::responseHandler *afterFunc) {
    ourRTSPClient->sendPlayCommand(*session, afterFunc, start, end, scale,
                                   ourAuthenticator);
}

void startPlayingSession(
    MediaSession *session, char const *absStartTime, char const *absEndTime,
    float scale, RTSPClient::responseHandler *afterFunc) {
    ourRTSPClient->sendPlayCommand(*session, afterFunc, absStartTime,
                                   absEndTime, scale, ourAuthenticator);
}

void tearDownSession(
    MediaSession *session, RTSPClient::responseHandler *afterFunc) {
    ourRTSPClient->sendTeardownCommand(*session, afterFunc, ourAuthenticator);
}

void setUserAgentString(char const *userAgentString) {
    ourRTSPClient->setUserAgentString(userAgentString);
}

void continueAfterClientCreation0(
    RTSPClient *newRTSPClient, bool requestStreamingOverTCP) {
    if (newRTSPClient == nullptr) return;

    streamUsingTCP = requestStreamingOverTCP;

    assignClient(ourRTSPClient = newRTSPClient);
    streamURL = newRTSPClient->url();

    // Having handled one "REGISTER" command (giving us a "rtsp://" URL to
    // stream from), we don't handle any more:
    Medium::close(handlerServerForREGISTERCommand);
    handlerServerForREGISTERCommand = nullptr;

    continueAfterClientCreation1();
}

void continueAfterClientCreation1() {
    setUserAgentString(userAgent);

    if (sendOptionsRequest) {
        // Begin by sending an "OPTIONS" command:
        getOptions(continueAfterOPTIONS);
    } else {
        continueAfterOPTIONS(nullptr, 0, nullptr);
    }
}

void continueAfterOPTIONS(RTSPClient *, int resultCode,
                                              char *resultString) {
    if (sendOptionsRequestOnly) {
        if (resultCode != 0) {
            *env << clientProtocolName
                 << " \"OPTIONS\" request failed: " << resultString << "\n";
        } else {
            *env << clientProtocolName
                 << " \"OPTIONS\" request returned: " << resultString << "\n";
        }
        shutdown(resultCode);
        return;
    }
    delete[] resultString;

    // Next, get a SDP description for the stream:
    getSDPDescription(continueAfterDESCRIBE);
}

void continueAfterDESCRIBE(RTSPClient *, int resultCode,
                                               char *resultString) {
    if (resultCode != 0) {
        *env << "Failed to get a SDP description for the URL \"" << streamURL
             << "\": " << resultString << "\n";
        delete[] resultString;
        shutdown(resultCode);
        return;
    }

    char *sdpDescription = resultString;
    *env << "Opened URL \"" << streamURL << "\", returning a SDP description:\n"
         << sdpDescription << "\n";

    // Create a media session object from this SDP description:
    session = MediaSession::createNew(*env, sdpDescription);
    delete[] sdpDescription;
    if (session == nullptr) {
        *env << "Failed to create a MediaSession object from the SDP "
                "description: "
             << env->getResultMsg() << "\n";
        shutdown(-100);
        return;
    } else if (!session->hasSubsessions()) {
        *env << "This session has no media subsessions (i.e., no \"m=\" "
                "lines)\n";
        shutdown(-101);
        return;
    }

    // Then, setup the "RTPSource"s for the session:
    MediaSubsessionIterator iter(*session);
    MediaSubsession *subsession;
    bool madeProgress = false;
    char const *singleMediumToTest = singleMedium;
    while ((subsession = iter.next()) != nullptr) {
        // If we've asked to receive only a single medium, then check this now:
        if (singleMediumToTest != nullptr) {
            if (strcmp(subsession->mediumName(), singleMediumToTest) != 0) {
                *env
                    << "Ignoring \"" << subsession->mediumName() << "/"
                    << subsession->codecName()
                    << "\" subsession, because we've asked to receive a single "
                    << singleMedium << " session only\n";
                continue;
            } else {
                // Receive this subsession only
                singleMediumToTest = "xxxxx";
                // this hack ensures that we get only 1 subsession of this type
            }
        }

        if (desiredPortNum != 0) {
            subsession->setClientPortNum(desiredPortNum);
            desiredPortNum += 2;
        }

        if (createReceivers) {
            if (!subsession->initiate(simpleRTPoffsetArg)) {
                *env << "Unable to create receiver for \""
                     << subsession->mediumName() << "/"
                     << subsession->codecName()
                     << "\" subsession: " << env->getResultMsg() << "\n";
            } else {
                *env << "Created receiver for \"" << subsession->mediumName()
                     << "/" << subsession->codecName() << "\" subsession (";
                if (subsession->rtcpIsMuxed()) {
                    *env << "client port " << subsession->clientPortNum();
                } else {
                    *env << "client ports " << subsession->clientPortNum()
                         << "-" << subsession->clientPortNum() + 1;
                }
                *env << ")\n";
                madeProgress = true;

                if (subsession->rtpSource() != nullptr) {
                    // Because we're saving the incoming data, rather than
                    // playing it in real time, allow an especially large time
                    // threshold (1 second) for reordering misordered incoming
                    // packets:
                    unsigned const thresh = 1000000;  // 1 second
                    subsession->rtpSource()->setPacketReorderingThresholdTime(
                        thresh);

                    // Set the RTP source's OS socket buffer size as appropriate
                    // - either if we were explicitly asked (using -B), or if
                    // the desired FileSink buffer size happens to be larger
                    // than the current OS socket buffer size. (The latter case
                    // is a heuristic, on the assumption that if the user asked
                    // for a large FileSink buffer size, then the input data
                    // rate may be large enough to justify increasing the OS
                    // socket buffer size also.)
                    int socketNum =
                        subsession->rtpSource()->RTPgs()->socketNum();
                    unsigned curBufferSize =
                        getReceiveBufferSize(*env, socketNum);
                    if (socketInputBufferSize > 0 ||
                        BblSinkBufferSize > curBufferSize) {
                        unsigned newBufferSize = socketInputBufferSize > 0
                                                     ? socketInputBufferSize
                                                     : BblSinkBufferSize;
                        newBufferSize =
                            setReceiveBufferTo(*env, socketNum, newBufferSize);
                        if (socketInputBufferSize >
                            0) {  // The user explicitly asked for the new
                                  // socket buffer size; announce it:
                            *env << "Changed socket receive buffer size for "
                                    "the \""
                                 << subsession->mediumName() << "/"
                                 << subsession->codecName()
                                 << "\" subsession from " << curBufferSize
                                 << " to " << newBufferSize << " bytes\n";
                        }
                    }
                }
            }
        } else {
            if (subsession->clientPortNum() == 0) {
                *env << "No client port was specified for the \""
                     << subsession->mediumName() << "/"
                     << subsession->codecName()
                     << "\" subsession.  (Try adding the \"-p <portNum>\" "
                        "option.)\n";
            } else {
                madeProgress = true;
            }
        }
    }
    if (!madeProgress) {
        shutdown(-102);
        return;
    }

    // Perform additional 'setup' on each subsession, before playing them:MediaSubsession *subsession;
    setupStreams();
}

MediaSubsession *subsession;
void continueAfterSETUP(RTSPClient *client, int resultCode,
                                            char *resultString) {
    if (resultCode == 0) {
        *env << "Setup \"" << subsession->mediumName() << "/"
             << subsession->codecName() << "\" subsession (";
        if (subsession->rtcpIsMuxed()) {
            *env << "client port " << subsession->clientPortNum();
        } else {
            *env << "client ports " << subsession->clientPortNum() << "-"
                 << subsession->clientPortNum() + 1;
        }
        *env << ")\n";
        madeProgress = true;
    } else {
        *env << "Failed to setup \"" << subsession->mediumName() << "/"
             << subsession->codecName() << "\" subsession: " << resultString
             << "\n";
    }
    delete[] resultString;

    if (client != nullptr)
        sessionTimeoutParameter = client->sessionTimeoutParameter();

    // Set up the next subsession, if any:
    setupStreams();
}

void createOutputSink() {
    madeProgress = false;
    MediaSubsessionIterator iter(*session);
    while ((subsession = iter.next()) != nullptr) {
        if (subsession->readSource() == nullptr) continue;  // was not initiated

        MediaSink *sink = nullptr;
        if (strcmp(subsession->mediumName(), "video") == 0) {
            if (strcmp(subsession->codecName(), "H264") == 0) {
                // For H.264 video stream, we use a special sink that adds
                // 'start codes', and (at the start) the SPS and PPS NAL units:
                sink = Live555TunnelSink::createNew(*env, *subsession, ourRTSPClient->url());
            } else if (strcmp(subsession->codecName(), "H265") == 0) {
                // For H.265 video stream, we use a special sink that adds
                // 'start codes', and (at the start) the VPS, SPS, and PPS NAL
                // units:
                /* fileSink = H265VideoFileSink::createNew(*env, outFileName,
                                                        subsession->fmtp_spropvps(),
                                                        subsession->fmtp_spropsps(),
                                                        subsession->fmtp_sproppps(),
                                                        BblSinkBufferSize,
                                                        oneFilePerFrame);
                 */
                *env << "[BambuTunnelLive555::createOutputSink] not "
                                  "support h265 now";
            } else if (strcmp(subsession->codecName(), "THEORA") == 0) {
                *env << "[BambuTunnelLive555::createOutputSink] not "
                                  "support THEORA now";
            }
        } else if (strcmp(subsession->mediumName(), "audio") == 0) {
            if (strcmp(subsession->codecName(), "AMR") == 0 ||
                strcmp(subsession->codecName(), "AMR-WB") == 0) {
                *env << "[BambuTunnelLive555::createOutputSink] not "
                                  "support audio now";
            } else if (strcmp(subsession->codecName(), "VORBIS") == 0 ||
                       strcmp(subsession->codecName(), "OPUS") == 0) {
                *env << "[BambuTunnelLive555::createOutputSink] not "
                                  "support audio now";
            } else if (strcmp(subsession->codecName(), "MPEG4-GENERIC") == 0) {
                *env << "[BambuTunnelLive555::createOutputSink] not "
                                  "support audio now";
            }
            *env << "[BambuTunnelLive555::createOutputSink] not "
                              "support audio now";
        }

        subsession->sink = sink;

        if (subsession->sink == nullptr) {
            *env << "Failed to create MediaSink for \""
                 << "\": " << env->getResultMsg() << "\n";
        } else {
            if (singleMedium == nullptr) {
                *env << "Created output file: \""  << "\"\n";
            } else {
                *env << "Outputting data from the \""
                     << subsession->mediumName() << "/"
                     << subsession->codecName() << "\" subsession to \""
                     << "\"\n";
            }

            if (strcmp(subsession->mediumName(), "video") == 0 &&
                strcmp(subsession->codecName(), "MP4V-ES") == 0 &&
                subsession->fmtp_config() != nullptr) {
                // For MPEG-4 video RTP streams, the 'config' information
                // from the SDP description contains useful VOL etc. headers.
                // Insert this data at the front of the output file:
                /*unsigned configLen;
                unsigned char *configData =
                parseGeneralConfigStr(subsession->fmtp_config(), configLen);
                struct timeval timeNow;
                gettimeofday(&timeNow, nullptr);
                fileSink->addData(configData, configLen, timeNow);
                delete[] configData;
                */
                *env << "[BambuTunnelLive555::createOutputSink] not "
                                  "support video MP4V-ES now";
            }

            subsession->sink->startPlaying(*(subsession->readSource()),
                                           subsessionAfterPlaying, subsession);

            // Also set a handler to be called if a RTCP "BYE" arrives
            // for this subsession:
            if (subsession->rtcpInstance() != nullptr) {
                subsession->rtcpInstance()->setByeWithReasonHandler(
                    subsessionByeHandler, subsession);
            }

            madeProgress = true;
        }
    }

    if (!madeProgress) shutdown(-103);
}

void setupStreams() {
    static MediaSubsessionIterator *setupIter = nullptr;
    if (setupIter == nullptr) setupIter = new MediaSubsessionIterator(*session);

    while ((subsession = setupIter->next()) != nullptr) {
        // We have another subsession left to set up:
        if (subsession->clientPortNum() == 0) continue;  // port # was not set

        setupSubsession(subsession, streamUsingTCP, forceMulticastOnUnspecified,
                        continueAfterSETUP);
        return;
    }

    // We're done setting up subsessions.
    delete setupIter;
    if (!madeProgress) { shutdown(-104); return; }

    if (createReceivers) {
        createOutputSink();
    }

    // Finally, start playing each subsession, to start the data flow:
    if (duration == 0) {
        if (scale > 0)
            duration =
                session->playEndTime() - initialSeekTime;  // use SDP end time
        else if (scale < 0)
            duration = initialSeekTime;
    }
    if (duration < 0) duration = 0.0;

    endTime = initialSeekTime;
    if (scale > 0) {
        if (duration <= 0)
            endTime = -1.0f;
        else
            endTime = initialSeekTime + duration;
    } else {
        endTime = initialSeekTime - duration;
        if (endTime < 0) endTime = 0.0f;
    }

    char const *absStartTime = initialAbsoluteSeekTime != nullptr
                                   ? initialAbsoluteSeekTime
                                   : session->absStartTime();
    char const *absEndTime = initialAbsoluteSeekEndTime != nullptr
                                 ? initialAbsoluteSeekEndTime
                                 : session->absEndTime();
    if (absStartTime != nullptr) {
        // Either we or the server have specified that seeking should be done by
        // 'absolute' time:
        startPlayingSession(session, absStartTime, absEndTime, scale,
                            continueAfterPLAY);
    } else {
        // Normal case: Seek by relative time (NPT):
        startPlayingSession(session, initialSeekTime, endTime, scale,
                            continueAfterPLAY);
    }
}

void continueAfterPLAY(RTSPClient *, int resultCode,
                                           char *resultString) {
    if (resultCode != 0) {
        *env << "Failed to start playing session: " << resultString << "\n";
        delete[] resultString;
        shutdown(resultCode);
        return;
    } else {
        *env << "Started playing session\n";
    }
    delete[] resultString;

    if (qosMeasurementIntervalMS > 0) {
        // Begin periodic QOS measurements:
        beginQOSMeasurement();
    }

    // Figure out how long to delay (if at all) before shutting down, or
    // repeating the playing
    bool timerIsBeingUsed = false;
    double secondsToDelay = duration;
    if (duration > 0) {
        // First, adjust "duration" based on any change to the play range (that
        // was specified in the "PLAY" response):
        double rangeAdjustment =
            (session->playEndTime() - session->playStartTime()) -
            (endTime - initialSeekTime);
        if (duration + rangeAdjustment > 0.0) duration += rangeAdjustment;

        timerIsBeingUsed = true;
        double absScale = scale > 0 ? scale : -scale;  // ASSERT: scale != 0
        secondsToDelay = duration / absScale + durationSlop;

        int64_t uSecsToDelay = (int64_t)(secondsToDelay * 1000000.0);
        sessionTimerTask = env->taskScheduler().scheduleDelayedTask(
            uSecsToDelay, (TaskFunc *)sessionTimerHandler, (void *)nullptr);
    }

    char const *actionString =
        createReceivers ? "Receiving streamed data" : "Data is being streamed";
    if (timerIsBeingUsed) {
        *env << actionString << " (for up to " << secondsToDelay
             << " seconds)...\n";
    } else {
#ifdef USE_SIGNALS
        pid_t ourPid = getpid();
        *env << actionString << " (signal with \"kill -HUP " << (int)ourPid
             << "\" or \"kill -USR1 " << (int)ourPid << "\" to terminate)...\n";
#else
        *env << actionString << "...\n";
#endif
    }

    sessionTimeoutBrokenServerTask = nullptr;

    // Watch for incoming packets (if desired):
    checkForPacketArrival(nullptr);
    checkInterPacketGaps(nullptr);
    checkSessionTimeoutBrokenServer(nullptr);
}

void closeMediaSinks() {
    if (session == nullptr) return;
    MediaSubsessionIterator iter(*session);
    MediaSubsession *subsession;
    while ((subsession = iter.next()) != nullptr) {
        Medium::close(subsession->sink);
        subsession->sink = nullptr;
    }
}

void subsessionAfterPlaying(void *clientData) {
    // Begin by closing this media subsession's stream:
    MediaSubsession *subsession = (MediaSubsession *)clientData;
    Medium::close(subsession->sink);
    subsession->sink = nullptr;

    // Next, check whether *all* subsessions' streams have now been closed:
    MediaSession &session = subsession->parentSession();
    MediaSubsessionIterator iter(session);
    while ((subsession = iter.next()) != nullptr) {
        if (subsession->sink != nullptr)
            return;  // this subsession is still active
    }

    // All subsessions' streams have now been closed
    sessionAfterPlaying();
}

void subsessionByeHandler(void *clientData,
                                              char const *reason) {
    struct timeval timeNow;
    gettimeofday(&timeNow, nullptr);
    unsigned secsDiff = timeNow.tv_sec - startTime.tv_sec;

    MediaSubsession *subsession = (MediaSubsession *)clientData;
    *env << "Received RTCP \"BYE\"";
    if (reason != nullptr) {
        *env << " (reason:\"" << reason << "\")";
        delete[] (char *)reason;
    }
    *env << " on \"" << subsession->mediumName() << "/"
         << subsession->codecName() << "\" subsession (after " << secsDiff
         << " seconds)\n";

    // Act now as if the subsession had closed:
    subsessionAfterPlaying(subsession);
}

void sessionAfterPlaying(void * /*clientData*/) {
    if (!playContinuously) {
        shutdown(0);
    } else {
        // We've been asked to play the stream(s) over again.
        // First, reset state from the current session:
        if (env != nullptr) {
            // Keep this running:
            env->taskScheduler().unscheduleDelayedTask(sessionTimerTask);
            env->taskScheduler().unscheduleDelayedTask(
                sessionTimeoutBrokenServerTask);
            env->taskScheduler().unscheduleDelayedTask(arrivalCheckTimerTask);
            env->taskScheduler().unscheduleDelayedTask(
                interPacketGapCheckTimerTask);
            env->taskScheduler().unscheduleDelayedTask(qosMeasurementTimerTask);
        }
        totNumPacketsReceived = ~0;

        startPlayingSession(session, initialSeekTime, endTime, scale,
                            continueAfterPLAY);
    }
}

void sessionTimerHandler(void * /*clientData*/) {
    sessionTimerTask = nullptr;

    sessionAfterPlaying();
}

static void scheduleNextQOSMeasurement() {
    nextQOSMeasurementUSecs += qosMeasurementIntervalMS * 1000;
    struct timeval timeNow;
    gettimeofday(&timeNow, nullptr);
    unsigned timeNowUSecs = timeNow.tv_sec * 1000000 + timeNow.tv_usec;
    int usecsToDelay = nextQOSMeasurementUSecs - timeNowUSecs;

    qosMeasurementTimerTask = env->taskScheduler().scheduleDelayedTask(
        usecsToDelay, (TaskFunc *)periodicQOSMeasurement, (void *)nullptr);
}

static void periodicQOSMeasurement(void * /*clientData*/) {
    struct timeval timeNow;
    gettimeofday(&timeNow, nullptr);

    for (qosMeasurementRecord *qosRecord = qosRecordHead; qosRecord != nullptr;
         qosRecord = qosRecord->fNext) {
        qosRecord->periodicQOSMeasurement(timeNow);
    }

    // Do this again later:
    scheduleNextQOSMeasurement();
}

void qosMeasurementRecord::periodicQOSMeasurement(
    struct timeval const &timeNow) {
    unsigned secsDiff = timeNow.tv_sec - measurementEndTime.tv_sec;
    int usecsDiff = timeNow.tv_usec - measurementEndTime.tv_usec;
    double timeDiff = secsDiff + usecsDiff / 1000000.0;
    measurementEndTime = timeNow;

    RTPReceptionStatsDB::Iterator statsIter(fSource->receptionStatsDB());
    // Assume that there's only one SSRC source (usually the case):
    RTPReceptionStats *stats = statsIter.next(true);
    if (stats != nullptr) {
        double kBytesTotalNow = stats->totNumKBytesReceived();
        double kBytesDeltaNow = kBytesTotalNow - kBytesTotal;
        kBytesTotal = kBytesTotalNow;

        double kbpsNow = timeDiff == 0.0 ? 0.0 : 8 * kBytesDeltaNow / timeDiff;
        if (kbpsNow < 0.0) kbpsNow = 0.0;  // in case of roundoff error
        if (kbpsNow < kbits_per_second_min) kbits_per_second_min = kbpsNow;
        if (kbpsNow > kbits_per_second_max) kbits_per_second_max = kbpsNow;

        unsigned totReceivedNow = stats->totNumPacketsReceived();
        unsigned totExpectedNow = stats->totNumPacketsExpected();
        unsigned deltaReceivedNow = totReceivedNow - totNumPacketsReceived;
        unsigned deltaExpectedNow = totExpectedNow - totNumPacketsExpected;
        totNumPacketsReceived = totReceivedNow;
        totNumPacketsExpected = totExpectedNow;

        double lossFractionNow =
            deltaExpectedNow == 0
                ? 0.0
                : 1.0 - deltaReceivedNow / (double)deltaExpectedNow;
        // if (lossFractionNow < 0.0) lossFractionNow = 0.0; //reordering can
        // cause
        if (lossFractionNow < packet_loss_fraction_min) {
            packet_loss_fraction_min = lossFractionNow;
        }
        if (lossFractionNow > packet_loss_fraction_max) {
            packet_loss_fraction_max = lossFractionNow;
        }
    }
}

void beginQOSMeasurement() {
    // Set up a measurement record for each active subsession:
    struct timeval startTime;
    gettimeofday(&startTime, nullptr);
    nextQOSMeasurementUSecs = startTime.tv_sec * 1000000 + startTime.tv_usec;
    qosMeasurementRecord *qosRecordTail = nullptr;
    MediaSubsessionIterator iter(*session);
    MediaSubsession *subsession;
    while ((subsession = iter.next()) != nullptr) {
        RTPSource *src = subsession->rtpSource();
        if (src == nullptr) continue;

        qosMeasurementRecord *qosRecord =
            new qosMeasurementRecord(startTime, src);
        if (qosRecordHead == nullptr) qosRecordHead = qosRecord;
        if (qosRecordTail != nullptr) qosRecordTail->fNext = qosRecord;
        qosRecordTail = qosRecord;
    }

    // Then schedule the first of the periodic measurements:
    scheduleNextQOSMeasurement();
}

void printQOSData(int exitCode) {
    *env << "begin_QOS_statistics\n";

    // Print out stats for each active subsession:
    qosMeasurementRecord *curQOSRecord = qosRecordHead;
    if (session != nullptr) {
        MediaSubsessionIterator iter(*session);
        MediaSubsession *subsession;
        while ((subsession = iter.next()) != nullptr) {
            RTPSource *src = subsession->rtpSource();
            if (src == nullptr) continue;

            *env << "subsession\t" << subsession->mediumName() << "/"
                 << subsession->codecName() << "\n";

            unsigned numPacketsReceived = 0, numPacketsExpected = 0;

            if (curQOSRecord != nullptr) {
                numPacketsReceived = curQOSRecord->totNumPacketsReceived;
                numPacketsExpected = curQOSRecord->totNumPacketsExpected;
            }
            *env << "num_packets_received\t" << numPacketsReceived << "\n";
            *env << "num_packets_lost\t"
                 << int(numPacketsExpected - numPacketsReceived) << "\n";

            if (curQOSRecord != nullptr) {
                unsigned secsDiff = curQOSRecord->measurementEndTime.tv_sec -
                                    curQOSRecord->measurementStartTime.tv_sec;
                int usecsDiff = curQOSRecord->measurementEndTime.tv_usec -
                                curQOSRecord->measurementStartTime.tv_usec;
                double measurementTime = secsDiff + usecsDiff / 1000000.0;
                *env << "elapsed_measurement_time\t" << measurementTime << "\n";

                *env << "kBytes_received_total\t" << curQOSRecord->kBytesTotal
                     << "\n";

                *env << "measurement_sampling_interval_ms\t"
                     << qosMeasurementIntervalMS << "\n";

                if (curQOSRecord->kbits_per_second_max == 0) {
                    // special case: we didn't receive any data:
                    *env << "kbits_per_second_min\tunavailable\n"
                            "kbits_per_second_ave\tunavailable\n"
                            "kbits_per_second_max\tunavailable\n";
                } else {
                    *env << "kbits_per_second_min\t"
                         << curQOSRecord->kbits_per_second_min << "\n";
                    *env << "kbits_per_second_ave\t"
                         << (measurementTime == 0.0
                                 ? 0.0
                                 : 8 * curQOSRecord->kBytesTotal /
                                       measurementTime)
                         << "\n";
                    *env << "kbits_per_second_max\t"
                         << curQOSRecord->kbits_per_second_max << "\n";
                }

                *env << "packet_loss_percentage_min\t"
                     << 100 * curQOSRecord->packet_loss_fraction_min << "\n";
                double packetLossFraction =
                    numPacketsExpected == 0
                        ? 1.0
                        : 1.0 - numPacketsReceived / (double)numPacketsExpected;
                if (packetLossFraction < 0.0) packetLossFraction = 0.0;
                *env << "packet_loss_percentage_ave\t"
                     << 100 * packetLossFraction << "\n";
                *env << "packet_loss_percentage_max\t"
                     << (packetLossFraction == 1.0
                             ? 100.0
                             : 100 * curQOSRecord->packet_loss_fraction_max)
                     << "\n";

                RTPReceptionStatsDB::Iterator statsIter(
                    src->receptionStatsDB());
                // Assume that there's only one SSRC source (usually the case):
                RTPReceptionStats *stats = statsIter.next(true);
                if (stats != nullptr) {
                    *env << "inter_packet_gap_ms_min\t"
                         << stats->minInterPacketGapUS() / 1000.0 << "\n";
                    struct timeval totalGaps = stats->totalInterPacketGaps();
                    double totalGapsMS =
                        totalGaps.tv_sec * 1000.0 + totalGaps.tv_usec / 1000.0;
                    unsigned totNumPacketsReceived =
                        stats->totNumPacketsReceived();
                    *env << "inter_packet_gap_ms_ave\t"
                         << (totNumPacketsReceived == 0
                                 ? 0.0
                                 : totalGapsMS / totNumPacketsReceived)
                         << "\n";
                    *env << "inter_packet_gap_ms_max\t"
                         << stats->maxInterPacketGapUS() / 1000.0 << "\n";
                }

                curQOSRecord = curQOSRecord->fNext;
            }
        }
    }

    *env << "end_QOS_statistics\n";
    delete qosRecordHead;
}

void shutdown(int exitCode) {
    if (areAlreadyShuttingDown)
        return;  // in case we're called after receiving a RTCP "BYE" while in
                 // the middle of a "TEARDOWN".

    areAlreadyShuttingDown = true;

    shutdownExitCode = exitCode;
    if (env != nullptr) {
        env->taskScheduler().unscheduleDelayedTask(sessionTimerTask);
        env->taskScheduler().unscheduleDelayedTask(
            sessionTimeoutBrokenServerTask);
        env->taskScheduler().unscheduleDelayedTask(arrivalCheckTimerTask);
        env->taskScheduler().unscheduleDelayedTask(
            interPacketGapCheckTimerTask);
        env->taskScheduler().unscheduleDelayedTask(qosMeasurementTimerTask);
    }

    if (qosMeasurementIntervalMS > 0) {
        printQOSData(exitCode);
    }

    // Teardown, then shutdown, any outstanding RTP/RTCP subsessions
    bool shutdownImmediately = true;  // by default
    if (session != nullptr) {
        RTSPClient::responseHandler *responseHandlerForTEARDOWN =
            nullptr;  // unless:
        if (waitForResponseToTEARDOWN) {
            shutdownImmediately = false;
            responseHandlerForTEARDOWN = continueAfterTEARDOWN;
        }
        tearDownSession(session, responseHandlerForTEARDOWN);
    }

    if (shutdownImmediately) continueAfterTEARDOWN(nullptr, 0, nullptr);

    Live555Block block {0, 0, 0, exitCode, nullptr};
    tunnel->callback(tunnel->context, &block);
}

void continueAfterTEARDOWN(RTSPClient *, int /*resultCode*/,
                                               char *resultString) {
    delete[] resultString;

    // Now that we've stopped any more incoming data from arriving, close our
    // output files:
    closeMediaSinks();
    Medium::close(session);

    // Finally, shut down our client:
    if (nullptr != ourAuthenticator) {
        delete ourAuthenticator;
    }
    if (nullptr != authDBForREGISTER) {
        delete authDBForREGISTER;
    }

    Medium::close(ourRTSPClient);
}

void checkForPacketArrival(void * /*clientData*/) {
    arrivalCheckTimerTask = nullptr;
    if (!notifyOnPacketArrival) return;  // we're not checking

    // Check each subsession, to see whether it has received data packets:
    unsigned numSubsessionsChecked = 0;
    unsigned numSubsessionsWithReceivedData = 0;
    unsigned numSubsessionsThatHaveBeenSynced = 0;

    MediaSubsessionIterator iter(*session);
    MediaSubsession *subsession;
    while ((subsession = iter.next()) != nullptr) {
        RTPSource *src = subsession->rtpSource();
        if (src == nullptr) continue;
        ++numSubsessionsChecked;

        if (src->receptionStatsDB().numActiveSourcesSinceLastReset() > 0) {
            // At least one data packet has arrived
            ++numSubsessionsWithReceivedData;
        }
        if (src->hasBeenSynchronizedUsingRTCP()) {
            ++numSubsessionsThatHaveBeenSynced;
        }
    }

    unsigned numSubsessionsToCheck = numSubsessionsChecked;

    bool notifyTheUser;
    if (!syncStreams) {
        notifyTheUser = numSubsessionsWithReceivedData > 0;  // easy case
    } else {
        notifyTheUser =
            numSubsessionsWithReceivedData >= numSubsessionsToCheck &&
            numSubsessionsThatHaveBeenSynced == numSubsessionsChecked;
        // Note: A subsession with no active sources is considered to be synced
    }
    if (notifyTheUser) {
        struct timeval timeNow;
        gettimeofday(&timeNow, nullptr);
        char timestampStr[100];
        sprintf(timestampStr, "%ld%03ld", timeNow.tv_sec,
                (long)(timeNow.tv_usec / 1000));
        *env << (syncStreams ? "Synchronized d" : "D")
             << "ata packets have begun arriving [" << timestampStr
             << "]\007\n";
        return;
    }

    // No luck, so reschedule this check again, after a delay:
    int uSecsToDelay = 100000;  // 100 ms
    arrivalCheckTimerTask = env->taskScheduler().scheduleDelayedTask(
        uSecsToDelay, (TaskFunc *)checkForPacketArrival, nullptr);
}

void checkInterPacketGaps(void * /*clientData*/) {
    interPacketGapCheckTimerTask = nullptr;
    if (interPacketGapMaxTime == 0) return;  // we're not checking

    // Check each subsession, counting up how many packets have been received:
    unsigned newTotNumPacketsReceived = 0;

    MediaSubsessionIterator iter(*session);
    MediaSubsession *subsession;
    while ((subsession = iter.next()) != nullptr) {
        RTPSource *src = subsession->rtpSource();
        if (src == nullptr) continue;
        newTotNumPacketsReceived +=
            src->receptionStatsDB().totNumPacketsReceived();
    }

    if (newTotNumPacketsReceived == totNumPacketsReceived) {
        // No additional packets have been received since the last time we
        // checked, so end this stream:
        *env << "Closing session, because we stopped receiving packets.\n";
        interPacketGapCheckTimerTask = nullptr;
        sessionAfterPlaying();
    } else {
        totNumPacketsReceived = newTotNumPacketsReceived;
        // Check again, after the specified delay:
        interPacketGapCheckTimerTask = env->taskScheduler().scheduleDelayedTask(
            interPacketGapMaxTime * 1000000, (TaskFunc *)checkInterPacketGaps,
            nullptr);
    }
}

void checkSessionTimeoutBrokenServer(void * /*clientData*/) {
    if (!sendKeepAlivesToBrokenServers) return;  // we're not checking

    // Send an "OPTIONS" request, starting with the second call
    if (sessionTimeoutBrokenServerTask != nullptr) {
        getOptions(nullptr);
    }

    unsigned sessionTimeout =
        sessionTimeoutParameter == 0 ? 60 /*default*/ : sessionTimeoutParameter;
    unsigned secondsUntilNextKeepAlive =
        sessionTimeout <= 5 ? 1 : sessionTimeout - 5;
    // Reduce the interval a little, to be on the safe side

    sessionTimeoutBrokenServerTask = env->taskScheduler().scheduleDelayedTask(
        secondsUntilNextKeepAlive * 1000000,
        (TaskFunc *)checkSessionTimeoutBrokenServer, nullptr);
}
