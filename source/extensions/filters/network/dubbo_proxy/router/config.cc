#include "source/extensions/filters/network/dubbo_proxy/router/config.h"

#include "envoy/extensions/filters/network/dubbo_proxy/router/v3/router.pb.h"
#include "envoy/extensions/filters/network/dubbo_proxy/router/v3/router.pb.validate.h"
#include "envoy/registry/registry.h"

#include "source/extensions/filters/network/dubbo_proxy/router/router_impl.h"

namespace Envoy {
namespace Extensions {
namespace NetworkFilters {
namespace DubboProxy {
namespace Router {
// 启动时系统会调用该接口，由Factory构建出我们的Filter。
DubboFilters::FilterFactoryCb RouterFilterConfig::createFilterFactoryFromProtoTyped(
    const envoy::extensions::filters::network::dubbo_proxy::router::v3::Router&, const std::string&,
    Server::Configuration::FactoryContext& context) {
  return [&context](DubboFilters::FilterChainFactoryCallbacks& callbacks) -> void {
    callbacks.addFilter(std::make_shared<Router>(context.clusterManager()));
  };
}

/**
 * Static registration for the router filter. @see RegisterFactory.
 * 将factory注册到envoy的上下文环境中
 */
REGISTER_FACTORY(RouterFilterConfig, DubboFilters::NamedDubboFilterConfigFactory);

} // namespace Router
} // namespace DubboProxy
} // namespace NetworkFilters
} // namespace Extensions
} // namespace Envoy
