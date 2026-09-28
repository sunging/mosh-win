/*
 * Mosh: the mobile shell
 * Copyright 2026 The mosh-win contributors
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#pragma once

#ifndef _WIN32
#error "win32_crypto.h is only available on Windows"
#endif

#include <windows.h>
#include <bcrypt.h>

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>

namespace mosh::win32 {

class CryptoError : public std::runtime_error {
public:
  CryptoError(const char *operation, NTSTATUS status);

  [[nodiscard]] NTSTATUS status() const noexcept { return status_; }

private:
  NTSTATUS status_;
};

/* Fill a caller-owned buffer with bytes from the system-preferred CNG RNG. */
void secure_random(void *destination, std::size_t size);

/*
 * A small AES-128 ECB primitive for Mosh's upstream OCB3 implementation.
 * ECB is deliberately exposed only as whole 16-byte blocks; OCB supplies
 * authentication, nonces, and block composition itself.
 */
class Aes128Ecb final {
public:
  static constexpr std::size_t block_size = 16;
  static constexpr std::size_t key_size = 16;

  explicit Aes128Ecb(const std::uint8_t key[key_size]);
  ~Aes128Ecb();

  Aes128Ecb(const Aes128Ecb &) = delete;
  Aes128Ecb &operator=(const Aes128Ecb &) = delete;
  Aes128Ecb(Aes128Ecb &&) = delete;
  Aes128Ecb &operator=(Aes128Ecb &&) = delete;

  void encrypt_blocks(const void *source, void *destination,
                      std::size_t block_count) const;
  void decrypt_blocks(const void *source, void *destination,
                      std::size_t block_count) const;

private:
  void crypt_blocks(bool encrypt, const void *source, void *destination,
                    std::size_t block_count) const;

  BCRYPT_ALG_HANDLE algorithm_{nullptr};
  BCRYPT_KEY_HANDLE key_{nullptr};
  PUCHAR key_object_{nullptr};
  ULONG key_object_size_{0};
};

/* Best-effort secret memory helpers used by the launcher and client. */
void secure_erase(void *data, std::size_t size) noexcept;
bool lock_secret_memory(void *data, std::size_t size) noexcept;
void unlock_secret_memory(void *data, std::size_t size) noexcept;

} // namespace mosh::win32
