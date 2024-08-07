#include "source/extensions/filters/http/proto_message_logging/filter_config.h"

#include <memory>
#include <utility>

#include "envoy/api/api.h"

#include "source/common/common/logger.h"
#include "source/common/grpc/common.h"
#include "source/extensions/filters/http/proto_message_logging/extractor.h"

#include "absl/log/log.h"
#include "absl/strings/string_view.h"
#include "grpc_transcoding/type_helper.h"

namespace Envoy {
namespace Extensions {
namespace HttpFilters {
namespace ProtoMessageLogging {
namespace {

using ::envoy::extensions::filters::http::proto_message_logging::v3::ProtoMessageLoggingConfig;
using ::google::grpc::transcoding::TypeHelper;
} // namespace

FilterConfig::FilterConfig(const ProtoMessageLoggingConfig& proto_config,
                           std::unique_ptr<ExtractorFactory> extractor_factory, Api::Api& api)
    : proto_config_(proto_config) {
  initDescriptorPool(api);

  type_helper_ =
      std::make_unique<const TypeHelper>(Envoy::Protobuf::util::NewTypeResolverForDescriptorPool(
          Envoy::Grpc::Common::typeUrlPrefix(), descriptor_pool_.get()));

  type_finder_ = std::make_unique<const TypeFinder>(
      [this](absl::string_view type_url) -> const ::Envoy::ProtobufWkt::Type* {
        return type_helper_->Info()->GetTypeByTypeUrl(type_url);
      });

  initExtractors(*extractor_factory);
}

const Extractor* FilterConfig::findExtractor(absl::string_view proto_path) const {
  if (!proto_path_to_extractor_.contains(proto_path)) {
    return nullptr;
  }
  return proto_path_to_extractor_.find(proto_path)->second.get();
}

void FilterConfig::initExtractors(ExtractorFactory& extractor_factory) {
  for (const auto& it : proto_config_.logging_by_method()) {
    auto* method = descriptor_pool_->FindMethodByName(it.first);

    if (method == nullptr) {
      ENVOY_LOG_MISC(debug, "couldn't find the gRPC method `{}` defined in the proto descriptor",
                     it.first);
      return;
    }

    auto extractor = extractor_factory.createExtractor(
        *type_helper_, *type_finder_,
        Envoy::Grpc::Common::typeUrlPrefix() + "/" + method->input_type()->full_name(),
        Envoy::Grpc::Common::typeUrlPrefix() + "/" + method->output_type()->full_name(), it.second);
    if (!extractor.ok()) {
      ENVOY_LOG_MISC(debug, "couldn't init extractor for method `{}`: {}", it.first,
                     extractor.status().message());
      return;
    }

    ENVOY_LOG_MISC(debug, "registered field extraction for gRPC method `{}`", it.first);
    proto_path_to_extractor_.emplace(it.first, std::move(extractor.value()));
  }
}

void FilterConfig::initDescriptorPool(Api::Api& api) {
  Envoy::Protobuf::FileDescriptorSet descriptor_set;
  const ::envoy::config::core::v3::DataSource& descriptor_config = proto_config_.data_source();

  auto pool = std::make_unique<Envoy::Protobuf::DescriptorPool>();

  switch (descriptor_config.specifier_case()) {
  case envoy::config::core::v3::DataSource::SpecifierCase::kFilename: {
    auto file_or_error = api.fileSystem().fileReadToEnd(descriptor_config.filename());
    if (!file_or_error.status().ok() || !descriptor_set.ParseFromString(file_or_error.value())) {
      ENVOY_LOG_MISC(debug, "unable to parse proto descriptor from file `{}`",
                     descriptor_config.filename());
      return;
    }
    break;
  }
  case envoy::config::core::v3::DataSource::SpecifierCase::kInlineBytes: {
    if (!descriptor_set.ParseFromString(descriptor_config.inline_bytes())) {
      ENVOY_LOG_MISC(debug, "unable to parse proto descriptor from inline bytes: {}",
                     descriptor_config.inline_bytes());
      return;
    }
    break;
  }
  default: {
    ENVOY_LOG_MISC(debug, "unsupported DataSource case `{}` for configuring `descriptor_set`",
                   descriptor_config.specifier_case());
    return;
  }
  }

  for (const auto& file : descriptor_set.file()) {
    pool->BuildFile(file);
  }
  descriptor_pool_ = std::move(pool);
}

} // namespace ProtoMessageLogging
} // namespace HttpFilters
} // namespace Extensions
} // namespace Envoy
