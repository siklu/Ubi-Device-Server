/*
 * Copyright 2020 by Siklu Ltd. All rights reserved.
 */

#include <folly/init/Init.h>
#include <folly/logging/xlog.h>
#include <thrift/lib/cpp2/server/ThriftServer.h>

#include <csignal>

#include "ServerSignalHandler.h"
#include "UbiDeviceFactory.h"
#include "UbiDeviceServer.h"

static constexpr int kThriftPort = 12999;

int main(int argc, char* argv[]) {
  FLAGS_logtostderr = 1;
  google::InstallFailureSignalHandler();
  folly::init(&argc, &argv);

  auto ubi_device_factory = std::make_shared<UbiDeviceFactory>();

  auto maybe_server =
      UbiDeviceServer::CreateServer(kThriftPort, ubi_device_factory);
  if (!maybe_server) {
    XLOG(CRITICAL) << "failed to create server";
    return -1;
  }

  ServerSignalHandler signal_handler(*maybe_server);
  try {
    signal_handler.registerSignalHandler(SIGTERM);
  } catch (...) {
    XLOG(CRITICAL) << "failed to register signal SIGTERM";
    return -1;
  }
  XLOG(INFO) << "server: starts";
  maybe_server->get()->serve();

  return 0;
}