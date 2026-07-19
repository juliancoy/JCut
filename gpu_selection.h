#pragma once

#include <QDir>
#include <QFile>
#include <QString>
#include <QStringList>
#include <QVulkanWindow>

namespace editor::gpu {

inline QString preference()
{
    const QString value = qEnvironmentVariable("JCUT_GPU_PREFERENCE", QStringLiteral("auto"))
                              .trimmed().toLower();
    return value.startsWith(QStringLiteral("pci:")) ? value : QStringLiteral("auto");
}

inline QString deviceKey(quint32 vendorId, quint32 deviceId)
{
    return QStringLiteral("pci:%1:%2")
        .arg(vendorId, 4, 16, QLatin1Char('0'))
        .arg(deviceId, 4, 16, QLatin1Char('0'));
}

inline quint32 requestedVendorId()
{
    const QStringList parts = preference().split(QLatin1Char(':'));
    bool ok = false;
    const quint32 vendor = parts.size() == 3 ? parts[1].toUInt(&ok, 16) : 0;
    return ok ? vendor : 0x8086; // Automatic deliberately prefers Intel Iris.
}

inline int chooseVulkanDevice(const QVector<VkPhysicalDeviceProperties>& devices)
{
    const QString requested = preference();
    if (requested != QStringLiteral("auto")) {
        for (int i = 0; i < devices.size(); ++i) {
            if (deviceKey(devices[i].vendorID, devices[i].deviceID) == requested) return i;
        }
    }
    for (int i = 0; i < devices.size(); ++i) {
        if (devices[i].vendorID == 0x8086) return i;
    }
    for (int i = 0; i < devices.size(); ++i) {
        if (devices[i].deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU) return i;
    }
    return devices.isEmpty() ? -1 : 0;
}

inline QStringList linuxVaapiRenderNodes()
{
    struct Node { QString path; quint32 vendor = 0; };
    QVector<Node> nodes;
    const QStringList entries = QDir(QStringLiteral("/sys/class/drm"))
                                    .entryList({QStringLiteral("renderD*")}, QDir::Dirs);
    for (const QString& entry : entries) {
        QFile vendorFile(QStringLiteral("/sys/class/drm/%1/device/vendor").arg(entry));
        quint32 vendor = 0;
        if (vendorFile.open(QIODevice::ReadOnly)) {
            bool ok = false;
            vendor = QString::fromLatin1(vendorFile.readAll()).trimmed().toUInt(&ok, 0);
            if (!ok) vendor = 0;
        }
        nodes.push_back({QStringLiteral("/dev/dri/%1").arg(entry), vendor});
    }
    const quint32 preferredVendor = requestedVendorId();
    QStringList result;
    for (const Node& node : nodes) if (node.vendor == preferredVendor) result.append(node.path);
    for (const Node& node : nodes) if (!result.contains(node.path)) result.append(node.path);
    return result;
}

} // namespace editor::gpu
