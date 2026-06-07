#pragma once

#include <QStringList>

// Runs the FaceDetections offscreen generator logic in-process.
// Args should match the CLI arguments normally passed to
// jcut_vulkan_facedetections_offscreen, excluding argv[0].
int runVulkanFacestreamOffscreen(const QStringList &args);

int runVulkanFacestreamOffscreenWithArgv(int argc, char **argv);
