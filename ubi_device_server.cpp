/*
 * Copyright 2020 by Siklu Ltd. All rights reserved.
 */

#include "ubi_device_server.h"

#include "log.h"

static siklu::terragraph::ubi_device_server::UbiDeviceServerException
UbiDeviceServerException(const int& error_code) {
  auto ubi_device_server_exception =
      siklu::terragraph::ubi_device_server::UbiDeviceServerException();
  ubi_device_server_exception.set_error_code(error_code);
  return ubi_device_server_exception;
}

std::unique_ptr<apache::thrift::ThriftServer> UbiDeviceServer::CreateServer(
    const int& thrift_port,
    std::shared_ptr<IUbiDeviceFactory> ubi_device_factory) {
  auto handler = std::make_shared<UbiDeviceServer>();
  handler->ubi_device_factory_ = ubi_device_factory;
  auto proc_factory = std::make_shared<
      apache::thrift::ThriftServerAsyncProcessorFactory<UbiDeviceServer>>(
      handler);
  auto server = std::make_unique<apache::thrift::ThriftServer>();
  server->setPort(thrift_port);
  server->setProcessorFactory(proc_factory);
  return server;
}

void UbiDeviceServer::Init(std::unique_ptr<std::string> mtd_device_name,
                           bool is_to_format_first) {
  ubi_device_.reset();
  auto result = ubi_device_factory_->CreateUbiDevice(*mtd_device_name,
                                                     is_to_format_first);
  if (!result) {
    SKL_LOG(SKL_ERROR) << "ubi device failed to be created with error "
                       << int(result.error());
    throw UbiDeviceServerException(int(result.error()));
  } else {
    ubi_device_ = std::move(*result);
    SKL_LOG(SKL_INFO) << "ubi device successfully created";
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
    SKL_LOG(SKL_ERROR)
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
    SKL_LOG(SKL_ERROR)
        << "UnmountVolume() ubi device wasn't created by thrift server";
    throw UbiDeviceServerException(-1);
  }
}

void UbiDeviceServer::MakeVolume(std::unique_ptr<std::string> vol_name,
                                 int64_t size_in_bytes) {
  if (ubi_device_) {
    auto make_volume = ubi_device_->MakeVolume(*vol_name, size_in_bytes);
    if (!make_volume) throw UbiDeviceServerException(int(make_volume.error()));
  } else {
    SKL_LOG(SKL_ERROR)
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
    SKL_LOG(SKL_ERROR)
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
    SKL_LOG(SKL_ERROR)
        << "UpdateVolume() error ubi device wasn't created by thrift server";
    throw UbiDeviceServerException(-1);
  }
}
