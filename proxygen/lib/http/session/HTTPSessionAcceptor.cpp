/*
 *  Copyright (c) 2015-present, Facebook, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  LICENSE file in the root directory of this source tree. An additional grant
 *  of patent rights can be found in the PATENTS file in the same directory.
 *
 */
#include <proxygen/lib/http/session/HTTPSessionAcceptor.h>
#include <proxygen/lib/http/codec/HTTP1xCodec.h>
#include <proxygen/lib/http/codec/HTTP2Codec.h>
#include <proxygen/lib/http/session/HTTPDefaultSessionCodecFactory.h>
#include <proxygen/lib/http/session/HTTPDirectResponseHandler.h>

using folly::AsyncSocket;
using folly::SocketAddress;
using std::list;
using std::string;
using std::unique_ptr;

namespace proxygen {

const SocketAddress HTTPSessionAcceptor::unknownSocketAddress_("0.0.0.0", 0);

HTTPSessionAcceptor::HTTPSessionAcceptor(const AcceptorConfiguration& accConfig)
    : HTTPSessionAcceptor(accConfig, nullptr) {}

HTTPSessionAcceptor::HTTPSessionAcceptor(
  const AcceptorConfiguration& accConfig,
  std::shared_ptr<HTTPCodecFactory> codecFactory):
    HTTPAcceptor(accConfig),
    codecFactory_(codecFactory),
    simpleController_(this) {
  if (!codecFactory_) {
    codecFactory_ =
        std::make_shared<HTTPDefaultSessionCodecFactory>(accConfig_);
  }
}

HTTPSessionAcceptor::~HTTPSessionAcceptor() {
}

const HTTPErrorPage* HTTPSessionAcceptor::getErrorPage(
    const SocketAddress& addr) const {
  const HTTPErrorPage* errorPage = nullptr;
  if (isInternal()) {
    if (addr.isPrivateAddress()) {
      errorPage = diagnosticErrorPage_.get();
    }
  }
  if (errorPage == nullptr) {
    errorPage = defaultErrorPage_.get();
  }
  return errorPage;
}

void HTTPSessionAcceptor::onNewConnection(
    folly::AsyncTransportWrapper::UniquePtr sock,
    const SocketAddress* peerAddress,
    const string& nextProtocol,
    wangle::SecureTransportType,
    const wangle::TransportInfo& tinfo) {

  unique_ptr<HTTPCodec> codec
      = codecFactory_->getCodec(
          nextProtocol,
          TransportDirection::DOWNSTREAM,
          // we assume if security protocol isn't empty, then it's TLS
          !sock->getSecurityProtocol().empty());

  if (!codec) {
    VLOG(2) << "codecFactory_ failed to provide codec";
    onSessionCreationError(ProxygenError::kErrorUnsupportedScheme);
    return;
  }

  auto controller = getController();
  SocketAddress localAddress;
  try {
    sock->getLocalAddress(&localAddress);
  } catch (...) {
    VLOG(3) << "couldn't get local address for socket";
    localAddress = unknownSocketAddress_;
  }

  // overwrite address if the socket has no IP, e.g. Unix domain socket
  if (!localAddress.isFamilyInet()) {
    if (accConfig_.bindAddress.isFamilyInet()) {
      localAddress = accConfig_.bindAddress;
    } else {
      localAddress = unknownSocketAddress_;
    }
    VLOG(4) << "set localAddress=" << localAddress.describe();
  }

  auto sessionInfoCb = sessionInfoCb_ ? sessionInfoCb_ : this;
  VLOG(4) << "Created new " << nextProtocol
          << " session for peer " << *peerAddress;
  HTTPDownstreamSession* session =
    new HTTPDownstreamSession(getTransactionTimeoutSet(), std::move(sock),
                              localAddress, *peerAddress,
                              controller, std::move(codec), tinfo,
                              sessionInfoCb);
  if (accConfig_.maxConcurrentIncomingStreams) {
    session->setMaxConcurrentIncomingStreams(
        accConfig_.maxConcurrentIncomingStreams);
  }
  session->setEgressSettings(accConfig_.egressSettings);

  // set HTTP2 priorities flag on session object
  auto HTTP2PrioritiesEnabled = getHttp2PrioritiesEnabled();
  session->setHTTP2PrioritiesEnabled(HTTP2PrioritiesEnabled);

  // set flow control parameters
  session->setFlowControl(accConfig_.initialReceiveWindow,
                          accConfig_.receiveStreamWindowSize,
                          accConfig_.receiveSessionWindowSize);
  if (accConfig_.writeBufferLimit > 0) {
    session->setWriteBufferLimit(accConfig_.writeBufferLimit);
  }
  session->setSessionStats(downstreamSessionStats_);
  Acceptor::addConnection(session);
  session->startNow();
}

size_t HTTPSessionAcceptor::dropIdleConnections(size_t num) {
  // release in batch for more efficiency
  VLOG(4) << "attempt to reelease resource";
  return downstreamConnectionManager_->dropIdleConnections(num);
}

} // proxygen
