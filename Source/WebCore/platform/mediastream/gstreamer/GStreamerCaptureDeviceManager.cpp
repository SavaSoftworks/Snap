/*
 * Copyright (C) 2018 Metrological Group B.V.
 * Author: Thibault Saunier <tsaunier@igalia.com>
 * Author: Alejandro G. Castro <alex@igalia.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public License
 * aint with this library; see the file COPYING.LIB.  If not, write to
 * the Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 */

#include "config.h"

#if ENABLE(MEDIA_STREAM) && USE(GSTREAMER)
#include "GStreamerCaptureDeviceManager.h"

#include "GStreamerCommon.h"
#include "GStreamerMockDeviceProvider.h"
#include "MockRealtimeMediaSourceCenter.h"
#include <wtf/Condition.h>
#include <wtf/Lock.h>
#include <wtf/MainThread.h>
#include <wtf/NeverDestroyed.h>
#include <wtf/ThreadSafeRefCounted.h>
#include <wtf/WorkQueue.h>
#include <wtf/glib/GMallocString.h>
#include <wtf/glib/GSpanExtras.h>
#include <wtf/glib/GUniquePtr.h>
#include <wtf/text/MakeString.h>

namespace WebCore {

GST_DEBUG_CATEGORY(webkitGStreamerCaptureDeviceManagerDebugCategory);
#define GST_CAT_DEFAULT webkitGStreamerCaptureDeviceManagerDebugCategory

static bool audioCaptureSingletonInitialized = false;
static bool videoCaptureSingletonInitialized = false;

static gint sortDevices(gconstpointer a, gconstpointer b)
{
    GstDevice* adev = GST_DEVICE(a), *bdev = GST_DEVICE(b);
    GUniquePtr<GstStructure> aprops(gst_device_get_properties(adev));
    GUniquePtr<GstStructure> bprops(gst_device_get_properties(bdev));
    auto aIsDefault = gstStructureGet<bool>(aprops.get(), "is-default"_s).value_or(false);
    auto bIsDefault = gstStructureGet<bool>(bprops.get(), "is-default"_s).value_or(false);

    if (aIsDefault == bIsDefault) {
        auto aName = GMallocString::unsafeAdoptFromUTF8(gst_device_get_display_name(adev));
        auto bName = GMallocString::unsafeAdoptFromUTF8(gst_device_get_display_name(bdev));

        auto result = compareSpans(aName.span(), bName.span());
        return is_eq(result) ? 0 : (is_lt(result) ? -1 : 1);
    }

    return aIsDefault > bIsDefault ? -1 : 1;
}

struct StartedGStreamerDeviceMonitor {
    GRefPtr<GstDeviceMonitor> monitor;
    Vector<GRefPtr<GstDevice>> devices;
};

class GStreamerDeviceMonitorStartup final : public ThreadSafeRefCounted<GStreamerDeviceMonitorStartup> {
public:
    static Ref<GStreamerDeviceMonitorStartup> create() { return adoptRef(*new GStreamerDeviceMonitorStartup); }

    void finish(StartedGStreamerDeviceMonitor&& started)
    {
        Locker locker { m_lock };
        m_started = WTF::move(started);
        m_finished = true;
        m_condition.notifyAll();
    }

    std::optional<StartedGStreamerDeviceMonitor> take(bool waitForStartup)
    {
        Locker locker { m_lock };
        while (waitForStartup && !m_finished)
            m_condition.wait(m_lock);
        return std::exchange(m_started, std::nullopt);
    }

private:
    GStreamerDeviceMonitorStartup() = default;

    Lock m_lock;
    Condition m_condition;
    bool m_finished WTF_GUARDED_BY_LOCK(m_lock) { false };
    std::optional<StartedGStreamerDeviceMonitor> m_started WTF_GUARDED_BY_LOCK(m_lock);
};

static WorkQueue& deviceMonitorQueue()
{
    static NeverDestroyed<Ref<WorkQueue>> queue(WorkQueue::create("GStreamer device monitor"_s));
    return queue.get();
}

static StartedGStreamerDeviceMonitor startDeviceMonitorAndCollectDevices(OptionSet<CaptureDevice::DeviceType> types)
{
    StartedGStreamerDeviceMonitor started;
    GRefPtr monitor = adoptGRef(gst_device_monitor_new());
    if (types.contains(CaptureDevice::DeviceType::Camera))
        gst_device_monitor_add_filter(monitor.get(), "Video/Source", nullptr);
    if (types.contains(CaptureDevice::DeviceType::Microphone)) {
        GRefPtr caps = adoptGRef(gst_caps_new_empty_simple("audio/x-raw"));
        gst_device_monitor_add_filter(monitor.get(), "Audio/Source", caps.get());
    }
    if (types.contains(CaptureDevice::DeviceType::Speaker) || types.contains(CaptureDevice::DeviceType::SystemAudio)) {
        GRefPtr caps = adoptGRef(gst_caps_new_empty_simple("audio/x-raw"));
        gst_device_monitor_add_filter(monitor.get(), "Audio/Sink", caps.get());
    }

    if (!gst_device_monitor_start(monitor.get())) {
        GST_WARNING_OBJECT(monitor.get(), "Could not start device monitor");
        return started;
    }

    GRefPtr bus = adoptGRef(gst_device_monitor_get_bus(monitor.get()));
#if GST_CHECK_VERSION(1, 28, 0)
    static constexpr GstClockTime maximumProviderStartup = 5 * GST_SECOND;
    if (GRefPtr message = adoptGRef(gst_bus_timed_pop_filtered(bus.get(), maximumProviderStartup, GST_MESSAGE_DEVICE_MONITOR_STARTED)))
        GST_DEBUG_OBJECT(monitor.get(), "Device provider(s) successfully started");
    else
        GST_WARNING_OBJECT(monitor.get(), "Device provider(s) did not report startup in time, using the devices found so far");
#endif

    GList* devices = g_list_sort(gst_device_monitor_get_devices(monitor.get()), sortDevices);
    while (devices) {
        started.devices.append(adoptGRef(GST_DEVICE_CAST(devices->data)));
        devices = g_list_delete_link(devices, devices);
    }

    gst_bus_set_flushing(bus.get(), TRUE);
    gst_bus_set_flushing(bus.get(), FALSE);

    started.monitor = WTF::move(monitor);
    return started;
}

GStreamerAudioCaptureDeviceManager& GStreamerAudioCaptureDeviceManager::singleton()
{
    static NeverDestroyed<GStreamerAudioCaptureDeviceManager> manager;
    audioCaptureSingletonInitialized = true;
    return manager;
}

GStreamerVideoCaptureDeviceManager& GStreamerVideoCaptureDeviceManager::singleton()
{
    static NeverDestroyed<GStreamerVideoCaptureDeviceManager> manager;
    videoCaptureSingletonInitialized = true;
    return manager;
}

void teardownGStreamerCaptureDeviceManagers()
{
    if (audioCaptureSingletonInitialized) {
        auto& audioManager = GStreamerAudioCaptureDeviceManager::singleton();
        audioManager.teardown();
    }

    if (videoCaptureSingletonInitialized) {
        auto& videoManager = GStreamerVideoCaptureDeviceManager::singleton();
        videoManager.teardown();
    }
}

GStreamerCaptureDeviceManager::GStreamerCaptureDeviceManager()
{
    ensureGStreamerInitialized();

    static std::once_flag onceFlag;
    std::call_once(onceFlag, [] {
        GST_DEBUG_CATEGORY_INIT(webkitGStreamerCaptureDeviceManagerDebugCategory, "webkitcapturedevicemanager", 0, "WebKit Capture Device Manager");
        gst_device_provider_register(nullptr, "mock-device-provider", GST_RANK_PRIMARY, GST_TYPE_MOCK_DEVICE_PROVIDER);
    });

    RealtimeMediaSourceCenter::singleton().addDevicesChangedObserver(*this);
}

GStreamerCaptureDeviceManager::~GStreamerCaptureDeviceManager()
{
    teardown();
}

void GStreamerCaptureDeviceManager::teardown()
{
    GST_DEBUG_OBJECT(m_deviceMonitor.get(), "Tearing down");
    m_isTearingDown = true;
    stopMonitor();
    for (auto& capturer : m_capturers)
        capturer->stopDevice(true);
    m_capturers.clear();
    RealtimeMediaSourceCenter::singleton().removeDevicesChangedObserver(*this);
    m_devices.clear();
    m_gstreamerDevices.clear();
}

void GStreamerCaptureDeviceManager::stopMonitor()
{
    m_monitorStartup = nullptr;
    if (!m_deviceMonitor)
        return;

    GRefPtr bus = adoptGRef(gst_device_monitor_get_bus(m_deviceMonitor.get()));
    gst_bus_remove_watch(bus.get());
    gst_device_monitor_stop(m_deviceMonitor.get());
    m_deviceMonitor.clear();
}

void GStreamerCaptureDeviceManager::devicesChanged()
{
    GST_INFO_OBJECT(m_deviceMonitor.get(), "RealtimeMediaSourceCenter notified devices list update, clearing our internal cache");
    stopMonitor();
    m_gstreamerDevices.clear();
    m_devices.clear();
    m_speakerDevices.clear();
}

void GStreamerCaptureDeviceManager::deviceWillBeRemoved(const String& persistentId)
{
    stopCapturing(persistentId);
}

void GStreamerCaptureDeviceManager::registerCapturer(RefPtr<GStreamerCapturer>&& capturer)
{
    GST_DEBUG("Registering capturer for device %s", capturer->devicePersistentId().ascii().data());
    m_capturers.append(WTF::move(capturer));
}

void GStreamerCaptureDeviceManager::unregisterCapturer(const GStreamerCapturer& capturer)
{
    GST_DEBUG("Un-registering capturer for device %s", capturer.devicePersistentId().ascii().data());
    m_capturers.removeAllMatching([&](auto& item) -> bool {
        return item.get() == &capturer;
    });
}

void GStreamerCaptureDeviceManager::stopCapturing(const String& persistentId)
{
    GST_DEBUG("Stopping capturer for device with persistent ID: %s", persistentId.ascii().data());
    for (auto& capturer : m_capturers) {
        GST_DEBUG("Checking capturer with device persistent ID: %s", capturer->devicePersistentId().ascii().data());
        if (capturer->devicePersistentId() != persistentId)
            continue;
        capturer->stopDevice(false);
        break;
    }
}

std::optional<GStreamerCaptureDevice> GStreamerCaptureDeviceManager::gstreamerDeviceWithUID(const String& deviceID)
{
    captureDevices();

    GST_DEBUG("Looking for device with UID %s", deviceID.ascii().data());
    for (auto& device : m_gstreamerDevices) {
        GST_LOG("Checking device with persistent ID: %s", device.persistentId().ascii().data());
        if (device.persistentId() == deviceID)
            return device;
    }
    GST_WARNING("Device not found");
    return std::nullopt;
}

const Vector<CaptureDevice>& GStreamerCaptureDeviceManager::captureDevices()
{
    if (m_devices.isEmpty() && !m_isTearingDown)
        refreshCaptureDevices();

    return m_devices;
}

const Vector<CaptureDevice>& GStreamerCaptureDeviceManager::speakerDevices()
{
    if (m_speakerDevices.isEmpty() && !m_isTearingDown)
        refreshCaptureDevices();

    return m_speakerDevices;
}

std::optional<GStreamerCaptureDevice> GStreamerCaptureDeviceManager::captureDeviceFromGstDevice(GRefPtr<GstDevice>&& device)
{
    GUniquePtr<GstStructure> properties(gst_device_get_properties(device.get()));
    auto deviceClassFromProperties = gstStructureGetString(properties.get(), "device.class"_s);
    if (deviceClassFromProperties == "monitor"_s)
        return { };

    auto types = deviceTypes();
    auto deviceClass = GMallocString::unsafeAdoptFromUTF8(gst_device_get_device_class(device.get()));
    CaptureDevice::DeviceType type;
    if (startsWith(deviceClass.span(), "Audio"_s)) {
        if (types.contains(CaptureDevice::DeviceType::Microphone) && endsWith(deviceClass.span(), "Source"_s))
            type = CaptureDevice::DeviceType::Microphone;
        else if (types.contains(CaptureDevice::DeviceType::Speaker) && endsWith(deviceClass.span(), "Sink"_s))
            type = CaptureDevice::DeviceType::Speaker;
        else
            return { };
    } else if (types.contains(CaptureDevice::DeviceType::Camera) && startsWith(deviceClass.span(), "Video"_s))
        type = CaptureDevice::DeviceType::Camera;
    else
        return { };

    // This isn't really a UID but should be good enough (libwebrtc
    // itself does that at least for pulseaudio devices).
    auto deviceName = GMallocString::unsafeAdoptFromUTF8(gst_device_get_display_name(device.get()));
    auto isDefault = gstStructureGet<bool>(properties.get(), "is-default"_s).value_or(false);
    auto label = makeString(isDefault ? "default: "_s : ""_s, deviceName.span());

    auto nodeName = gstStructureGetString(properties.get(), "node.name"_s);
    String identifier;
    if (nodeName.isEmpty())
        identifier = makeString(deviceName.span(), ", "_s, deviceClass.span(), ", "_s, deviceClass.span());
    else
        identifier = makeString(nodeName, ", "_s, deviceClass.span(), ", "_s, deviceClass.span());

    bool isMock = false;
    if (auto persistentId = gstStructureGetString(properties.get(), "persistent-id"_s)) {
        identifier = persistentId.span();
        isMock = true;
    }

    auto gstCaptureDevice = GStreamerCaptureDevice(WTF::move(device), identifier, type, deviceName.span());
    gstCaptureDevice.setEnabled(true);
    gstCaptureDevice.setIsDefault(isDefault);
    gstCaptureDevice.setIsMockDevice(isMock);
    return gstCaptureDevice;
}

void GStreamerCaptureDeviceManager::addDevice(GRefPtr<GstDevice>&& device)
{
    auto gstCaptureDevice = captureDeviceFromGstDevice(WTF::move(device));
    if (!gstCaptureDevice)
        return;

    GST_INFO_OBJECT(gstCaptureDevice->device(), "Registering %sdefault device %s", gstCaptureDevice->isDefault() ? "" : "non-", gstCaptureDevice->label().utf8().data());
    const auto type = gstCaptureDevice->type();
    m_gstreamerDevices.append(WTF::move(*gstCaptureDevice));
    if (type == CaptureDevice::DeviceType::Speaker)
        m_speakerDevices.append(m_gstreamerDevices.last());
    else
        m_devices.append(m_gstreamerDevices.last());
}

void GStreamerCaptureDeviceManager::removeDevice(GRefPtr<GstDevice>&& gstDevice)
{
    m_gstreamerDevices.removeFirstMatching([&](auto& captureDevice) -> bool {
        if (captureDevice.device() != gstDevice.get())
            return false;

        m_devices.removeFirstMatching([&](auto& device) -> bool {
            return device.persistentId() == captureDevice.persistentId();
        });
        m_speakerDevices.removeFirstMatching([&](auto& device) -> bool {
            return device.persistentId() == captureDevice.persistentId();
        });
        return true;
    });
}

void GStreamerCaptureDeviceManager::updateDevice(GRefPtr<GstDevice>&& gstDevice, GRefPtr<GstDevice>&& oldGstDevice)
{
    GUniquePtr<GstStructure> oldProperties(gst_device_get_properties(oldGstDevice.get()));
    GST_DEBUG_OBJECT(m_deviceMonitor.get(), "Old props: %" GST_PTR_FORMAT, oldProperties.get());
    auto wasDefault = gstStructureGet<bool>(oldProperties.get(), "is-default"_s).value_or(false);
    if (wasDefault) {
        auto oldGstCaptureDevice = captureDeviceFromGstDevice(GRefPtr(oldGstDevice));
        if (!oldGstCaptureDevice)
            return;

        for (auto& capturer : m_capturers) {
            if (capturer->devicePersistentId() == oldGstCaptureDevice->persistentId()) {
                m_defaultCapturer = capturer;
                break;
            }
        }
    }

    GUniquePtr<GstStructure> properties(gst_device_get_properties(gstDevice.get()));
    GST_DEBUG_OBJECT(m_deviceMonitor.get(), "New props: %" GST_PTR_FORMAT, properties.get());
    auto isDefault = gstStructureGet<bool>(properties.get(), "is-default"_s).value_or(false);
    if (isDefault && m_defaultCapturer) {
        auto gstCaptureDevice = captureDeviceFromGstDevice(GRefPtr(gstDevice));
        if (!gstCaptureDevice)
            return;

        m_defaultCapturer->setDevice(WTF::move(gstCaptureDevice));
        m_defaultCapturer = nullptr;
    }

    removeDevice(WTF::move(oldGstDevice));
    addDevice(WTF::move(gstDevice));
}

void GStreamerCaptureDeviceManager::computeCaptureDevices(CompletionHandler<void()>&& callback)
{
    if (m_deviceMonitor || m_isTearingDown || MockRealtimeMediaSourceCenter::mockRealtimeMediaSourceCenterEnabled()) {
        callback();
        return;
    }

    m_pendingComputeCallbacks.append(WTF::move(callback));
    if (!m_monitorStartup)
        startMonitorAsynchronously();
}

void GStreamerCaptureDeviceManager::startMonitorAsynchronously()
{
    Ref startup = GStreamerDeviceMonitorStartup::create();
    m_monitorStartup = startup.copyRef();
    deviceMonitorQueue().dispatch([this, startup = WTF::move(startup), types = deviceTypes()]() mutable {
        startup->finish(startDeviceMonitorAndCollectDevices(types));
        callOnMainThread([this, startup = WTF::move(startup)]() mutable {
            finishMonitorStartup(startup.get());
        });
    });
}

void GStreamerCaptureDeviceManager::finishMonitorStartup(GStreamerDeviceMonitorStartup& startup)
{
    auto started = startup.take(false);
    if (&startup == m_monitorStartup.get()) {
        m_monitorStartup = nullptr;
        if (started)
            adoptDeviceMonitor(WTF::move(*started));
    } else {
        if (started && started->monitor)
            gst_device_monitor_stop(started->monitor.get());
        if (m_monitorStartup)
            return;
        if (!m_deviceMonitor && !m_isTearingDown && !m_pendingComputeCallbacks.isEmpty()) {
            startMonitorAsynchronously();
            return;
        }
    }

    for (auto& callback : std::exchange(m_pendingComputeCallbacks, {}))
        callback();
}

void GStreamerCaptureDeviceManager::refreshCaptureDevices()
{
    GST_DEBUG_OBJECT(m_deviceMonitor.get(), "Refreshing capture devices");
    m_devices.clear();
    m_gstreamerDevices.clear();
    m_speakerDevices.clear();
    if (m_isTearingDown)
        return;

    if (!m_deviceMonitor) {
        std::optional<StartedGStreamerDeviceMonitor> started;
        if (RefPtr startup = std::exchange(m_monitorStartup, nullptr))
            started = startup->take(true);
        else
            started = startDeviceMonitorAndCollectDevices(deviceTypes());
        if (started)
            adoptDeviceMonitor(WTF::move(*started));
        return;
    }

    GList* devices = g_list_sort(gst_device_monitor_get_devices(m_deviceMonitor.get()), sortDevices);
    while (devices) {
        addDevice(adoptGRef(GST_DEVICE_CAST(devices->data)));
        devices = g_list_delete_link(devices, devices);
    }
}

void GStreamerCaptureDeviceManager::adoptDeviceMonitor(StartedGStreamerDeviceMonitor&& started)
{
    if (!started.monitor)
        return;

    if (m_isTearingDown) {
        gst_device_monitor_stop(started.monitor.get());
        return;
    }

    m_devices.clear();
    m_gstreamerDevices.clear();
    m_speakerDevices.clear();
    m_deviceMonitor = WTF::move(started.monitor);
    for (auto& device : started.devices)
        addDevice(WTF::move(device));

    GRefPtr bus = adoptGRef(gst_device_monitor_get_bus(m_deviceMonitor.get()));

    // Monitor the bus for future device messages.
    gst_bus_add_watch(bus.get(), reinterpret_cast<GstBusFunc>(+[](GstBus*, GstMessage* message, GStreamerCaptureDeviceManager* manager) -> gboolean {
        GRefPtr<GstDevice> device;
#ifndef GST_DISABLE_GST_DEBUG
        GMallocString name;
#endif
        switch (GST_MESSAGE_TYPE(message)) {
        case GST_MESSAGE_DEVICE_ADDED:
            gst_message_parse_device_added(message, &device.outPtr());
#ifndef GST_DISABLE_GST_DEBUG
            name = GMallocString::unsafeAdoptFromUTF8(gst_device_get_display_name(device.get()));
            GST_INFO_OBJECT(GST_MESSAGE_SRC(message), "Device added: %s", name.utf8());
#endif
            manager->addDevice(WTF::move(device));
            manager->deviceChanged();
            break;
        case GST_MESSAGE_DEVICE_REMOVED:
            gst_message_parse_device_removed(message, &device.outPtr());
#ifndef GST_DISABLE_GST_DEBUG
            name = GMallocString::unsafeAdoptFromUTF8(gst_device_get_display_name(device.get()));
            GST_INFO_OBJECT(GST_MESSAGE_SRC(message), "Device removed: %s", name.utf8());
#endif
            manager->removeDevice(WTF::move(device));
            manager->deviceChanged();
            break;
        case GST_MESSAGE_DEVICE_CHANGED: {
            GRefPtr<GstDevice> oldDevice;

            gst_message_parse_device_changed(message, &device.outPtr(), &oldDevice.outPtr());
#ifndef GST_DISABLE_GST_DEBUG
            name = GMallocString::unsafeAdoptFromUTF8(gst_device_get_display_name(device.get()));
            GST_INFO_OBJECT(GST_MESSAGE_SRC(message), "Device changed: %s", name.utf8());
#endif
            manager->updateDevice(WTF::move(device), WTF::move(oldDevice));
            break;
        }
        default:
            break;
        }
        return G_SOURCE_CONTINUE;
    }), this);
}

#undef GST_CAT_DEFAULT

} // namespace WebCore

#endif // ENABLE(MEDIA_STREAM) && USE(GSTREAMER)
