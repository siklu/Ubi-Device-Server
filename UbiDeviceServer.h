/*
 * Copyright 2020 by Siklu Ltd. All rights reserved.
 */

#pragma once

#include <thrift/lib/cpp2/server/ThriftServer.h>

#include "IUbiDeviceFactory.h"
#include "thrift/gen-cpp2/UbiDeviceServerService.h"
#include "thrift/gen-cpp2/UbiDeviceServer_data.h"
#include "thrift/gen-cpp2/UbiDeviceServer_types.h"

class UbiDeviceServer
    : public siklu::terragraph::ubi_device_server::UbiDeviceServerServiceSvIf {
 public:
  UbiDeviceServer() = default;

  static folly::Expected<std::shared_ptr<apache::thrift::ThriftServer>,
                         folly::Unit>
  CreateServer(const uint16_t &thrift_port,
               std::shared_ptr<IUbiDeviceFactory> ubi_device_factory);

  void Init(std::unique_ptr<std::string> mtd_device_name,
            bool is_to_format_first) override;

  void Destroy() override;

  void MountVolume(std::unique_ptr<std::string> vol_name,
                   std::unique_ptr<std::string> dir_to_mount) override;

  void UnmountVolume(std::unique_ptr<std::string> dir_to_unmount) override;

  void MakeVolume(std::unique_ptr<std::string> vol_name,
                  int64_t size_in_bytes) override;

  void RemoveVolume(std::unique_ptr<std::string> vol_name,
                    bool is_to_print_log_error) override;

  void UpdateVolume(std::unique_ptr<std::string> vol_name,
                    std::unique_ptr<std::string> ubifs_image_file_str,
                    int64_t skip_bytes, int64_t size) override;

  void Format() override;

  void Attach() override;

  void Detach() override;

 private:
  std::shared_ptr<IUbiDeviceFactory> ubi_device_factory_;
  std::shared_ptr<IUbiDevice> ubi_device_;
};
