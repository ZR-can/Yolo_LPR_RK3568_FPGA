#ifndef TRAFFIC_PCIE_BRIDGE_H_
#define TRAFFIC_PCIE_BRIDGE_H_

#include "pcie_demo_bridge.h"
#include "traffic_violation.h"

int RunTrafficPcieQtDemo(const char* model_path,
                         const TrafficRoiConfig& roi,
                         const TrafficLightRoiConfig& light_roi,
                         const PcieUiCallbacks* callbacks);

#endif  // TRAFFIC_PCIE_BRIDGE_H_
