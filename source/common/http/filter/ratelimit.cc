#include "ratelimit.h"

#include "envoy/http/codes.h"

#include "common/common/assert.h"
#include "common/common/empty_string.h"
#include "common/common/enum_to_int.h"
#include "common/http/codes.h"
#include "common/json/config_schemas.h"
#include "common/router/config_impl.h"

namespace Http {
namespace RateLimit {

FilterConfig::FilterConfig(const Json::Object& config, const LocalInfo::LocalInfo& local_info,
                           Stats::Store& global_store, Runtime::Loader& runtime,
                           Upstream::ClusterManager& cm)
    : domain_(config.getString("domain")), stage_(config.getInteger("stage", 0)),
      local_info_(local_info), global_store_(global_store), runtime_(runtime), cm_(cm) {
  config.validateSchema(Json::Schema::RATE_LIMIT_HTTP_FILTER_SCHEMA);
}

const Http::HeaderMapPtr Filter::TOO_MANY_REQUESTS_HEADER{new Http::HeaderMapImpl{
    {Http::Headers::get().Status, std::to_string(enumToInt(Code::TooManyRequests))}}};

FilterHeadersStatus Filter::decodeHeaders(HeaderMap& headers, bool) {
  if (!config_->runtime().snapshot().featureEnabled("ratelimit.http_filter_enabled", 100)) {
    return FilterHeadersStatus::Continue;
  }

  const Router::Route* route = callbacks_->route();
  if (route && route->routeEntry()) {
    const Router::RouteEntry* route_entry = route->routeEntry();

    // TODO: Cluster may not exist.
    cluster_ = config_->cm().get(route_entry->clusterName());

    std::vector<::RateLimit::Descriptor> descriptors;

    // Get all applicable rate limit policy entries for the route.
    populateRateLimitDescriptors(route_entry->rateLimitPolicy(), descriptors, route_entry, headers);

    // Get all applicable rate limit policy entries for the virtual host.
    populateRateLimitDescriptors(route_entry->virtualHost().rateLimitPolicy(), descriptors,
                                 route_entry, headers);

    if (!descriptors.empty()) {
      state_ = State::Calling;
      initiating_call_ = true;
      client_->limit(*this, config_->domain(), descriptors,
                     headers.RequestId() ? headers.RequestId()->value().c_str() : EMPTY_STRING);
      initiating_call_ = false;
    }
  }

  return (state_ == State::Calling || state_ == State::Responded)
             ? FilterHeadersStatus::StopIteration
             : FilterHeadersStatus::Continue;
}

FilterDataStatus Filter::decodeData(Buffer::Instance&, bool) {
  ASSERT(state_ != State::Responded);
  return state_ == State::Calling ? FilterDataStatus::StopIterationAndBuffer
                                  : FilterDataStatus::Continue;
}

FilterTrailersStatus Filter::decodeTrailers(HeaderMap&) {
  ASSERT(state_ != State::Responded);
  return state_ == State::Calling ? FilterTrailersStatus::StopIteration
                                  : FilterTrailersStatus::Continue;
}

void Filter::setDecoderFilterCallbacks(StreamDecoderFilterCallbacks& callbacks) {
  callbacks_ = &callbacks;
  callbacks.addResetStreamCallback([this]() -> void {
    if (state_ == State::Calling) {
      client_->cancel();
    }
  });
}

void Filter::complete(::RateLimit::LimitStatus status) {
  state_ = State::Complete;

  switch (status) {
  case ::RateLimit::LimitStatus::OK:
    cluster_->statsScope().counter("ratelimit.ok").inc();
    break;
  case ::RateLimit::LimitStatus::Error:
    cluster_->statsScope().counter("ratelimit.error").inc();
    break;
  case ::RateLimit::LimitStatus::OverLimit:
    cluster_->statsScope().counter("ratelimit.over_limit").inc();
    Http::CodeUtility::ResponseStatInfo info{
        config_->globalStore(), cluster_->statsScope(), EMPTY_STRING, *TOO_MANY_REQUESTS_HEADER,
        true, EMPTY_STRING, EMPTY_STRING, EMPTY_STRING, EMPTY_STRING, false};
    Http::CodeUtility::chargeResponseStat(info);
    break;
  }

  if (status == ::RateLimit::LimitStatus::OverLimit &&
      config_->runtime().snapshot().featureEnabled("ratelimit.http_filter_enforcing", 100)) {
    state_ = State::Responded;
    Http::HeaderMapPtr response_headers{new HeaderMapImpl(*TOO_MANY_REQUESTS_HEADER)};
    callbacks_->encodeHeaders(std::move(response_headers), true);
    callbacks_->requestInfo().setResponseFlag(Http::AccessLog::ResponseFlag::RateLimited);
  } else if (!initiating_call_) {
    callbacks_->continueDecoding();
  }
}

void Filter::populateRateLimitDescriptors(const Router::RateLimitPolicy& rate_limit_policy,
                                          std::vector<::RateLimit::Descriptor>& descriptors,
                                          const Router::RouteEntry* route_entry,
                                          const HeaderMap& headers) const {
  for (const Router::RateLimitPolicyEntry& rate_limit :
       rate_limit_policy.getApplicableRateLimit(config_->stage())) {
    const std::string& route_key = rate_limit.routeKey();
    if (!route_key.empty() &&
        !config_->runtime().snapshot().featureEnabled(
            fmt::format("ratelimit.{}.http_filter_enabled", route_key), 100)) {
      continue;
    }
    rate_limit.populateDescriptors(*route_entry, descriptors, config_->localInfo().clusterName(),
                                   headers, callbacks_->downstreamAddress());
  }
}

} // RateLimit
} // Http
