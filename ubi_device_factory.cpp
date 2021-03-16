/*
 * Copyright 2020 by Siklu Ltd. All rights reserved.
 */

#include "ubi_device_factory.h"

folly::Expected<std::shared_ptr<IUbiDevice>, int32_t>
UbiDeviceFactory::CreateUbiDevice(const std::string& mtd_device_name,
                                  bool is_to_format_first) {
  return UbiDevice::Create(mtd_device_name, is_to_format_first);
}

std::shared_ptr<UbiDeviceFactory> UbiDeviceFactory::Create() {
  return std::make_shared<UbiDeviceFactory>();
}
