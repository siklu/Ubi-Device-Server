/*
 * Copyright 2021 by Siklu Ltd. All rights reserved.
 */
#pragma once
#include <folly/Expected.h>
#include <thrift/lib/cpp2/async/HeaderClientChannel.h>
#include <thrift/lib/cpp2/gen/client_h.h>
#include <thrift/lib/cpp2/server/ThriftServer.h>

#include "log.h"

class ThriftServiceFactory {
 public:
  /**
   * @brief Create a thrift server that will delegate its input to the handler
   *
   * @tparam T A thrift service which has the type of the handler.
   * @tparam std::enable_if_t<
   * is_base_of<apache::thrift::ServerInterface, T>>
   * @param port
   * @param handler a shared pointer to a thrift service.
   * IMPORTENT: Ownership of the handler must be passed to the server.
   * @return folly::Expected<std::shared_ptr<apache::thrift::ThriftServer>,
   * folly::Unit>
   */
  template <class T, typename = std::enable_if_t<
                         std::is_base_of_v<apache::thrift::ServerInterface, T>>>
  static folly::Expected<std::shared_ptr<apache::thrift::ThriftServer>,
                         folly::Unit>
  CreateServer(const uint16_t& port, std::shared_ptr<T> handler) {
    auto proc_factory =
        std::make_shared<apache::thrift::ThriftServerAsyncProcessorFactory<T>>(
            handler);
    auto server = std::make_unique<apache::thrift::ThriftServer>();
    server->setPort(port);
    server->setProcessorFactory(proc_factory);

    // this means how much time to wait for data before disconnecting the
    // connection - default is 1 minute.
    // 0 means forever - so that we don't need to create a new server after more
    // than 1 minute idle time
    server->setIdleTimeout(std::chrono::milliseconds(0));

    return std::move(server);
  }
};