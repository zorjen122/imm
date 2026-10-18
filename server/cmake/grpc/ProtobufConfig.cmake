# gRPC core uses bundled upb. Project service messages are generated separately
# with the installed protoc/runtime; expose those imported targets to gRPC's
# package lookup without building its internal C++ code-generation targets.
include(FindProtobuf)
