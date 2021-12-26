/*
 * Copyright 2020 by Siklu Ltd. All rights reserved.
 */

#pragma once

#include "IUbiDeviceFactory.h"
#include "UbiDevice.h"

class UbiDeviceFactory : public IUbiDeviceFactory {
 public:
  static std::shared_ptr<UbiDeviceFactory> Create();
  folly::Expected<std::shared_ptr<IUbiDevice>, int32_t> CreateUbiDevice(
      const std::string& mtd_device_name,
      bool is_to_format_first = false) override;
};
