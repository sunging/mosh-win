/*
 * Mosh: the mobile shell
 * Copyright 2026 The mosh-win contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "win32_crypto.h"

#include <algorithm>
#include <limits>
#include <sstream>

namespace mosh::win32 {
namespace {

[[nodiscard]] std::string status_message(const char *operation,
                                         NTSTATUS status) {
  std::ostringstream out;
  out << operation << " failed with NTSTATUS 0x" << std::hex
      << static_cast<unsigned long>(status);
  return out.str();
}

void check_status(const char *operation, NTSTATUS status) {
  if (status < 0) {
    throw CryptoError(operation, status);
  }
}

} // namespace

CryptoError::CryptoError(const char *operation, NTSTATUS status)
    : std::runtime_error(status_message(operation, status)), status_(status) {}

void secure_random(void *destination, std::size_t size) {
  if (size == 0) {
    return;
  }
  if (destination == nullptr) {
    throw std::invalid_argument("secure_random: null destination");
  }

  auto *output = static_cast<PUCHAR>(destination);
  /* BCryptGenRandom takes a ULONG length; split larger requests. */
  while (size != 0) {
    const ULONG chunk = static_cast<ULONG>(std::min<std::size_t>(
        size, std::numeric_limits<ULONG>::max()));
    check_status("BCryptGenRandom",
                 BCryptGenRandom(nullptr, output, chunk,
                                 BCRYPT_USE_SYSTEM_PREFERRED_RNG));
    output += chunk;
    size -= chunk;
  }
}

Aes128Ecb::Aes128Ecb(const std::uint8_t key_bytes[key_size]) {
  if (key_bytes == nullptr) {
    throw std::invalid_argument("Aes128Ecb: null key");
  }

  try {
    check_status("BCryptOpenAlgorithmProvider",
                 BCryptOpenAlgorithmProvider(&algorithm_, BCRYPT_AES_ALGORITHM,
                                             nullptr, 0));

    check_status("BCryptSetProperty(BCRYPT_CHAINING_MODE)",
                 BCryptSetProperty(
                     algorithm_, BCRYPT_CHAINING_MODE,
                     reinterpret_cast<PUCHAR>(const_cast<wchar_t *>(
                         BCRYPT_CHAIN_MODE_ECB)),
                     sizeof(BCRYPT_CHAIN_MODE_ECB), 0));

    ULONG bytes_written = 0;
    check_status("BCryptGetProperty(BCRYPT_OBJECT_LENGTH)",
                 BCryptGetProperty(algorithm_, BCRYPT_OBJECT_LENGTH,
                                   reinterpret_cast<PUCHAR>(&key_object_size_),
                                   sizeof(key_object_size_), &bytes_written, 0));
    if (bytes_written != sizeof(key_object_size_) || key_object_size_ == 0) {
      throw std::runtime_error(
          "BCryptGetProperty returned an invalid AES key object size");
    }

    key_object_ = static_cast<PUCHAR>(
        HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, key_object_size_));
    if (key_object_ == nullptr) {
      throw std::bad_alloc();
    }

    check_status("BCryptGenerateSymmetricKey",
                 BCryptGenerateSymmetricKey(
                     algorithm_, &key_, key_object_, key_object_size_,
                     const_cast<PUCHAR>(key_bytes),
                     static_cast<ULONG>(key_size), 0));
  } catch (...) {
    if (key_ != nullptr) {
      BCryptDestroyKey(key_);
      key_ = nullptr;
    }
    if (key_object_ != nullptr) {
      secure_erase(key_object_, key_object_size_);
      HeapFree(GetProcessHeap(), 0, key_object_);
      key_object_ = nullptr;
    }
    if (algorithm_ != nullptr) {
      BCryptCloseAlgorithmProvider(algorithm_, 0);
      algorithm_ = nullptr;
    }
    throw;
  }
}

Aes128Ecb::~Aes128Ecb() {
  if (key_ != nullptr) {
    BCryptDestroyKey(key_);
  }
  if (key_object_ != nullptr) {
    secure_erase(key_object_, key_object_size_);
    HeapFree(GetProcessHeap(), 0, key_object_);
  }
  if (algorithm_ != nullptr) {
    BCryptCloseAlgorithmProvider(algorithm_, 0);
  }
}

void Aes128Ecb::encrypt_blocks(const void *source, void *destination,
                               std::size_t block_count) const {
  crypt_blocks(true, source, destination, block_count);
}

void Aes128Ecb::decrypt_blocks(const void *source, void *destination,
                               std::size_t block_count) const {
  crypt_blocks(false, source, destination, block_count);
}

void Aes128Ecb::crypt_blocks(bool encrypt, const void *source,
                             void *destination,
                             std::size_t block_count) const {
  if (block_count == 0) {
    return;
  }
  if (source == nullptr || destination == nullptr) {
    throw std::invalid_argument("Aes128Ecb: null block buffer");
  }
  if (block_count >
      std::numeric_limits<ULONG>::max() / static_cast<ULONG>(block_size)) {
    throw std::length_error("Aes128Ecb: block buffer is too large");
  }

  const ULONG byte_count =
      static_cast<ULONG>(block_count * static_cast<ULONG>(block_size));
  ULONG bytes_written = 0;
  const NTSTATUS status =
      encrypt
          ? BCryptEncrypt(key_,
                          reinterpret_cast<PUCHAR>(const_cast<void *>(source)),
                          byte_count, nullptr, nullptr, 0,
                          static_cast<PUCHAR>(destination), byte_count,
                          &bytes_written, 0)
          : BCryptDecrypt(key_,
                          reinterpret_cast<PUCHAR>(const_cast<void *>(source)),
                          byte_count, nullptr, nullptr, 0,
                          static_cast<PUCHAR>(destination), byte_count,
                          &bytes_written, 0);
  check_status(encrypt ? "BCryptEncrypt(AES-ECB)"
                       : "BCryptDecrypt(AES-ECB)",
               status);
  if (bytes_written != byte_count) {
    throw std::runtime_error("BCrypt AES operation returned a short result");
  }
}

void secure_erase(void *data, std::size_t size) noexcept {
  if (data != nullptr && size != 0) {
    SecureZeroMemory(data, size);
  }
}

bool lock_secret_memory(void *data, std::size_t size) noexcept {
  return size == 0 || (data != nullptr && VirtualLock(data, size) != FALSE);
}

void unlock_secret_memory(void *data, std::size_t size) noexcept {
  if (data != nullptr && size != 0) {
    secure_erase(data, size);
    VirtualUnlock(data, size);
  }
}

} // namespace mosh::win32
