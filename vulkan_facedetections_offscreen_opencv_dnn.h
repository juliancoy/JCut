#pragma once

#include "vulkan_facedetections_offscreen_detection_filters.h"

#include <QString>
#include <QVector>

#if JCUT_HAVE_OPENCV
#include <opencv2/dnn.hpp>
#include <opencv2/imgproc.hpp>

bool hasVulkanDnnTarget();
bool initializeTrainedVulkanDnn(cv::dnn::Net *net, QString *error);
QVector<Detection> detectTrainedVulkanDnn(cv::dnn::Net *net, const cv::Mat &bgr,
                                          float threshold, double *inferenceMs,
                                          QString *error);
#endif
