/*
 * Copyright 2020 by Siklu Ltd. All rights reserved.
 */

#include "UbiDeviceServer.h"

#include <folly/logging/xlog.h>

#include "ThriftServiceFactory.h"

static siklu::terragraph::ubi_device_server::UbiDeviceServerException
UbiDeviceServerException(const int& error_code) {
  auto ubi_device_server_exception =
      siklu::terragraph::ubi_device_server::UbiDeviceServerException();
  ubi_device_server_exception.set_error_code(error_code);
  return ubi_device_server_exception;
}

folly::Expected<std::shared_ptr<apache::thrift::ThriftServer>, folly::Unit>
UbiDeviceServer::CreateServer(
    const uint16_t& thrift_port,
    std::shared_ptr<IUbiDeviceFactory> ubi_device_factory) {
  auto handler = std::make_shared<UbiDeviceServer>();
  handler->ubi_device_factory_ = ubi_device_factory;
  return ThriftServiceFactory::CreateServer<UbiDeviceServer>(thrift_port,
                                                             handler);
}

void UbiDeviceServer::Init(std::unique_ptr<std::string> mtd_device_name,
                           bool is_to_format_first) {
  ubi_device_.reset();
  auto result = ubi_device_factory_->CreateUbiDevice(*mtd_device_name,
                                                     is_to_format_first);
  if (!result) {
    XLOG(ERR) << "ubi device failed to be created with error "
              << int(result.error());
    throw UbiDeviceServerException(int(result.error()));
  } else {
    ubi_device_ = std::move(*result);
    XLOG(INFO) << "ubi device successfully created";
  }
}

void UbiDeviceServer::Destroy() {
  if (ubi_device_) ubi_device_.reset();
}

void UbiDeviceServer::MountVolume(std::unique_ptr<std::string> vol_name,
                                  std::unique_ptr<std::string> dir_to_mount) {
  if (ubi_device_) {
    auto mount_volume = ubi_device_->MountVolume(*vol_name, *dir_to_mount);
    if (!mount_volume)
      throw UbiDeviceServerException(int(mount_volume.error()));
  } else {
    XLOG(ERR)
        << "MountVolume() error ubi device wasn't created by thrift server";
    throw UbiDeviceServerException(-1);
  }
}

void UbiDeviceServer::UnmountVolume(
    std::unique_ptr<std::string> dir_to_unmount) {
  if (ubi_device_) {
    auto unmount_volume = ubi_device_->UnmountVolume(*dir_to_unmount);
    if (!unmount_volume)
      throw UbiDeviceServerException(int(unmount_volume.error()));
  } else {
    XLOG(ERR) << "UnmountVolume() ubi device wasn't created by thrift server";
    throw UbiDeviceServerException(-1);
  }
}

void UbiDeviceServer::MakeVolume(std::unique_ptr<std::string> vol_name,
                                 int64_t size_in_bytes) {
  if (ubi_device_) {
    auto make_volume = ubi_device_->MakeVolume(*vol_name, size_in_bytes);
    if (!make_volume) throw UbiDeviceServerException(int(make_volume.error()));
  } else {
    XLOG(ERR)
        << "MakeVolume() error ubi device wasn't created by thrift server";
    throw UbiDeviceServerException(-1);
  }
}

void UbiDeviceServer::RemoveVolume(std::unique_ptr<std::string> vol_name,
                                   bool is_to_print_log_error) {
  if (ubi_device_) {
    auto remove_volume =
        ubi_device_->RemoveVolume(*vol_name, is_to_print_log_error);
    if (!remove_volume)
      throw UbiDeviceServerException(int(remove_volume.error()));
  } else {
    XLOG(ERR)
        << "RemoveVolume() error ubi device wasn't created by thrift server";
    throw UbiDeviceServerException(-1);
  }
}

void UbiDeviceServer::UpdateVolume(
    std::unique_ptr<std::string> vol_name,
    std::unique_ptr<std::string> ubifs_image_file_str, int64_t skip_bytes,
    int64_t size) {
  if (ubi_device_) {
    auto update_volume = ubi_device_->UpdateVolume(
        *vol_name, *ubifs_image_file_str, skip_bytes, size);
    if (!update_volume)
      throw UbiDeviceServerException(int(update_volume.error()));
  } else {
    XLOG(ERR)
        << "UpdateVolume() error ubi device wasn't created by thrift server";
    throw UbiDeviceServerException(-1);
  }
}

void UbiDeviceServer::Format() {
  if (ubi_device_) {
    auto format_volume = ubi_device_->Format();
    if (!format_volume)
      throw UbiDeviceServerException(int(format_volume.error()));
  } else {
    XLOG(ERR) << "Format() error ubi device wasn't created by thrift server";
    throw UbiDeviceServerException(-1);
  }
}

void UbiDeviceServer::Attach() {
  if (ubi_device_) {
    auto attach_volume = ubi_device_->Attach();
    if (!attach_volume)
      throw UbiDeviceServerException(int(attach_volume.error()));
  } else {
    XLOG(ERR) << "Attach() error ubi device wasn't created by thrift server";
    throw UbiDeviceServerException(-1);
  }
}

void UbiDeviceServer::Detach() {
  if (ubi_device_) {
    auto detach_volume = ubi_device_->Detach();
    if (!detach_volume)
      throw UbiDeviceServerException(int(detach_volume.error()));
  } else {
    XLOG(ERR) << "Detach() error ubi device wasn't created by thrift server";
    throw UbiDeviceServerException(-1);
  }
}