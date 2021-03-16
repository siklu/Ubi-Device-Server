/*
 * Copyright 2020 by Siklu Ltd. All rights reserved.
 */

#ifndef UBI_DEVICE_FACTORY_H
#define UBI_DEVICE_FACTORY_H

#include "iubi_device_factory.h"
#include "ubi_device.h"

class UbiDeviceFactory : public IUbiDeviceFactory {
 public:
  static std::shared_ptr<UbiDeviceFactory> Create();
  folly::Expected<std::shared_ptr<IUbiDevice>, int32_t> CreateUbiDevice(
      const std::string& mtd_device_name,
      bool is_to_format_first = false) override;
};

#endif  // UBI_DEVICE_FACTORY_H