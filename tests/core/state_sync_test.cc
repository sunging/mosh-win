#include "compressor.h"
#include "test_support.h"
#include "user.h"
#include "userinput.pb.h"

#include <cstdlib>
#include <iostream>
#include <string>

namespace {

using mosh::test::expect;

void test_protobuf_round_trip() {
  ClientBuffers::UserMessage message;

  const std::string binary_keys("a\0\xff", 3);
  ClientBuffers::Instruction *keys = message.add_instruction();
  keys->MutableExtension(ClientBuffers::keystroke)->set_keys(binary_keys);

  ClientBuffers::Instruction *resize = message.add_instruction();
  resize->MutableExtension(ClientBuffers::resize)->set_width(137);
  resize->MutableExtension(ClientBuffers::resize)->set_height(43);

  std::string wire;
  expect(message.SerializeToString(&wire), "protobuf serialization failed");
  expect(!wire.empty(), "protobuf serialization produced no bytes");

  ClientBuffers::UserMessage parsed;
  expect(parsed.ParseFromString(wire), "protobuf parsing failed");
  expect(parsed.instruction_size() == 2,
         "protobuf instruction count changed");
  expect(parsed.instruction(0).HasExtension(ClientBuffers::keystroke),
         "protobuf lost keystroke extension");
  expect(parsed.instruction(0)
                 .GetExtension(ClientBuffers::keystroke)
                 .keys() == binary_keys,
         "protobuf changed binary keystroke data");
  expect(parsed.instruction(1).HasExtension(ClientBuffers::resize),
         "protobuf lost resize extension");
  expect(parsed.instruction(1).GetExtension(ClientBuffers::resize).width() ==
             137 &&
             parsed.instruction(1)
                     .GetExtension(ClientBuffers::resize)
                     .height() == 43,
         "protobuf changed resize values");
}

void test_compressor_round_trip() {
  std::string input;
  input.reserve(64 * 1024);
  for (int repetition = 0; repetition < 256; ++repetition) {
    for (int byte = 0; byte < 256; ++byte) {
      input.push_back(static_cast<char>(byte));
    }
  }

  Network::Compressor &compressor = Network::get_compressor();
  const std::string compressed = compressor.compress_str(input);
  expect(compressed.size() < input.size(),
         "zlib did not compress the repetitive fixture");
  expect(compressor.uncompress_str(compressed) == input,
         "zlib compressor round trip changed binary data");
}

void test_user_stream_round_trip() {
  Network::UserStream authoritative;
  authoritative.push_back(Parser::UserByte('a'));
  authoritative.push_back(Parser::UserByte(0));
  authoritative.push_back(Parser::Resize(132, 41));
  authoritative.push_back(Parser::UserByte(0xff));

  Network::Compressor &compressor = Network::get_compressor();
  const std::string snapshot = authoritative.init_diff();
  const std::string packed = compressor.compress_str(snapshot);

  Network::UserStream replica;
  replica.apply_string(compressor.uncompress_str(packed));
  expect(replica == authoritative,
         "UserStream snapshot did not reconstruct the state");

  Network::UserStream prefix;
  prefix.push_back(Parser::UserByte('a'));
  prefix.push_back(Parser::UserByte(0));

  Network::UserStream incrementally_restored = prefix;
  incrementally_restored.apply_string(authoritative.diff_from(prefix));
  expect(incrementally_restored == authoritative,
         "UserStream incremental diff did not reconstruct the state");

  Network::UserStream suffix = authoritative;
  suffix.subtract(&prefix);
  Network::UserStream expected_suffix;
  expected_suffix.push_back(Parser::Resize(132, 41));
  expected_suffix.push_back(Parser::UserByte(0xff));
  expect(suffix == expected_suffix, "UserStream prefix subtraction failed");
}

} // namespace

int main() {
  test_protobuf_round_trip();
  test_compressor_round_trip();
  test_user_stream_round_trip();
  return 0;
}
