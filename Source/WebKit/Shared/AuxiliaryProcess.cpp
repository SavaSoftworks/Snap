/*
 * Copyright (C) 2010 Apple Inc. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. AND ITS CONTRIBUTORS ``AS IS''
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
 * THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL APPLE INC. OR ITS CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF
 * THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "config.h"
#include "AuxiliaryProcess.h"

#include "AuxiliaryProcessCreationParameters.h"
#include "Connection.h"
#include "ContentWorldShared.h"
#include "LogInitialization.h"
#include "Logging.h"
#include "SandboxInitializationParameters.h"
#include "StreamClientConnection.h"
#include "WebPageProxyIdentifier.h"
#include <WebCore/LogInitialization.h>
#include <pal/SessionID.h>
#include <wtf/LogInitialization.h>
#include <wtf/MonotonicTime.h>
#include <wtf/NeverDestroyed.h>
#include <wtf/RunLoop.h>
#include <wtf/SetForScope.h>
#include <wtf/Threading.h>
#include <wtf/WTFProcess.h>
#include <wtf/text/CString.h>
#include <wtf/text/MakeString.h>
#include <wtf/text/StringToIntegerConversion.h>

#if !OS(WINDOWS)
#include <unistd.h>
#endif

#if OS(LINUX)
#include <cerrno>
#include <dirent.h>
#include <dlfcn.h>
#include <execinfo.h>
#include <fcntl.h>
#include <semaphore.h>
#include <signal.h>
#include <sys/syscall.h>
#include <time.h>
#include <wtf/MemoryPressureHandler.h>
#if USE(GLIB)
#include <wtf/glib/Sandbox.h>
#endif
#endif

#if PLATFORM(COCOA)
#include "CoreIPCSecureCoding.h"
#endif

namespace WebKit {
using namespace WebCore;

AuxiliaryProcess::AuxiliaryProcess()
    : m_terminationCounter(0)
    , m_processSuppressionDisabled("Process Suppression Disabled by UIProcess"_s)
{
}

AuxiliaryProcess::~AuxiliaryProcess()
{
    if (RefPtr connection = m_connection)
        connection->invalidate();
}

void AuxiliaryProcess::didClose(IPC::Connection&)
{
// Stop the run loop for GTK and WPE to ensure a normal exit, since stopRunLoop()
// releases resources like EGL displays before the process exits.
#if PLATFORM(GTK) || PLATFORM(WPE)
    stopRunLoop();
#else
    terminateProcess(EXIT_SUCCESS);
#endif
}

#if OS(LINUX)
static constexpr size_t maximumHangFrames = 64;
static std::atomic<pid_t> hangCaptureTarget { 0 };
static std::atomic<int> hangCaptureCount { 0 };
static std::array<void*, maximumHangFrames> hangCaptureFrames;
static sem_t hangCaptureDone;
static std::atomic<double> hangHeartbeat { 0 };

static pid_t currentThreadIdentifier()
{
    return static_cast<pid_t>(syscall(SYS_gettid));
}

static void captureHangStack(int)
{
    if (hangCaptureTarget.load() != currentThreadIdentifier())
        return;
    hangCaptureCount.store(backtrace(hangCaptureFrames.data(), static_cast<int>(hangCaptureFrames.size())));
    sem_post(&hangCaptureDone);
}

static CString hangThreadName(pid_t thread)
{
    std::array<char, 64> name {};
    auto path = makeString("/proc/self/task/"_s, thread, "/comm"_s).utf8();
    int descriptor = open(path.data(), O_RDONLY | O_CLOEXEC);
    if (descriptor < 0)
        return "unknown"_s;
    auto length = read(descriptor, name.data(), name.size() - 1);
    close(descriptor);
    if (length <= 0)
        return "unknown"_s;
    auto characters = std::span { name }.first(static_cast<size_t>(length));
    if (characters.back() == '\n')
        characters = characters.first(characters.size() - 1);
    return CString { characters };
}

static bool hangThreadBlocksSignal(pid_t thread, int signalNumber)
{
    std::array<char, 4096> status {};
    auto path = makeString("/proc/self/task/"_s, thread, "/status"_s).utf8();
    int descriptor = open(path.data(), O_RDONLY | O_CLOEXEC);
    if (descriptor < 0)
        return false;
    auto length = read(descriptor, status.data(), status.size() - 1);
    close(descriptor);
    if (length <= 0)
        return false;
    auto text = StringView::fromLatin1(status.data());
    auto start = text.find("SigBlk:\t"_s);
    if (start == notFound)
        return false;
    auto mask = text.substring(start + 8, 16);
    auto blocked = parseInteger<uint64_t>(mask, 16);
    return blocked && (*blocked >> (signalNumber - 1)) & 1;
}

static void printHangStack(pid_t thread, int signalNumber)
{
    WTFLogAlways("  thread %d (%s)", static_cast<int>(thread), hangThreadName(thread).data());
    if (hangThreadBlocksSignal(thread, signalNumber)) {
        WTFLogAlways("    blocks the capture signal");
        return;
    }
    while (!sem_trywait(&hangCaptureDone)) {
    }
    hangCaptureCount.store(0);
    hangCaptureTarget.store(thread);
    if (syscall(SYS_tgkill, getpid(), thread, signalNumber)) {
        hangCaptureTarget.store(0);
        WTFLogAlways("    could not signal the thread");
        return;
    }
    timespec deadline {};
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_nsec += 100'000'000;
    if (deadline.tv_nsec >= 1'000'000'000) {
        deadline.tv_sec += 1;
        deadline.tv_nsec -= 1'000'000'000;
    }
    int waited;
    do
        waited = sem_timedwait(&hangCaptureDone, &deadline);
    while (waited && errno == EINTR);
    hangCaptureTarget.store(0);
    if (waited) {
        WTFLogAlways("    the thread did not report its stack");
        return;
    }
    auto frames = std::span { hangCaptureFrames }.first(static_cast<size_t>(std::clamp(hangCaptureCount.load(), 0, static_cast<int>(maximumHangFrames))));
    for (auto* frame : frames) {
        Dl_info info {};
        if (dladdr(frame, &info) && info.dli_fname) {
            auto library = StringView::fromLatin1(info.dli_fname);
            auto slash = library.reverseFind('/');
            auto name = (slash == notFound ? library : library.substring(slash + 1)).utf8();
            WTFLogAlways("    %s+0x%zx", name.data(), static_cast<size_t>(reinterpret_cast<uintptr_t>(frame) - reinterpret_cast<uintptr_t>(info.dli_fbase)));
        } else
            WTFLogAlways("    %p", frame);
    }
}

static void printHangStacks(pid_t mainThread, int signalNumber)
{
    printHangStack(mainThread, signalNumber);
    DIR* directory = opendir("/proc/self/task");
    if (!directory)
        return;
    while (auto* entry = readdir(directory)) {
        auto thread = parseInteger<pid_t>(StringView::fromLatin1(entry->d_name));
        if (!thread || *thread == mainThread || *thread == currentThreadIdentifier())
            continue;
        printHangStack(*thread, signalNumber);
    }
    closedir(directory);
}

void startEdenHangRecorderIfRequested();

void startEdenHangRecorderIfRequested()
{
    static std::atomic<bool> started { false };
    auto milliseconds = parseInteger<unsigned>(StringView::fromLatin1(getenv("EDEN_WPE_HANG_LOG")));
    if (!milliseconds || !*milliseconds)
        return;
    if (started.exchange(true))
        return;
    if (sem_init(&hangCaptureDone, 0, 0))
        return;
    int signalNumber = SIGRTMIN + 5;
    struct sigaction action {};
    action.sa_handler = captureHangStack;
    action.sa_flags = SA_RESTART;
    sigemptyset(&action.sa_mask);
    if (sigaction(signalNumber, &action, nullptr))
        return;
    std::array<void*, 1> warmUp {};
    backtrace(warmUp.data(), static_cast<int>(warmUp.size()));

    auto threshold = Seconds::fromMilliseconds(*milliseconds);
    auto interval = std::min(threshold / 4, 50_ms);
    pid_t mainThread = currentThreadIdentifier();
    hangHeartbeat.store(MonotonicTime::now().secondsSinceEpoch().value());
    static NeverDestroyed<std::unique_ptr<RunLoop::Timer>> heartbeat;
    heartbeat.get() = makeUnique<RunLoop::Timer>(RunLoop::mainSingleton(), "EdenHangRecorderHeartbeat"_s, [] {
        hangHeartbeat.store(MonotonicTime::now().secondsSinceEpoch().value());
    });
    heartbeat.get()->startRepeating(interval);
    WTFLogAlways("Eden hang recorder: %s reports main thread stalls over %u ms", program_invocation_short_name, *milliseconds);

    Thread::create("EdenHangRecorder"_s, [mainThread, threshold, interval, signalNumber] {
        bool stalled = false;
        double stallStart = 0;
        while (true) {
            WTF::sleep(interval);
            double lastHeartbeat = hangHeartbeat.load();
            double now = MonotonicTime::now().secondsSinceEpoch().value();
            if (now - lastHeartbeat >= threshold.seconds()) {
                if (stalled)
                    continue;
                stalled = true;
                stallStart = lastHeartbeat;
                WTFLogAlways("Eden hang recorder: %s main thread has not run for %.0f ms, thread stacks follow", program_invocation_short_name, (now - lastHeartbeat) * 1000);
                printHangStacks(mainThread, signalNumber);
                continue;
            }
            if (stalled) {
                stalled = false;
                WTFLogAlways("Eden hang recorder: %s main thread resumed after %.0f ms", program_invocation_short_name, (lastHeartbeat - stallStart) * 1000);
            }
        }
    })->detach();
}
#endif

void AuxiliaryProcess::initialize(AuxiliaryProcessInitializationParameters&& parameters)
{
    TraceScope traceScope(ProcessInitializeStart, ProcessInitializeEnd);

    WTF::RefCountDebuggerBase::enableThreadingChecksGlobally();

#if PLATFORM(COCOA)
    // On Cocoa platforms, setAuxiliaryProcessType() is called in XPCServiceInitializer().
    ASSERT(processType() == parameters.processType);
#else
    setAuxiliaryProcessType(parameters.processType);
#endif

    RELEASE_ASSERT_WITH_MESSAGE(parameters.processIdentifier, "Unable to initialize child process without a WebCore process identifier");
    Process::setIdentifier(*parameters.processIdentifier);

    platformInitialize(parameters);

    SandboxInitializationParameters sandboxParameters;
    initializeSandbox(parameters, sandboxParameters);

    initializeProcess(parameters);

#if !LOG_DISABLED || !RELEASE_LOG_DISABLED
    WTF::logChannels().initializeLogChannelsIfNecessary();
    WebCore::logChannels().initializeLogChannelsIfNecessary();
    WebKit::logChannels().initializeLogChannelsIfNecessary();
#endif // !LOG_DISABLED || !RELEASE_LOG_DISABLED

    initializeProcessName(parameters);

#if OS(LINUX)
    startEdenHangRecorderIfRequested();
#endif

    // In WebKit2, only the UI process should ever be generating certain identifiers.
    PAL::SessionID::enableGenerationProtection();
    WebPageProxyIdentifier::enableGenerationProtection();

    Ref connection = IPC::Connection::createClientConnection(WTF::move(parameters.connectionIdentifier));
    lazyInitialize(m_connection, connection.copyRef());
    initializeConnection(connection.ptr());
    connection->open(*this);
}

void AuxiliaryProcess::setProcessSuppressionEnabled(bool enabled)
{
    if (enabled)
        m_processSuppressionDisabled.stop();
    else
        m_processSuppressionDisabled.start();
}

void AuxiliaryProcess::initializeProcess(const AuxiliaryProcessInitializationParameters&)
{
}

void AuxiliaryProcess::initializeProcessName(const AuxiliaryProcessInitializationParameters&)
{
}

void AuxiliaryProcess::initializeConnection(IPC::Connection* connection)
{
#if OS(LINUX) && USE(GLIB)
    // When bubblewrap sandbox is used isInsideFlatpak() also returns true in the web process.
    if (isInsideFlatpak())
        connection->sendCredentials();
#else
    UNUSED_PARAM(connection);
#endif
}

bool AuxiliaryProcess::dispatchMessage(IPC::Connection& connection, IPC::Decoder& decoder)
{
    if (m_messageReceiverMap.dispatchMessage(connection, decoder))
        return true;
    // Note: because WebProcess receives messages to non-existing IDs, we have to filter the messages there to avoid asserts.
    // Once these stop, this should be removed.
    return filterUnhandledMessage(connection, decoder);
}

bool AuxiliaryProcess::filterUnhandledMessage(IPC::Connection&, IPC::Decoder&)
{
    return false;
}

bool AuxiliaryProcess::dispatchSyncMessage(IPC::Connection& connection, IPC::Decoder& decoder, UniqueRef<IPC::Encoder>& replyEncoder)
{
    return m_messageReceiverMap.dispatchSyncMessage(connection, decoder, replyEncoder);
}

void AuxiliaryProcess::addMessageReceiver(IPC::ReceiverName messageReceiverName, IPC::MessageReceiver& messageReceiver)
{
    m_messageReceiverMap.addMessageReceiver(messageReceiverName, messageReceiver);
}

void AuxiliaryProcess::addMessageReceiver(IPC::ReceiverName messageReceiverName, uint64_t destinationID, IPC::MessageReceiver& messageReceiver)
{
    m_messageReceiverMap.addMessageReceiver(messageReceiverName, destinationID, messageReceiver);
}

void AuxiliaryProcess::removeMessageReceiver(IPC::ReceiverName messageReceiverName, uint64_t destinationID)
{
    m_messageReceiverMap.removeMessageReceiver(messageReceiverName, destinationID);
}

void AuxiliaryProcess::removeMessageReceiver(IPC::ReceiverName messageReceiverName)
{
    m_messageReceiverMap.removeMessageReceiver(messageReceiverName);
}

void AuxiliaryProcess::removeMessageReceiver(IPC::MessageReceiver& messageReceiver)
{
    m_messageReceiverMap.removeMessageReceiver(messageReceiver);
}

void AuxiliaryProcess::disableTermination()
{
    m_terminationCounter++;
}

void AuxiliaryProcess::enableTermination()
{
    ASSERT(m_terminationCounter > 0);
    m_terminationCounter--;

    if (m_terminationCounter || m_isInShutDown)
        return;

    if (shouldTerminate())
        terminate();
}

void AuxiliaryProcess::mainThreadPing(CompletionHandler<void()>&& completionHandler)
{
    completionHandler();
}

IPC::Connection* AuxiliaryProcess::messageSenderConnection() const
{
    return m_connection.get();
}

uint64_t AuxiliaryProcess::messageSenderDestinationID() const
{
    return 0;
}

void AuxiliaryProcess::stopRunLoop()
{
    platformStopRunLoop();
}

#if !PLATFORM(COCOA)
void AuxiliaryProcess::platformStopRunLoop()
{
    RunLoop::mainSingleton().stop();
}
#endif

void AuxiliaryProcess::terminate()
{
    protect(parentProcessConnection())->invalidate();

    stopRunLoop();
}

void AuxiliaryProcess::shutDown()
{
    SetForScope<bool> isInShutDown(m_isInShutDown, true);
    terminate();
}

void AuxiliaryProcess::applyProcessCreationParameters(AuxiliaryProcessCreationParameters&& parameters)
{
#if !LOG_DISABLED || !RELEASE_LOG_DISABLED
    WTF::logChannels().initializeLogChannelsIfNecessary(parameters.wtfLoggingChannels);
    WebCore::logChannels().initializeLogChannelsIfNecessary(parameters.webCoreLoggingChannels);
    WebKit::logChannels().initializeLogChannelsIfNecessary(parameters.webKitLoggingChannels);
#endif
#if PLATFORM(COCOA)
    SecureCoding::applyProcessCreationParameters(WTF::move(parameters));
#endif
#if ENABLE(CORE_IPC_SIGNPOSTS)
    if (parameters.shouldEnableIPCSignposts)
        IPC::Connection::forceEnableSignposts();
    if (parameters.shouldEnableStreamingIPCSignposts)
        IPC::StreamClientConnection::forceEnableSignposts();
#endif
}

void AuxiliaryProcess::grantAccessToContainerTempDirectory(const SandboxExtension::Handle& handle)
{
    SandboxExtension::consumePermanently(handle);
#if ENABLE(LLVM_PROFILE_GENERATION) && PLATFORM(IOS_FAMILY)
    WebKit::initializeLLVMProfiling();
    WebCore::initializeLLVMProfiling();
    JSC::initializeLLVMProfiling();
#endif // ENABLE(LLVM_PROFILE_GENERATION) && PLATFORM(IOS_FAMILY)
}

#if !PLATFORM(IOS_FAMILY) || PLATFORM(MACCATALYST)
void AuxiliaryProcess::populateMobileGestaltCache(std::optional<SandboxExtension::Handle>&&)
{
}
#endif

#if !PLATFORM(COCOA)

void AuxiliaryProcess::initializeSandbox(const AuxiliaryProcessInitializationParameters&, SandboxInitializationParameters&)
{
}

void AuxiliaryProcess::didReceiveInvalidMessage(IPC::Connection&, IPC::MessageName messageName, const Vector<uint32_t>&)
{
    WTFLogAlways("Received invalid message: '%s'", description(messageName).characters());
    CRASH();
}

#if OS(LINUX) && !OS(ANDROID)
void AuxiliaryProcess::didReceiveMemoryPressureEvent(bool isCritical)
{
    MemoryPressureHandler::singleton().triggerMemoryPressureEvent(isCritical);
}
#endif

#endif // !PLATFORM(COCOA)

} // namespace WebKit
