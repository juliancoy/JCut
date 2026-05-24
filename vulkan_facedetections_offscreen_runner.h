#pragma once

#include <QStringList>

// Runs the FaceStream offscreen generator logic in-process.
// Args should match the CLI arguments normally passed to jcut_vulkan_facestream_offscreen,
// excluding argv[0].
int runVulkanFacestreamOffscreen(const QStringList& args);
