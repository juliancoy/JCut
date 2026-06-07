#include "vulkan_facedetections_offscreen_runner.h"

#ifdef JCUT_FACESTREAM_OFFSCREEN_STANDALONE
int main(int argc, char **argv) {
  return runVulkanFacestreamOffscreenWithArgv(argc, argv);
}
#endif
