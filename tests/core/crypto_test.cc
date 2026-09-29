/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "ae.h"
#include "test_support.h"
#include "crypto.h"
#include "platform/win32_crypto.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <iterator>
#include <new>
#include <string>
#include <vector>

namespace {

using mosh::test::expect;

void expect_invalid_base64_key(const std::string &value,
                               const char *message) {
  try {
    const Crypto::Base64Key key(value);
    (void)key;
  } catch (const Crypto::CryptoException &) {
    return;
  }
  expect(false, message);
}

} // namespace

int main() {
  Crypto::Base64Key generated_key;
  const std::string printable_key = generated_key.printable_key();
  expect(printable_key.size() == 22,
         "generated base64 key has the wrong length");
  Crypto::Base64Key parsed_key(printable_key);
  expect(parsed_key.printable_key() == printable_key &&
             std::memcmp(parsed_key.data(), generated_key.data(), 16) == 0,
         "base64 key round trip failed");

  alignas(Crypto::Base64Key)
      unsigned char key_storage[sizeof(Crypto::Base64Key)];
  std::memset(key_storage, 0xa5, sizeof(key_storage));
  auto *erasable_key =
      new (key_storage) Crypto::Base64Key(printable_key);
  expect(std::any_of(std::begin(key_storage), std::end(key_storage),
                     [](unsigned char value) { return value != 0; }),
         "placement key fixture was unexpectedly empty");
  erasable_key->~Base64Key();
  expect(std::all_of(std::begin(key_storage), std::end(key_storage),
                     [](unsigned char value) { return value == 0; }),
         "Base64Key destructor did not erase its raw key bytes");

  expect_invalid_base64_key("short", "accepted a short base64 key");
  expect_invalid_base64_key(std::string(22, '!'),
                            "accepted malformed base64 characters");
  expect_invalid_base64_key("AAAAAAAAAAAAAAAAAAAAAB",
                            "accepted a non-canonical 128-bit base64 key");

  const std::array<std::uint8_t, 16> aes_key{
      0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
      0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f};
  const std::array<std::uint8_t, 16> plaintext{
      0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77,
      0x88, 0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff};
  const std::array<std::uint8_t, 16> expected_ciphertext{
      0x69, 0xc4, 0xe0, 0xd8, 0x6a, 0x7b, 0x04, 0x30,
      0xd8, 0xcd, 0xb7, 0x80, 0x70, 0xb4, 0xc5, 0x5a};

  mosh::win32::Aes128Ecb aes(aes_key.data());
  std::array<std::uint8_t, 16> ciphertext{};
  aes.encrypt_blocks(plaintext.data(), ciphertext.data(), 1);
  expect(ciphertext == expected_ciphertext, "AES-128 known-answer failure");
  std::array<std::uint8_t, 16> decrypted{};
  aes.decrypt_blocks(ciphertext.data(), decrypted.data(), 1);
  expect(decrypted == plaintext, "AES-128 decrypt failure");

  std::array<std::uint8_t, 64> random_a{};
  std::array<std::uint8_t, 64> random_b{};
  mosh::win32::secure_random(random_a.data(), random_a.size());
  mosh::win32::secure_random(random_b.data(), random_b.size());
  expect(random_a != random_b, "CNG RNG repeated a 512-bit output");

  const std::array<std::uint8_t, 12> nonce{
      0xbb, 0xaa, 0x99, 0x88, 0x77, 0x66,
      0x55, 0x44, 0x33, 0x22, 0x11, 0x00};
  const std::array<std::uint8_t, 16> expected_empty_ocb{
      0x78, 0x54, 0x07, 0xbf, 0xff, 0xc8, 0xad, 0x9e,
      0xdc, 0xc5, 0x52, 0x0a, 0xc9, 0x11, 0x1e, 0xe6};
  std::array<std::uint8_t, 16> ocb_output{};
  const std::size_t context_words =
      (static_cast<std::size_t>(ae_ctx_sizeof()) + sizeof(std::max_align_t) -
       1) /
      sizeof(std::max_align_t);
  std::vector<std::max_align_t> context_storage(context_words);
  auto *context = reinterpret_cast<ae_ctx *>(context_storage.data());
  expect(ae_init(context, aes_key.data(), 16, 12, 16) == AE_SUCCESS,
         "ae_init failed");
  const int empty_length =
      ae_encrypt(context, nonce.data(), nullptr, 0, nullptr, 0,
                 ocb_output.data(), nullptr, AE_FINALIZE);
  expect(empty_length == 16 && ocb_output == expected_empty_ocb,
         "RFC 7253 empty OCB vector failure");

  const std::array<std::uint8_t, 23> message{
      'm', 'o', 's', 'h', '-', 'w', 'i', 'n', '-', 'o', 'c', 'b',
      '-', 'r', 'o', 'u', 'n', 'd', '-', 't', 'r', 'i', 'p'};
  std::array<std::uint8_t, message.size() + 16> sealed{};
  std::array<std::uint8_t, message.size()> opened{};
  const int sealed_length =
      ae_encrypt(context, nonce.data(), message.data(), message.size(), nullptr,
                 0, sealed.data(), nullptr, AE_FINALIZE);
  expect(sealed_length == static_cast<int>(sealed.size()),
         "OCB encrypt length failure");
  const int opened_length =
      ae_decrypt(context, nonce.data(), sealed.data(), sealed.size(), nullptr,
                 0, opened.data(), nullptr, AE_FINALIZE);
  expect(opened_length == static_cast<int>(message.size()) && opened == message,
         "OCB round trip failure");
  sealed.back() ^= 1;
  expect(ae_decrypt(context, nonce.data(), sealed.data(), sealed.size(), nullptr,
                    0, opened.data(), nullptr, AE_FINALIZE) == AE_INVALID,
         "OCB accepted a modified tag");
  expect(ae_clear(context) == AE_SUCCESS, "ae_clear failed");
  return 0;
}
