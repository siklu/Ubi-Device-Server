/*
 * Copyright 2020 by Siklu Ltd. All rights reserved.
 */

#include <folly/init/Init.h>
#include <thrift/lib/cpp2/server/ThriftServer.h>

#include "log.h"
#include "ubi_device_factory.h"
#include "ubi_device_server.h"

static constexpr int kThriftPort = 12999;

int main(int argc, char* argv[]) {
  FLAGS_logtostderr = 1;
  google::InstallFailureSignalHandler();
  LogStream::CreateLoggers();
  folly::init(&argc, &argv);

  auto ubi_device_factory = std::make_shared<UbiDeviceFactory>();
  auto server = UbiDeviceServer::CreateServer(kThriftPort, ubi_device_factory);
  SKL_LOG(SKL_INFO) << "server: starts";
  server->serve();

  return 0;
}