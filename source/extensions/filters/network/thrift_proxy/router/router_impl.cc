#include "source/extensions/filters/network/thrift_proxy/router/router_impl.h"

#include <memory>

#include "envoy/extensions/filters/network/thrift_proxy/v3/route.pb.h"
#include "envoy/upstream/cluster_manager.h"
#include "envoy/upstream/thread_local_cluster.h"

#include "source/common/common/utility.h"
#include "source/common/router/metadatamatchcriteria_impl.h"
#include "source/extensions/filters/network/thrift_proxy/app_exception_impl.h"
#include "source/extensions/filters/network/well_known_names.h"

#include "absl/strings/match.h"

namespace Envoy {
namespace Extensions {
namespace NetworkFilters {
namespace ThriftProxy {
namespace Router {

RouteEntryImplBase::RouteEntryImplBase(
    const envoy::extensions::filters::network::thrift_proxy::v3::Route& route)
    : cluster_name_(route.route().cluster()),
      config_headers_(Http::HeaderUtility::buildHeaderDataVector(route.match().headers())),
      rate_limit_policy_(route.route().rate_limits()),
      strip_service_name_(route.route().strip_service_name()),
      cluster_header_(route.route().cluster_header()) {
  if (route.route().has_metadata_match()) {
    const auto filter_it = route.route().metadata_match().filter_metadata().find(
        Envoy::Config::MetadataFilters::get().ENVOY_LB);
    if (filter_it != route.route().metadata_match().filter_metadata().end()) {
      metadata_match_criteria_ =
          std::make_unique<Envoy::Router::MetadataMatchCriteriaImpl>(filter_it->second);
    }
  }

  if (route.route().cluster_specifier_case() ==
      envoy::extensions::filters::network::thrift_proxy::v3::RouteAction::ClusterSpecifierCase::
          kWeightedClusters) {

    total_cluster_weight_ = 0UL;
    for (const auto& cluster : route.route().weighted_clusters().clusters()) {
      std::unique_ptr<WeightedClusterEntry> cluster_entry(new WeightedClusterEntry(*this, cluster));
      weighted_clusters_.emplace_back(std::move(cluster_entry));
      total_cluster_weight_ += weighted_clusters_.back()->clusterWeight();
    }
  }
}

const std::string& RouteEntryImplBase::clusterName() const { return cluster_name_; }

const RouteEntry* RouteEntryImplBase::routeEntry() const { return this; }

RouteConstSharedPtr RouteEntryImplBase::clusterEntry(uint64_t random_value,
                                                     const MessageMetadata& metadata) const {
  if (!weighted_clusters_.empty()) {
    return WeightedClusterUtil::pickCluster(weighted_clusters_, total_cluster_weight_, random_value,
                                            false);
  }

  const auto& cluster_header = clusterHeader();
  if (!cluster_header.get().empty()) {
    const auto& headers = metadata.headers();
    const auto entry = headers.get(cluster_header);
    if (!entry.empty()) {
      // This is an implicitly untrusted header, so per the API documentation only the first
      // value is used.
      return std::make_shared<DynamicRouteEntry>(*this, entry[0]->value().getStringView());
    }

    return nullptr;
  }

  return shared_from_this();
}

bool RouteEntryImplBase::headersMatch(const Http::HeaderMap& headers) const {
  return Http::HeaderUtility::matchHeaders(headers, config_headers_);
}

RouteEntryImplBase::WeightedClusterEntry::WeightedClusterEntry(
    const RouteEntryImplBase& parent,
    const envoy::extensions::filters::network::thrift_proxy::v3::WeightedCluster::ClusterWeight&
        cluster)
    : parent_(parent), cluster_name_(cluster.name()),
      cluster_weight_(PROTOBUF_GET_WRAPPED_REQUIRED(cluster, weight)) {
  if (cluster.has_metadata_match()) {
    const auto filter_it = cluster.metadata_match().filter_metadata().find(
        Envoy::Config::MetadataFilters::get().ENVOY_LB);
    if (filter_it != cluster.metadata_match().filter_metadata().end()) {
      if (parent.metadata_match_criteria_) {
        metadata_match_criteria_ =
            parent.metadata_match_criteria_->mergeMatchCriteria(filter_it->second);
      } else {
        metadata_match_criteria_ =
            std::make_unique<Envoy::Router::MetadataMatchCriteriaImpl>(filter_it->second);
      }
    }
  }
}

MethodNameRouteEntryImpl::MethodNameRouteEntryImpl(
    const envoy::extensions::filters::network::thrift_proxy::v3::Route& route)
    : RouteEntryImplBase(route), method_name_(route.match().method_name()),
      invert_(route.match().invert()) {
  if (method_name_.empty() && invert_) {
    throw EnvoyException("Cannot have an empty method name with inversion enabled");
  }
}

RouteConstSharedPtr MethodNameRouteEntryImpl::matches(const MessageMetadata& metadata,
                                                      uint64_t random_value) const {
  if (RouteEntryImplBase::headersMatch(metadata.headers())) {
    bool matches =
        method_name_.empty() || (metadata.hasMethodName() && metadata.methodName() == method_name_);

    if (matches ^ invert_) {
      return clusterEntry(random_value, metadata);
    }
  }

  return nullptr;
}

ServiceNameRouteEntryImpl::ServiceNameRouteEntryImpl(
    const envoy::extensions::filters::network::thrift_proxy::v3::Route& route)
    : RouteEntryImplBase(route), invert_(route.match().invert()) {
  const std::string service_name = route.match().service_name();
  if (service_name.empty() && invert_) {
    throw EnvoyException("Cannot have an empty service name with inversion enabled");
  }

  if (!service_name.empty() && !absl::EndsWith(service_name, ":")) {
    service_name_ = service_name + ":";
  } else {
    service_name_ = service_name;
  }
}

RouteConstSharedPtr ServiceNameRouteEntryImpl::matches(const MessageMetadata& metadata,
                                                       uint64_t random_value) const {
  if (RouteEntryImplBase::headersMatch(metadata.headers())) {
    bool matches =
        service_name_.empty() ||
        (metadata.hasMethodName() && absl::StartsWith(metadata.methodName(), service_name_));

    if (matches ^ invert_) {
      return clusterEntry(random_value, metadata);
    }
  }

  return nullptr;
}

RouteMatcher::RouteMatcher(
    const envoy::extensions::filters::network::thrift_proxy::v3::RouteConfiguration& config) {
  using envoy::extensions::filters::network::thrift_proxy::v3::RouteMatch;

  for (const auto& route : config.routes()) {
    switch (route.match().match_specifier_case()) {
    case RouteMatch::MatchSpecifierCase::kMethodName:
      routes_.emplace_back(new MethodNameRouteEntryImpl(route));
      break;
    case RouteMatch::MatchSpecifierCase::kServiceName:
      routes_.emplace_back(new ServiceNameRouteEntryImpl(route));
      break;
    default:
      NOT_REACHED_GCOVR_EXCL_LINE;
    }
  }
}
// 进行路由
RouteConstSharedPtr RouteMatcher::route(const MessageMetadata& metadata,
                                        uint64_t random_value) const {
  for (const auto& route : routes_) {
    // 循环调用，查看是否符合，例如常见的常用的有PrefixRouteEntryImpl::matches，看前缀是否一致
    RouteConstSharedPtr route_entry = route->matches(metadata, random_value);
    if (nullptr != route_entry) {
      return route_entry;
    }
  }

  return nullptr;
}

void Router::onDestroy() {
  if (upstream_request_ != nullptr) {
    upstream_request_->resetStream();
    cleanup();
  }
}

void Router::setDecoderFilterCallbacks(ThriftFilters::DecoderFilterCallbacks& callbacks) {
  callbacks_ = &callbacks;

  // TODO(zuercher): handle buffer limits
}

FilterStatus Router::transportBegin(MessageMetadataSharedPtr metadata) {
  UNREFERENCED_PARAMETER(metadata);
  return FilterStatus::Continue;
}

FilterStatus Router::transportEnd() {
  if (upstream_request_->metadata_->messageType() == MessageType::Oneway) {
    // No response expected
    upstream_request_->onResponseComplete();
    cleanup();
  }
  return FilterStatus::Continue;
}

FilterStatus Router::messageBegin(MessageMetadataSharedPtr metadata) {
  route_ = callbacks_->route();
  if (!route_) {
    ENVOY_STREAM_LOG(debug, "no route match for method '{}'", *callbacks_, metadata->methodName());
    stats_.route_missing_.inc();
    callbacks_->sendLocalReply(
        AppException(AppExceptionType::UnknownMethod,
                     fmt::format("no route for method '{}'", metadata->methodName())),
        true);
    return FilterStatus::StopIteration;
  }

  route_entry_ = route_->routeEntry();
  const std::string& cluster_name = route_entry_->clusterName();

  Upstream::ThreadLocalCluster* cluster = cluster_manager_.getThreadLocalCluster(cluster_name);
  if (!cluster) {
    ENVOY_STREAM_LOG(debug, "unknown cluster '{}'", *callbacks_, cluster_name);
    stats_.unknown_cluster_.inc();
    callbacks_->sendLocalReply(AppException(AppExceptionType::InternalError,
                                            fmt::format("unknown cluster '{}'", cluster_name)),
                               true);
    return FilterStatus::StopIteration;
  }

  cluster_ = cluster->info();
  ENVOY_STREAM_LOG(debug, "cluster '{}' match for method '{}'", *callbacks_, cluster_name,
                   metadata->methodName());
  switch (metadata->messageType()) {
  case MessageType::Call:
    incClusterScopeCounter({upstream_rq_call_});
    break;

  case MessageType::Oneway:
    incClusterScopeCounter({upstream_rq_oneway_});
    break;

  default:
    incClusterScopeCounter({upstream_rq_invalid_type_});
    break;
  }

  if (cluster_->maintenanceMode()) {
    stats_.upstream_rq_maintenance_mode_.inc();
    callbacks_->sendLocalReply(
        AppException(AppExceptionType::InternalError,
                     fmt::format("maintenance mode for cluster '{}'", cluster_name)),
        true);
    return FilterStatus::StopIteration;
  }

  const std::shared_ptr<const ProtocolOptionsConfig> options =
      cluster_->extensionProtocolOptionsTyped<ProtocolOptionsConfig>(
          NetworkFilterNames::get().ThriftProxy);

  const TransportType transport = options
                                      ? options->transport(callbacks_->downstreamTransportType())
                                      : callbacks_->downstreamTransportType();
  ASSERT(transport != TransportType::Auto);

  const ProtocolType protocol = options ? options->protocol(callbacks_->downstreamProtocolType())
                                        : callbacks_->downstreamProtocolType();
  ASSERT(protocol != ProtocolType::Auto);

  if (callbacks_->downstreamTransportType() == TransportType::Framed &&
      transport == TransportType::Framed && callbacks_->downstreamProtocolType() == protocol &&
      protocol != ProtocolType::Twitter) {
    passthrough_supported_ = true;
  }

  auto conn_pool_data = cluster->tcpConnPool(Upstream::ResourcePriority::Default, this);
  if (!conn_pool_data) {
    stats_.no_healthy_upstream_.inc();
    callbacks_->sendLocalReply(
        AppException(AppExceptionType::InternalError,
                     fmt::format("no healthy upstream for '{}'", cluster_name)),
        true);
    return FilterStatus::StopIteration;
  }

  ENVOY_STREAM_LOG(debug, "router decoding request", *callbacks_);

  if (route_entry_->stripServiceName()) {
    const auto& method = metadata->methodName();
    const auto pos = method.find(':');
    if (pos != std::string::npos) {
      metadata->setMethodName(method.substr(pos + 1));
    }
  }

  upstream_request_ =
      std::make_unique<UpstreamRequest>(*this, *conn_pool_data, metadata, transport, protocol);
  return upstream_request_->start();
}

FilterStatus Router::messageEnd() {
  ProtocolConverter::messageEnd();

  Buffer::OwnedImpl transport_buffer;

  upstream_request_->metadata_->setProtocol(upstream_request_->protocol_->type());

  upstream_request_->transport_->encodeFrame(transport_buffer, *upstream_request_->metadata_,
                                             upstream_request_buffer_);

  request_size_ += transport_buffer.length();
  recordClusterScopeHistogram({upstream_rq_size_}, Stats::Histogram::Unit::Bytes, request_size_);

  upstream_request_->conn_data_->connection().write(transport_buffer, false);
  upstream_request_->onRequestComplete();
  return FilterStatus::Continue;
}

void Router::onUpstreamData(Buffer::Instance& data, bool end_stream) {
  ASSERT(!upstream_request_->response_complete_);

  response_size_ += data.length();

  if (upstream_request_->upgrade_response_ != nullptr) {
    ENVOY_STREAM_LOG(trace, "reading upgrade response: {} bytes", *callbacks_, data.length());
    // Handle upgrade response.
    if (!upstream_request_->upgrade_response_->onData(data)) {
      // Wait for more data.
      return;
    }

    ENVOY_STREAM_LOG(debug, "upgrade response complete", *callbacks_);
    upstream_request_->protocol_->completeUpgrade(*upstream_request_->conn_state_,
                                                  *upstream_request_->upgrade_response_);

    upstream_request_->upgrade_response_.reset();
    upstream_request_->onRequestStart(true);
  } else {
    ENVOY_STREAM_LOG(trace, "reading response: {} bytes", *callbacks_, data.length());

    // Handle normal response.
    if (!upstream_request_->response_started_) {
      callbacks_->startUpstreamResponse(*upstream_request_->transport_,
                                        *upstream_request_->protocol_);
      upstream_request_->response_started_ = true;
    }

    ThriftFilters::ResponseStatus status = callbacks_->upstreamData(data);
    if (status == ThriftFilters::ResponseStatus::Complete) {
      ENVOY_STREAM_LOG(debug, "response complete", *callbacks_);
      recordClusterScopeHistogram({upstream_resp_size_}, Stats::Histogram::Unit::Bytes,
                                  response_size_);

      switch (callbacks_->responseMetadata()->messageType()) {
      case MessageType::Reply:
        incClusterScopeCounter({upstream_resp_reply_});
        if (callbacks_->responseSuccess()) {
          upstream_request_->upstream_host_->outlierDetector().putResult(
              Upstream::Outlier::Result::ExtOriginRequestSuccess);
          incClusterScopeCounter({upstream_resp_reply_success_});
        } else {
          upstream_request_->upstream_host_->outlierDetector().putResult(
              Upstream::Outlier::Result::ExtOriginRequestFailed);
          incClusterScopeCounter({upstream_resp_reply_error_});
        }
        break;

      case MessageType::Exception:
        upstream_request_->upstream_host_->outlierDetector().putResult(
            Upstream::Outlier::Result::ExtOriginRequestFailed);
        incClusterScopeCounter({upstream_resp_exception_});
        break;

      default:
        incClusterScopeCounter({upstream_resp_invalid_type_});
        break;
      }
      upstream_request_->onResponseComplete();
      cleanup();
      return;
    } else if (status == ThriftFilters::ResponseStatus::Reset) {
      // Note: invalid responses are not accounted in the response size histogram.
      ENVOY_STREAM_LOG(debug, "upstream reset", *callbacks_);
      upstream_request_->upstream_host_->outlierDetector().putResult(
          Upstream::Outlier::Result::ExtOriginRequestFailed);
      upstream_request_->resetStream();
      return;
    }
  }

  if (end_stream) {
    // Response is incomplete, but no more data is coming.
    ENVOY_STREAM_LOG(debug, "response underflow", *callbacks_);
    upstream_request_->onResponseComplete();
    upstream_request_->onResetStream(ConnectionPool::PoolFailureReason::RemoteConnectionFailure);
    cleanup();
  }
}

void Router::onEvent(Network::ConnectionEvent event) {
  ASSERT(upstream_request_ && !upstream_request_->response_complete_);

  switch (event) {
  case Network::ConnectionEvent::RemoteClose:
    ENVOY_STREAM_LOG(debug, "upstream remote close", *callbacks_);
    upstream_request_->onResetStream(ConnectionPool::PoolFailureReason::RemoteConnectionFailure);
    break;
  case Network::ConnectionEvent::LocalClose:
    ENVOY_STREAM_LOG(debug, "upstream local close", *callbacks_);
    upstream_request_->onResetStream(ConnectionPool::PoolFailureReason::LocalConnectionFailure);
    break;
  default:
    // Connected is consumed by the connection pool.
    NOT_REACHED_GCOVR_EXCL_LINE;
  }

  upstream_request_->releaseConnection(false);
}

const Network::Connection* Router::downstreamConnection() const {
  if (callbacks_ != nullptr) {
    return callbacks_->connection();
  }

  return nullptr;
}

void Router::convertMessageBegin(MessageMetadataSharedPtr metadata) {
  ProtocolConverter::messageBegin(metadata);
}

void Router::cleanup() { upstream_request_.reset(); }

Router::UpstreamRequest::UpstreamRequest(Router& parent, Upstream::TcpPoolData& pool_data,
                                         MessageMetadataSharedPtr& metadata,
                                         TransportType transport_type, ProtocolType protocol_type)
    : parent_(parent), conn_pool_data_(pool_data), metadata_(metadata),
      transport_(NamedTransportConfigFactory::getFactory(transport_type).createTransport()),
      protocol_(NamedProtocolConfigFactory::getFactory(protocol_type).createProtocol()),
      request_complete_(false), response_started_(false), response_complete_(false) {}

Router::UpstreamRequest::~UpstreamRequest() {
  if (conn_pool_handle_) {
    conn_pool_handle_->cancel(Tcp::ConnectionPool::CancelPolicy::Default);
  }
}

FilterStatus Router::UpstreamRequest::start() {
  Tcp::ConnectionPool::Cancellable* handle = conn_pool_data_.newConnection(*this);
  if (handle) {
    // Pause while we wait for a connection.
    conn_pool_handle_ = handle;
    return FilterStatus::StopIteration;
  }

  if (upgrade_response_ != nullptr) {
    // Pause while we wait for an upgrade response.
    return FilterStatus::StopIteration;
  }

  if (upstream_host_ == nullptr) {
    return FilterStatus::StopIteration;
  }

  return FilterStatus::Continue;
}

void Router::UpstreamRequest::releaseConnection(const bool close) {
  if (conn_pool_handle_) {
    conn_pool_handle_->cancel(Tcp::ConnectionPool::CancelPolicy::Default);
    conn_pool_handle_ = nullptr;
  }

  conn_state_ = nullptr;

  // The event triggered by close will also release this connection so clear conn_data_ before
  // closing.
  auto conn_data = std::move(conn_data_);
  if (close && conn_data != nullptr) {
    conn_data->connection().close(Network::ConnectionCloseType::NoFlush);
  }
}

void Router::UpstreamRequest::resetStream() { releaseConnection(true); }

void Router::UpstreamRequest::onPoolFailure(ConnectionPool::PoolFailureReason reason,
                                            absl::string_view,
                                            Upstream::HostDescriptionConstSharedPtr host) {
  conn_pool_handle_ = nullptr;

  // Mimic an upstream reset.
  onUpstreamHostSelected(host);
  onResetStream(reason);
}

void Router::UpstreamRequest::onPoolReady(Tcp::ConnectionPool::ConnectionDataPtr&& conn_data,
                                          Upstream::HostDescriptionConstSharedPtr host) {
  // Only invoke continueDecoding if we'd previously stopped the filter chain.
  bool continue_decoding = conn_pool_handle_ != nullptr;

  onUpstreamHostSelected(host);
  host->outlierDetector().putResult(Upstream::Outlier::Result::LocalOriginConnectSuccess);

  conn_data_ = std::move(conn_data);
  conn_data_->addUpstreamCallbacks(parent_);
  conn_pool_handle_ = nullptr;

  conn_state_ = conn_data_->connectionStateTyped<ThriftConnectionState>();
  if (conn_state_ == nullptr) {
    conn_data_->setConnectionState(std::make_unique<ThriftConnectionState>());
    conn_state_ = conn_data_->connectionStateTyped<ThriftConnectionState>();
  }

  if (protocol_->supportsUpgrade()) {
    upgrade_response_ =
        protocol_->attemptUpgrade(*transport_, *conn_state_, parent_.upstream_request_buffer_);
    if (upgrade_response_ != nullptr) {
      parent_.request_size_ += parent_.upstream_request_buffer_.length();
      conn_data_->connection().write(parent_.upstream_request_buffer_, false);
      return;
    }
  }

  onRequestStart(continue_decoding);
}

void Router::UpstreamRequest::onRequestStart(bool continue_decoding) {
  parent_.initProtocolConverter(*protocol_, parent_.upstream_request_buffer_);

  metadata_->setSequenceId(conn_state_->nextSequenceId());
  parent_.convertMessageBegin(metadata_);

  if (continue_decoding) {
    parent_.callbacks_->continueDecoding();
  }
}

void Router::UpstreamRequest::onRequestComplete() {
  Event::Dispatcher& dispatcher = parent_.callbacks_->dispatcher();
  downstream_request_complete_time_ = dispatcher.timeSource().monotonicTime();
  request_complete_ = true;
}

void Router::UpstreamRequest::onResponseComplete() {
  chargeResponseTiming();
  response_complete_ = true;
  conn_state_ = nullptr;
  conn_data_.reset();
}

void Router::UpstreamRequest::onUpstreamHostSelected(Upstream::HostDescriptionConstSharedPtr host) {
  upstream_host_ = host;
}

void Router::UpstreamRequest::onResetStream(ConnectionPool::PoolFailureReason reason) {
  if (metadata_->messageType() == MessageType::Oneway) {
    // For oneway requests, we should not attempt a response. Reset the downstream to signal
    // an error.
    parent_.callbacks_->resetDownstreamConnection();
    return;
  }

  chargeResponseTiming();

  switch (reason) {
  case ConnectionPool::PoolFailureReason::Overflow:
    parent_.callbacks_->sendLocalReply(
        AppException(AppExceptionType::InternalError,
                     "thrift upstream request: too many connections"),
        true);
    break;
  case ConnectionPool::PoolFailureReason::LocalConnectionFailure:
    upstream_host_->outlierDetector().putResult(
        Upstream::Outlier::Result::LocalOriginConnectFailed);
    // Should only happen if we closed the connection, due to an error condition, in which case
    // we've already handled any possible downstream response.
    parent_.callbacks_->resetDownstreamConnection();
    break;
  case ConnectionPool::PoolFailureReason::RemoteConnectionFailure:
  case ConnectionPool::PoolFailureReason::Timeout:
    if (reason == ConnectionPool::PoolFailureReason::Timeout) {
      upstream_host_->outlierDetector().putResult(Upstream::Outlier::Result::LocalOriginTimeout);
    } else if (reason == ConnectionPool::PoolFailureReason::RemoteConnectionFailure) {
      upstream_host_->outlierDetector().putResult(
          Upstream::Outlier::Result::LocalOriginConnectFailed);
    }

    // TODO(zuercher): distinguish between these cases where appropriate (particularly timeout)
    if (!response_started_) {
      parent_.callbacks_->sendLocalReply(
          AppException(
              AppExceptionType::InternalError,
              fmt::format("connection failure '{}'", (upstream_host_ != nullptr)
                                                         ? upstream_host_->address()->asString()
                                                         : "to upstream")),
          true);
      return;
    }

    // Error occurred after a partial response, propagate the reset to the downstream.
    parent_.callbacks_->resetDownstreamConnection();
    break;
  default:
    NOT_REACHED_GCOVR_EXCL_LINE;
  }
}

void Router::UpstreamRequest::chargeResponseTiming() {
  if (charged_response_timing_ || !request_complete_) {
    return;
  }
  charged_response_timing_ = true;
  Event::Dispatcher& dispatcher = parent_.callbacks_->dispatcher();
  const std::chrono::milliseconds response_time =
      std::chrono::duration_cast<std::chrono::milliseconds>(
          dispatcher.timeSource().monotonicTime() - downstream_request_complete_time_);
  const uint64_t count = response_time.count();
  parent_.recordClusterScopeHistogram({parent_.upstream_rq_time_},
                                      Stats::Histogram::Unit::Milliseconds, count);
}

} // namespace Router
} // namespace ThriftProxy
} // namespace NetworkFilters
} // namespace Extensions
} // namespace Envoy
