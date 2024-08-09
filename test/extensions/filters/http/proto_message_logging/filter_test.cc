#include <cstdint>
#include <memory>
#include <string>

#include "source/extensions/filters/http/proto_message_logging/extractor_impl.h"
#include "source/extensions/filters/http/proto_message_logging/filter.h"
#include "source/extensions/filters/http/proto_message_logging/filter_config.h"

#include "test/extensions/filters/http/grpc_field_extraction/message_converter/message_converter_test_lib.h"
#include "test/mocks/http/mocks.h"
#include "test/proto/apikeys.pb.h"
#include "test/test_common/environment.h"
#include "test/test_common/utility.h"

#include "absl/log/log.h"
#include "absl/strings/string_view.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"

namespace Envoy {
namespace Extensions {
namespace HttpFilters {
namespace ProtoMessageLogging {
namespace {

using ::apikeys::ApiKey;
using ::apikeys::CreateApiKeyRequest;
using ::envoy::extensions::filters::http::proto_message_logging::v3::ProtoMessageLoggingConfig;
using ::Envoy::Extensions::HttpFilters::GrpcFieldExtraction::checkSerializedData;
using ::Envoy::Http::MockStreamDecoderFilterCallbacks;
using ::Envoy::Http::MockStreamEncoderFilterCallbacks;
using ::Envoy::Http::TestRequestHeaderMapImpl;
using ::Envoy::Http::TestResponseHeaderMapImpl;
using ::Envoy::ProtobufWkt::Struct;
using ::testing::Eq;

constexpr absl::string_view kFilterName = "envoy.filters.http.proto_message_logging";

constexpr absl::string_view kExpectedRequestMetadata = R"pb(
fields {
  key: "requests.first"
  value {
    list_value {
      values {
        struct_value {
          fields {
            key: "@type"
            value {
              string_value: "type.googleapis.com/apikeys.CreateApiKeyRequest"
            }
          }
          fields {
            key: "parent"
            value { string_value: "project-id" }
          }
        }
      }
    }
  }
}
)pb";

constexpr absl::string_view kExpectedResponseMetadata = R"pb(
fields {
  key: "responses.first"
  value {
    list_value {
      values {
        struct_value {
          fields {
            key: "@type"
            value { string_value: "type.googleapis.com/apikeys.ApiKey" }
          }
          fields {
            key: "name"
            value { string_value: "apikey-name" }
          }
        }
      }
    }
  }
}
)pb";

void checkProtoStruct(Envoy::ProtobufWkt::Struct got, absl::string_view expected_in_pbtext) {
  Envoy::ProtobufWkt::Struct expected;
  ASSERT_THAT(Envoy::Protobuf::TextFormat::ParseFromString(expected_in_pbtext, &expected), true);
  EXPECT_THAT(Envoy::TestUtility::protoEqual(got, expected, true), true)
      << "got:\n"
      << got.DebugString() << "expected:\n"
      << expected_in_pbtext;
}

class FilterTestBase : public ::testing::Test {
protected:
  FilterTestBase() : api_(Envoy::Api::createApiForTest()) {}

  void setUp(absl::string_view config = "") {
    ASSERT_THAT(Envoy::Protobuf::TextFormat::ParseFromString(
                    config.empty() ? protoConfig() : config, &proto_config_),
                true);

    *proto_config_.mutable_data_source()->mutable_inline_bytes() =
        api_->fileSystem()
            .fileReadToEnd(Envoy::TestEnvironment::runfilesPath("test/proto/apikeys.descriptor"))
            .value();

    ON_CALL(mock_decoder_callbacks_, decoderBufferLimit())
        .WillByDefault(testing::Return(UINT32_MAX));

    ON_CALL(mock_encoder_callbacks_, encoderBufferLimit())
        .WillByDefault(testing::Return(UINT32_MAX));

    filter_config_ = std::make_unique<FilterConfig>(
        proto_config_, std::make_unique<ExtractorFactoryImpl>(), *api_);

    filter_ = std::make_unique<Filter>(*filter_config_);
    filter_->setDecoderFilterCallbacks(mock_decoder_callbacks_);
    filter_->setEncoderFilterCallbacks(mock_encoder_callbacks_);
  }

  void TearDown() override {
    // Test onDestroy doesn't crash.
    filter_->PassThroughDecoderFilter::onDestroy();
    filter_->PassThroughEncoderFilter::onDestroy();
  }

  virtual std::string protoConfig() {
    return R"pb(
      mode: FIRST_AND_LAST
      logging_by_method: {
        key: "apikeys.ApiKeys.CreateApiKey"
        value: {
          request_logging_by_field: { key: "parent" value: LOG }
          response_logging_by_field: { key: "name" value: LOG }
        }
      }
    )pb";
  }

  Envoy::Api::ApiPtr api_;
  std::string config_;
  ProtoMessageLoggingConfig proto_config_;
  std::unique_ptr<FilterConfig> filter_config_;
  testing::NiceMock<MockStreamDecoderFilterCallbacks> mock_decoder_callbacks_;
  testing::NiceMock<MockStreamEncoderFilterCallbacks> mock_encoder_callbacks_;
  std::unique_ptr<Filter> filter_;
};

apikeys::CreateApiKeyRequest makeCreateApiKeyRequest(absl::string_view pb = R"pb(
      parent: "project-id"
      key: {
        display_name: "Display Name"
        current_key: "current-key"
        create_time { seconds: 1684306560 nanos: 0 }
        update_time { seconds: 1684306560 nanos: 0 }
        location: "global"
        kms_key: "projects/my-project/locations/my-location"
        expire_time { seconds: 1715842560 nanos: 0 }
      }
    )pb") {
  apikeys::CreateApiKeyRequest request;
  Envoy::Protobuf::TextFormat::ParseFromString(pb, &request);
  return request;
}

apikeys::ApiKey makeCreateApiKeyResponse(absl::string_view pb = R"pb(
  name: "apikey-name"
  display_name: "Display Name"
  current_key: "current-key"
  create_time { seconds: 1684306560 nanos: 0 }
  update_time { seconds: 1684306560 nanos: 0 }
  location: "global"
  kms_key: "projects/my-project/locations/my-location"
  expire_time { seconds: 1715842560 nanos: 0 }
)pb") {
  apikeys::ApiKey response;
  Envoy::Protobuf::TextFormat::ParseFromString(pb, &response);
  return response;
}

using FilterTestExtractOk = FilterTestBase;

TEST_F(FilterTestExtractOk, UnarySingleBuffer) {
  setUp();

  Envoy::Http::TestRequestHeaderMapImpl req_headers =
      TestRequestHeaderMapImpl{{":method", "POST"},
                               {":path", "/apikeys.ApiKeys/CreateApiKey"},
                               {"content-type", "application/grpc"}};
  EXPECT_EQ(Envoy::Http::FilterHeadersStatus::StopIteration,
            filter_->decodeHeaders(req_headers, true));

  CreateApiKeyRequest request = makeCreateApiKeyRequest();
  Envoy::Buffer::InstancePtr request_data = Envoy::Grpc::Common::serializeToGrpcFrame(request);

  EXPECT_CALL(mock_decoder_callbacks_.stream_info_, setDynamicMetadata(_, _))
      .WillOnce(
          Invoke([](const std::string& ns, const Envoy::ProtobufWkt::Struct& new_dynamic_metadata) {
            EXPECT_EQ(ns, kFilterName);
            checkProtoStruct(new_dynamic_metadata, kExpectedRequestMetadata);
          }));

  EXPECT_EQ(Envoy::Http::FilterDataStatus::Continue, filter_->decodeData(*request_data, true));

  // No data modification.
  checkSerializedData<CreateApiKeyRequest>(*request_data, {request});

  Envoy::Http::TestResponseHeaderMapImpl resp_headers = TestResponseHeaderMapImpl{
      {":status", "200"},
      {"grpc-status", "1"},
      {"content-type", "application/grpc"},
  };
  EXPECT_EQ(Envoy::Http::FilterHeadersStatus::StopIteration,
            filter_->encodeHeaders(resp_headers, false));

  apikeys::ApiKey response = makeCreateApiKeyResponse();
  Envoy::Buffer::InstancePtr response_data = Envoy::Grpc::Common::serializeToGrpcFrame(response);

  EXPECT_CALL(mock_encoder_callbacks_.stream_info_, setDynamicMetadata(_, _))
      .WillOnce(
          Invoke([](const std::string& ns, const Envoy::ProtobufWkt::Struct& new_dynamic_metadata) {
            EXPECT_EQ(ns, kFilterName);
            checkProtoStruct(new_dynamic_metadata, kExpectedResponseMetadata);
          }));

  EXPECT_EQ(Envoy::Http::FilterDataStatus::Continue, filter_->encodeData(*response_data, true));

  // No data modification.
  checkSerializedData<apikeys::ApiKey>(*response_data, {response});
}

TEST_F(FilterTestExtractOk, UnarySingleBufferWithMultipleFields) {
  setUp(R"pb(
      mode: FIRST_AND_LAST
      logging_by_method: {
        key: "apikeys.ApiKeys.CreateApiKey"
        value: {
          request_logging_by_field: { key: "parent" value: LOG }
          request_logging_by_field: { key: "key.display_name" value: LOG }
          request_logging_by_field: { key: "key.current_key" value: LOG }
          response_logging_by_field: { key: "name" value: LOG }
          response_logging_by_field: { key: "display_name" value: LOG }
          response_logging_by_field: { key: "current_key" value: LOG }
        }
      }
    )pb");

  Envoy::Http::TestRequestHeaderMapImpl req_headers =
      TestRequestHeaderMapImpl{{":method", "POST"},
                               {":path", "/apikeys.ApiKeys/CreateApiKey"},
                               {"content-type", "application/grpc"}};
  EXPECT_EQ(Envoy::Http::FilterHeadersStatus::StopIteration,
            filter_->decodeHeaders(req_headers, true));

  CreateApiKeyRequest request = makeCreateApiKeyRequest();
  Envoy::Buffer::InstancePtr request_data = Envoy::Grpc::Common::serializeToGrpcFrame(request);

  EXPECT_CALL(mock_decoder_callbacks_.stream_info_, setDynamicMetadata(_, _))
      .WillOnce(
          Invoke([](const std::string& ns, const Envoy::ProtobufWkt::Struct& new_dynamic_metadata) {
            EXPECT_EQ(ns, kFilterName);
            checkProtoStruct(new_dynamic_metadata, R"pb(
fields {
  key: "requests.first"
  value {
    list_value {
      values {
        struct_value {
          fields {
            key: "@type"
            value {
              string_value: "type.googleapis.com/apikeys.CreateApiKeyRequest"
            }
          }
          fields {
            key: "key"
            value {
              struct_value {
                fields {
                  key: "currentKey"
                  value {
                    string_value: "current-key"
                  }
                }
                fields {
                  key: "displayName"
                  value {
                    string_value: "Display Name"
                  }
                }
              }
            }
          }
          fields {
            key: "parent"
            value {
              string_value: "project-id"
            }
          }
        }
      }
    }
  }
}
)pb");
          }));

  EXPECT_EQ(Envoy::Http::FilterDataStatus::Continue, filter_->decodeData(*request_data, true));

  // No data modification.
  checkSerializedData<CreateApiKeyRequest>(*request_data, {request});

  Envoy::Http::TestResponseHeaderMapImpl resp_headers = TestResponseHeaderMapImpl{
      {":status", "200"},
      {"grpc-status", "1"},
      {"content-type", "application/grpc"},
  };
  EXPECT_EQ(Envoy::Http::FilterHeadersStatus::StopIteration,
            filter_->encodeHeaders(resp_headers, false));

  apikeys::ApiKey response = makeCreateApiKeyResponse();
  Envoy::Buffer::InstancePtr response_data = Envoy::Grpc::Common::serializeToGrpcFrame(response);

  EXPECT_CALL(mock_encoder_callbacks_.stream_info_, setDynamicMetadata(_, _))
      .WillOnce(
          Invoke([](const std::string& ns, const Envoy::ProtobufWkt::Struct& new_dynamic_metadata) {
            EXPECT_EQ(ns, kFilterName);
            checkProtoStruct(new_dynamic_metadata, R"pb(
fields {
  key: "responses.first"
  value {
    list_value {
      values {
        struct_value {
          fields {
            key: "@type"
            value {
              string_value: "type.googleapis.com/apikeys.ApiKey"
            }
          }
          fields {
            key: "currentKey"
            value {
              string_value: "current-key"
            }
          }
          fields {
            key: "displayName"
            value {
              string_value: "Display Name"
            }
          }
          fields {
            key: "name"
            value {
              string_value: "apikey-name"
            }
          }
        }
      }
    }
  }
}
)pb");
          }));

  EXPECT_EQ(Envoy::Http::FilterDataStatus::Continue, filter_->encodeData(*response_data, true));

  // No data modification.
  checkSerializedData<apikeys::ApiKey>(*response_data, {response});
}

TEST_F(FilterTestExtractOk, EmptyFields) {
  setUp();
  TestRequestHeaderMapImpl req_headers =
      TestRequestHeaderMapImpl{{":method", "POST"},
                               {":path", "/apikeys.ApiKeys/CreateApiKey"},
                               {"content-type", "application/grpc"}};
  EXPECT_EQ(Envoy::Http::FilterHeadersStatus::StopIteration,
            filter_->decodeHeaders(req_headers, true));

  CreateApiKeyRequest request = makeCreateApiKeyRequest("");
  Envoy::Buffer::InstancePtr request_data = Envoy::Grpc::Common::serializeToGrpcFrame(request);

  EXPECT_CALL(mock_decoder_callbacks_.stream_info_, setDynamicMetadata(_, _))
      .WillOnce(Invoke([](const std::string& ns, const Struct& new_dynamic_metadata) {
        EXPECT_EQ(ns, kFilterName);
        checkProtoStruct(new_dynamic_metadata, R"pb(
fields {
  key: "requests.first"
  value {
    list_value {
      values {
        struct_value {
          fields {
            key: "@type"
            value {
              string_value: "type.googleapis.com/apikeys.CreateApiKeyRequest"
            }
          }
        }
      }
    }
  }
}
)pb");
      }));
  EXPECT_EQ(Envoy::Http::FilterDataStatus::Continue, filter_->decodeData(*request_data, true));

  // No data modification.
  checkSerializedData<CreateApiKeyRequest>(*request_data, {request});

  Envoy::Http::TestResponseHeaderMapImpl resp_headers = TestResponseHeaderMapImpl{
      {":status", "200"},
      {"grpc-status", "1"},
      {"content-type", "application/grpc"},
  };
  EXPECT_EQ(Envoy::Http::FilterHeadersStatus::StopIteration,
            filter_->encodeHeaders(resp_headers, false));

  apikeys::ApiKey response = makeCreateApiKeyResponse("");
  Envoy::Buffer::InstancePtr response_data = Envoy::Grpc::Common::serializeToGrpcFrame(response);

  EXPECT_CALL(mock_encoder_callbacks_.stream_info_, setDynamicMetadata(_, _))
      .WillOnce(
          Invoke([](const std::string& ns, const Envoy::ProtobufWkt::Struct& new_dynamic_metadata) {
            EXPECT_EQ(ns, kFilterName);
            checkProtoStruct(new_dynamic_metadata, R"pb(
fields {
  key: "responses.first"
  value {
    list_value {
      values {
        struct_value {
          fields {
            key: "@type"
            value {
              string_value: "type.googleapis.com/apikeys.ApiKey"
            }
          }
        }
      }
    }
  }
}
)pb");
          }));

  EXPECT_EQ(Envoy::Http::FilterDataStatus::Continue, filter_->encodeData(*response_data, true));

  // No data modification.
  checkSerializedData<apikeys::ApiKey>(*response_data, {response});
}

TEST_F(FilterTestExtractOk, UnaryMultipeBuffers) {
  setUp();
  TestRequestHeaderMapImpl req_headers =
      TestRequestHeaderMapImpl{{":method", "POST"},
                               {":path", "/apikeys.ApiKeys/CreateApiKey"},
                               {"content-type", "application/grpc"}};
  EXPECT_EQ(Envoy::Http::FilterHeadersStatus::StopIteration,
            filter_->decodeHeaders(req_headers, true));

  const uint32_t req_start_data_size = 3;
  const uint32_t req_middle_data_size = 4;

  CreateApiKeyRequest request = makeCreateApiKeyRequest();
  Envoy::Buffer::InstancePtr request_data = Envoy::Grpc::Common::serializeToGrpcFrame(request);

  // Split into multiple buffers.
  Envoy::Buffer::OwnedImpl start_request_data;
  Envoy::Buffer::OwnedImpl middle_request_data;
  Envoy::Buffer::OwnedImpl end_request_data;
  start_request_data.move(*request_data, req_start_data_size);
  middle_request_data.move(*request_data, req_middle_data_size);
  end_request_data.move(*request_data);
  EXPECT_EQ(request_data->length(), 0);

  EXPECT_EQ(Envoy::Http::FilterDataStatus::StopIterationNoBuffer,
            filter_->decodeData(start_request_data, false));
  EXPECT_EQ(start_request_data.length(), 0);

  EXPECT_EQ(Envoy::Http::FilterDataStatus::StopIterationNoBuffer,
            filter_->decodeData(middle_request_data, false));
  EXPECT_EQ(middle_request_data.length(), 0);

  EXPECT_CALL(mock_decoder_callbacks_.stream_info_, setDynamicMetadata(_, _))
      .WillOnce(Invoke([](const std::string& ns, const Struct& new_dynamic_metadata) {
        EXPECT_EQ(ns, kFilterName);
        checkProtoStruct(new_dynamic_metadata, kExpectedRequestMetadata);
      }));
  EXPECT_EQ(Envoy::Http::FilterDataStatus::Continue, filter_->decodeData(end_request_data, true));

  // Inject data back and no data modification.
  checkSerializedData<CreateApiKeyRequest>(end_request_data, {request});

  Envoy::Http::TestResponseHeaderMapImpl resp_headers = TestResponseHeaderMapImpl{
      {":status", "200"},
      {"grpc-status", "1"},
      {"content-type", "application/grpc"},
  };
  EXPECT_EQ(Envoy::Http::FilterHeadersStatus::StopIteration,
            filter_->encodeHeaders(resp_headers, true));

  const uint32_t resp_start_data_size = 1;
  const uint32_t resp_middle_data_size = 2;

  apikeys::ApiKey response = makeCreateApiKeyResponse();
  Envoy::Buffer::InstancePtr response_data = Envoy::Grpc::Common::serializeToGrpcFrame(response);

  // Split into multiple buffers.
  Envoy::Buffer::OwnedImpl start_response_data;
  Envoy::Buffer::OwnedImpl middle_response_data;
  Envoy::Buffer::OwnedImpl end_response_data;
  start_response_data.move(*response_data, resp_start_data_size);
  middle_response_data.move(*response_data, resp_middle_data_size);
  end_response_data.move(*response_data);
  EXPECT_EQ(response_data->length(), 0);

  EXPECT_EQ(Envoy::Http::FilterDataStatus::StopIterationNoBuffer,
            filter_->encodeData(start_response_data, false));
  EXPECT_EQ(start_response_data.length(), 0);

  EXPECT_EQ(Envoy::Http::FilterDataStatus::StopIterationNoBuffer,
            filter_->encodeData(middle_response_data, false));
  EXPECT_EQ(middle_response_data.length(), 0);

  EXPECT_CALL(mock_encoder_callbacks_.stream_info_, setDynamicMetadata(_, _))
      .WillOnce(
          Invoke([](const std::string& ns, const Envoy::ProtobufWkt::Struct& new_dynamic_metadata) {
            EXPECT_EQ(ns, kFilterName);
            checkProtoStruct(new_dynamic_metadata, kExpectedResponseMetadata);
          }));

  EXPECT_EQ(Envoy::Http::FilterDataStatus::Continue, filter_->encodeData(end_response_data, true));

  // No data modification.
  checkSerializedData<apikeys::ApiKey>(*response_data, {response});
}

TEST_F(FilterTestExtractOk, StreamingMultipleMessageSingleBuffer) {
  setUp(R"pb(
mode: FIRST_AND_LAST
logging_by_method: {
  key: "apikeys.ApiKeys.CreateApiKeyInStream"
  value: {
    request_logging_by_field: {
      key: "parent"
      value: LOG
    }
  }
}
    )pb");
  TestRequestHeaderMapImpl req_headers =
      TestRequestHeaderMapImpl{{":method", "POST"},
                               {":path", "/apikeys.ApiKeys/CreateApiKeyInStream"},
                               {"content-type", "application/grpc"}};
  EXPECT_EQ(Envoy::Http::FilterHeadersStatus::StopIteration,
            filter_->decodeHeaders(req_headers, true));
  CreateApiKeyRequest request1 = makeCreateApiKeyRequest();
  Envoy::Buffer::InstancePtr request_data1 = Envoy::Grpc::Common::serializeToGrpcFrame(request1);
  CreateApiKeyRequest request2 = makeCreateApiKeyRequest(
      R"pb(
      parent: "from-req2"
)pb");
  Envoy::Buffer::InstancePtr request_data2 = Envoy::Grpc::Common::serializeToGrpcFrame(request2);
  CreateApiKeyRequest request3 = makeCreateApiKeyRequest(
      R"pb(
      parent: "from-req3"
)pb");
  Envoy::Buffer::InstancePtr request_data3 = Envoy::Grpc::Common::serializeToGrpcFrame(request3);

  // Split into multiple buffers.
  Envoy::Buffer::OwnedImpl request_data;
  request_data.move(*request_data1);
  request_data.move(*request_data2);
  request_data.move(*request_data3);
  EXPECT_EQ(request_data1->length(), 0);
  EXPECT_EQ(request_data2->length(), 0);
  EXPECT_EQ(request_data3->length(), 0);

  EXPECT_CALL(mock_decoder_callbacks_.stream_info_, setDynamicMetadata(_, _))
      .WillOnce(Invoke([](const std::string& ns, const ProtobufWkt::Struct& new_dynamic_metadata) {
        EXPECT_EQ(ns, "envoy.filters.http.proto_message_logging");
        checkProtoStruct(new_dynamic_metadata, kExpectedRequestMetadata);
      }));
  EXPECT_EQ(Envoy::Http::FilterDataStatus::Continue, filter_->decodeData(request_data, false));

  // Inject data back and no data modification.
  checkSerializedData<CreateApiKeyRequest>(request_data, {request1, request2, request3});

  // No op for the following messages.
  CreateApiKeyRequest request4 = makeCreateApiKeyRequest(
      R"pb(
      parent: "from-req4"
)pb");
  Envoy::Buffer::InstancePtr request_data4 = Envoy::Grpc::Common::serializeToGrpcFrame(request4);
  EXPECT_EQ(Envoy::Http::FilterDataStatus::Continue, filter_->decodeData(*request_data4, true));
  checkSerializedData<CreateApiKeyRequest>(*request_data4, {request4});
}

using FilterTestFieldTypes = FilterTestBase;

TEST_F(FilterTestFieldTypes, SingularType) {
  setUp(R"pb(
mode: FIRST_AND_LAST
logging_by_method: {
  key: "apikeys.ApiKeys.CreateApiKey"
  value: {
    request_logging_by_field: {
      key: "supported_types.string"
      value: LOG
    }
    request_logging_by_field: {
      key: "supported_types.uint32"
      value: LOG
    }
    request_logging_by_field: {
      key: "supported_types.uint64"
      value: LOG
    }
    request_logging_by_field: {
      key: "supported_types.int32"
      value: LOG
    }
    request_logging_by_field: {
      key: "supported_types.int64"
      value: LOG
    }
    request_logging_by_field: {
      key: "supported_types.sint32"
      value: LOG
    }
    request_logging_by_field: {
      key: "supported_types.sint64"
      value: LOG
    }
    request_logging_by_field: {
      key: "supported_types.fixed32"
      value: LOG
    }
    request_logging_by_field: {
      key: "supported_types.fixed64"
      value: LOG
    }
    request_logging_by_field: {
      key: "supported_types.sfixed32"
      value: LOG
    }
    request_logging_by_field: {
      key: "supported_types.sfixed64"
      value: LOG
    }
    request_logging_by_field: {
      key: "supported_types.float"
      value: LOG
    }
    request_logging_by_field: {
      key: "supported_types.double"
      value: LOG
    }
  }
})pb");
  TestRequestHeaderMapImpl req_headers =
      TestRequestHeaderMapImpl{{":method", "POST"},
                               {":path", "/apikeys.ApiKeys/CreateApiKey"},
                               {"content-type", "application/grpc"}};
  EXPECT_EQ(Envoy::Http::FilterHeadersStatus::StopIteration,
            filter_->decodeHeaders(req_headers, false));

  CreateApiKeyRequest request = makeCreateApiKeyRequest(
      R"pb(
supported_types: {
  string: "1"
  uint32: 2
  uint64: 3
  int32: 4
  int64: 5
  sint32: 6
  sint64: 7
  fixed32: 8
  fixed64: 9
  sfixed32: 10
  sfixed64: 11
  float: 1.2
  double: 1.3
}
)pb");
  Envoy::Buffer::InstancePtr request_data = Envoy::Grpc::Common::serializeToGrpcFrame(request);
  EXPECT_CALL(mock_decoder_callbacks_.stream_info_, setDynamicMetadata(_, _))
      .WillOnce(Invoke([](const std::string& ns, const ProtobufWkt::Struct& new_dynamic_metadata) {
        EXPECT_EQ(ns, "envoy.filters.http.proto_message_logging");
        checkProtoStruct(new_dynamic_metadata, R"pb(fields {
  key: "requests.first"
  value {
    list_value {
      values {
        struct_value {
          fields {
            key: "@type"
            value {
              string_value: "type.googleapis.com/apikeys.CreateApiKeyRequest"
            }
          }
          fields {
            key: "supportedTypes"
            value {
              struct_value {
                fields {
                  key: "double"
                  value {
                    number_value: 1.3
                  }
                }
                fields {
                  key: "fixed32"
                  value {
                    number_value: 8
                  }
                }
                fields {
                  key: "fixed64"
                  value {
                    string_value: "9"
                  }
                }
                fields {
                  key: "float"
                  value {
                    number_value: 1.2
                  }
                }
                fields {
                  key: "int32"
                  value {
                    number_value: 4
                  }
                }
                fields {
                  key: "int64"
                  value {
                    string_value: "5"
                  }
                }
                fields {
                  key: "sfixed32"
                  value {
                    number_value: 10
                  }
                }
                fields {
                  key: "sfixed64"
                  value {
                    string_value: "11"
                  }
                }
                fields {
                  key: "sint32"
                  value {
                    number_value: 6
                  }
                }
                fields {
                  key: "sint64"
                  value {
                    string_value: "7"
                  }
                }
                fields {
                  key: "string"
                  value {
                    string_value: "1"
                  }
                }
                fields {
                  key: "uint32"
                  value {
                    number_value: 2
                  }
                }
                fields {
                  key: "uint64"
                  value {
                    string_value: "3"
                  }
                }
              }
            }
          }
        }
      }
    }
  }
})pb");
      }));
  EXPECT_EQ(Envoy::Http::FilterDataStatus::Continue, filter_->decodeData(*request_data, true));

  // No data modification.
  checkSerializedData<CreateApiKeyRequest>(*request_data, {request});
}

TEST_F(FilterTestFieldTypes, RepeatedIntermediate) {
  setUp(R"pb(
mode: FIRST_AND_LAST
logging_by_method: {
  key: "apikeys.ApiKeys.CreateApiKey"
  value: {
    request_logging_by_field: {
      key: "repeated_intermediate.values.list_value.values.string_value"
      value: LOG
    }
  }
})pb");
  TestRequestHeaderMapImpl req_headers =
      TestRequestHeaderMapImpl{{":method", "POST"},
                               {":path", "/apikeys.ApiKeys/CreateApiKey"},
                               {"content-type", "application/grpc"}};
  EXPECT_EQ(Envoy::Http::FilterHeadersStatus::StopIteration,
            filter_->decodeHeaders(req_headers, false));

  CreateApiKeyRequest request = makeCreateApiKeyRequest(
      R"pb(
repeated_intermediate: {
  values: {
    list_value: {
      values: {
        string_value: "1"
      }
      values: {
        string_value: "2"
      }
    }
  }
  values: {
    list_value: {
      values: {
        string_value: "3"
      }
      values: {
        string_value: "4"
      }
    }
  }
}
)pb");
  Envoy::Buffer::InstancePtr request_data = Envoy::Grpc::Common::serializeToGrpcFrame(request);
  EXPECT_CALL(mock_decoder_callbacks_.stream_info_, setDynamicMetadata(_, _))
      .WillOnce(Invoke([](const std::string& ns, const ProtobufWkt::Struct& new_dynamic_metadata) {
        EXPECT_EQ(ns, "envoy.filters.http.proto_message_logging");
        checkProtoStruct(new_dynamic_metadata, R"pb(
fields {
  key: "requests.first"
  value {
    list_value {
      values {
        struct_value {
          fields {
            key: "@type"
            value {
              string_value: "type.googleapis.com/apikeys.CreateApiKeyRequest"
            }
          }
        }
      }
    }
  }
}
)pb");
      }));
  EXPECT_EQ(Envoy::Http::FilterDataStatus::Continue, filter_->decodeData(*request_data, true));

  // No data modification.
  checkSerializedData<CreateApiKeyRequest>(*request_data, {request});
}

TEST_F(FilterTestFieldTypes, RepeatedTypes) {
  setUp(R"pb(
mode: FIRST_AND_LAST
logging_by_method: {
  key: "apikeys.ApiKeys.CreateApiKey"
  value: {
    request_logging_by_field: {
      key: "repeated_supported_types.string"
      value: LOG
    }
    request_logging_by_field: {
      key: "repeated_supported_types.uint32"
      value: LOG
    }
    request_logging_by_field: {
      key: "repeated_supported_types.uint64"
      value: LOG
    }
    request_logging_by_field: {
      key: "repeated_supported_types.int32"
      value: LOG
    }
    request_logging_by_field: {
      key: "repeated_supported_types.int64"
      value: LOG
    }
    request_logging_by_field: {
      key: "repeated_supported_types.sint32"
      value: LOG
    }
    request_logging_by_field: {
      key: "repeated_supported_types.sint64"
      value: LOG
    }
    request_logging_by_field: {
      key: "repeated_supported_types.fixed32"
      value: LOG
    }
    request_logging_by_field: {
      key: "repeated_supported_types.fixed64"
      value: LOG
    }
    request_logging_by_field: {
      key: "repeated_supported_types.sfixed32"
      value: LOG
    }
    request_logging_by_field: {
      key: "repeated_supported_types.sfixed64"
      value: LOG
    }
    request_logging_by_field: {
      key: "repeated_supported_types.float"
      value: LOG
    }
    request_logging_by_field: {
      key: "repeated_supported_types.double"
      value: LOG
    }
  }
})pb");
  TestRequestHeaderMapImpl req_headers =
      TestRequestHeaderMapImpl{{":method", "POST"},
                               {":path", "/apikeys.ApiKeys/CreateApiKey"},
                               {"content-type", "application/grpc"}};
  EXPECT_EQ(Envoy::Http::FilterHeadersStatus::StopIteration,
            filter_->decodeHeaders(req_headers, false));

  CreateApiKeyRequest request = makeCreateApiKeyRequest(
      R"pb(
repeated_supported_types: {
  string: "1"
  uint32: 2
  uint64: 3
  int32: 4
  int64: 5
  sint32: 6
  sint64: 7
  fixed32: 8
  fixed64: 9
  sfixed32: 10
  sfixed64: 11
  float: 1.2
  double: 1.3
  string: "11"
  uint32: 22
  uint64: 33
  int32: 44
  int64: 55
  sint32: 66
  sint64: 77
  fixed32: 88
  fixed64: 99
  sfixed32: 1010
  sfixed64: 1111
  float: 1.212
  double: 1.313
}

)pb");
  Envoy::Buffer::InstancePtr request_data = Envoy::Grpc::Common::serializeToGrpcFrame(request);
  EXPECT_CALL(mock_decoder_callbacks_.stream_info_, setDynamicMetadata(_, _))
      .WillOnce(Invoke([](const std::string& ns, const ProtobufWkt::Struct& new_dynamic_metadata) {
        EXPECT_EQ(ns, "envoy.filters.http.proto_message_logging");
        checkProtoStruct(new_dynamic_metadata, R"pb(
fields {
  key: "requests.first"
  value {
    list_value {
      values {
        struct_value {
          fields {
            key: "@type"
            value {
              string_value: "type.googleapis.com/apikeys.CreateApiKeyRequest"
            }
          }
          fields {
            key: "repeatedSupportedTypes"
            value {
              struct_value {
                fields {
                  key: "double"
                  value {
                    list_value {
                      values {
                        number_value: 1.3
                      }
                      values {
                        number_value: 1.313
                      }
                    }
                  }
                }
                fields {
                  key: "fixed32"
                  value {
                    list_value {
                      values {
                        number_value: 8
                      }
                      values {
                        number_value: 88
                      }
                    }
                  }
                }
                fields {
                  key: "fixed64"
                  value {
                    list_value {
                      values {
                        string_value: "9"
                      }
                      values {
                        string_value: "99"
                      }
                    }
                  }
                }
                fields {
                  key: "float"
                  value {
                    list_value {
                      values {
                        number_value: 1.2
                      }
                      values {
                        number_value: 1.212
                      }
                    }
                  }
                }
                fields {
                  key: "int32"
                  value {
                    list_value {
                      values {
                        number_value: 4
                      }
                      values {
                        number_value: 44
                      }
                    }
                  }
                }
                fields {
                  key: "int64"
                  value {
                    list_value {
                      values {
                        string_value: "5"
                      }
                      values {
                        string_value: "55"
                      }
                    }
                  }
                }
                fields {
                  key: "sfixed32"
                  value {
                    list_value {
                      values {
                        number_value: 10
                      }
                      values {
                        number_value: 1010
                      }
                    }
                  }
                }
                fields {
                  key: "sfixed64"
                  value {
                    list_value {
                      values {
                        string_value: "11"
                      }
                      values {
                        string_value: "1111"
                      }
                    }
                  }
                }
                fields {
                  key: "sint32"
                  value {
                    list_value {
                      values {
                        number_value: 6
                      }
                      values {
                        number_value: 66
                      }
                    }
                  }
                }
                fields {
                  key: "sint64"
                  value {
                    list_value {
                      values {
                        string_value: "7"
                      }
                      values {
                        string_value: "77"
                      }
                    }
                  }
                }
                fields {
                  key: "string"
                  value {
                    list_value {
                      values {
                        string_value: "1"
                      }
                      values {
                        string_value: "11"
                      }
                    }
                  }
                }
                fields {
                  key: "uint32"
                  value {
                    list_value {
                      values {
                        number_value: 2
                      }
                      values {
                        number_value: 22
                      }
                    }
                  }
                }
                fields {
                  key: "uint64"
                  value {
                    list_value {
                      values {
                        string_value: "3"
                      }
                      values {
                        string_value: "33"
                      }
                    }
                  }
                }
              }
            }
          }
        }
      }
    }
  }
})pb");
      }));
  EXPECT_EQ(Envoy::Http::FilterDataStatus::Continue, filter_->decodeData(*request_data, true));

  // No data modification.
  checkSerializedData<CreateApiKeyRequest>(*request_data, {request});
}

using FilterTestExtractRejected = FilterTestBase;

TEST_F(FilterTestExtractRejected, BufferLimitedExceeded) {
  setUp();
  ON_CALL(mock_decoder_callbacks_, decoderBufferLimit()).WillByDefault(testing::Return(0));

  TestRequestHeaderMapImpl req_headers =
      TestRequestHeaderMapImpl{{":method", "POST"},
                               {":path", "/apikeys.ApiKeys/CreateApiKey"},
                               {"content-type", "application/grpc"}};
  EXPECT_EQ(Envoy::Http::FilterHeadersStatus::StopIteration,
            filter_->decodeHeaders(req_headers, true));

  CreateApiKeyRequest request = makeCreateApiKeyRequest();
  Envoy::Buffer::InstancePtr request_data = Envoy::Grpc::Common::serializeToGrpcFrame(request);

  EXPECT_CALL(mock_decoder_callbacks_,
              sendLocalReply(
                  Http::Code::BadRequest, "Rejected because internal buffer limits are exceeded.",
                  Eq(nullptr), Eq(Envoy::Grpc::Status::FailedPrecondition),
                  "proto_message_logging_FAILED_PRECONDITION{REQUEST_BUFFER_CONVERSION_FAIL}"));
  EXPECT_EQ(Envoy::Http::FilterDataStatus::StopIterationNoBuffer,
            filter_->decodeData(*request_data, true));
}

TEST_F(FilterTestExtractRejected, NotEnoughData) {
  setUp();
  TestRequestHeaderMapImpl req_headers =
      TestRequestHeaderMapImpl{{":method", "POST"},
                               {":path", "/apikeys.ApiKeys/CreateApiKey"},
                               {"content-type", "application/grpc"}};
  EXPECT_EQ(Envoy::Http::FilterHeadersStatus::StopIteration,
            filter_->decodeHeaders(req_headers, false));

  Envoy::Buffer::OwnedImpl empty;

  EXPECT_CALL(mock_decoder_callbacks_,
              sendLocalReply(Http::Code::BadRequest,
                             "did not receive enough data to form a message.", Eq(nullptr),
                             Eq(Envoy::Grpc::Status::InvalidArgument),
                             "proto_message_logging_INVALID_ARGUMENT{REQUEST_OUT_OF_DATA}"));
  EXPECT_EQ(Envoy::Http::FilterDataStatus::StopIterationNoBuffer, filter_->decodeData(empty, true));
}

TEST_F(FilterTestExtractRejected, MisformedGrpcPath) {
  setUp();
  ON_CALL(mock_decoder_callbacks_, decoderBufferLimit()).WillByDefault(testing::Return(0));

  TestRequestHeaderMapImpl req_headers = TestRequestHeaderMapImpl{
      {":method", "POST"}, {":path", "/misformatted"}, {"content-type", "application/grpc"}};
  EXPECT_CALL(mock_decoder_callbacks_,
              sendLocalReply(Http::Code::BadRequest,
                             ":path `/misformatted` should be in form of `/package.service/method`",
                             Eq(nullptr), Eq(Envoy::Grpc::Status::InvalidArgument),
                             "proto_message_logging_INVALID_ARGUMENT{BAD_REQUEST}"));

  EXPECT_EQ(Envoy::Http::FilterHeadersStatus::StopIteration,
            filter_->decodeHeaders(req_headers, false));
}

using FilterTestPassThrough = FilterTestBase;

TEST_F(FilterTestPassThrough, NotGrpc) {
  setUp();
  TestRequestHeaderMapImpl req_headers =
      TestRequestHeaderMapImpl{{":method", "POST"},
                               {":path", "/apikeys.ApiKeys/CreateApiKey"},
                               {"content-type", "not-grpc"}};

  // Pass through headers directly.
  EXPECT_EQ(Envoy::Http::FilterHeadersStatus::Continue, filter_->decodeHeaders(req_headers, true));
}

TEST_F(FilterTestPassThrough, PathNotExist) {
  setUp();
  TestRequestHeaderMapImpl req_headers =
      TestRequestHeaderMapImpl{{":method", "POST"}, {"content-type", "application/grpc"}};

  // Pass through headers directly.
  EXPECT_EQ(Envoy::Http::FilterHeadersStatus::Continue, filter_->decodeHeaders(req_headers, true));
}

TEST_F(FilterTestPassThrough, UnconfiguredRequest) {
  setUp();
  TestRequestHeaderMapImpl req_headers =
      TestRequestHeaderMapImpl{{":method", "POST"},
                               {":path", "/pkg.svc/UnconfiguredRequest"},
                               {"content-type", "application/grpc"}};

  // Pass through headers directly.
  EXPECT_EQ(Envoy::Http::FilterHeadersStatus::Continue, filter_->decodeHeaders(req_headers, true));
}
} // namespace

} // namespace ProtoMessageLogging
} // namespace HttpFilters
} // namespace Extensions
} // namespace Envoy
