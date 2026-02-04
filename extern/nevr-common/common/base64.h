#pragma once

#include <cstdint>
#include <string>

/**
 * @brief Encodes binary data to base64 string
 * @param data Pointer to the data to encode
 * @param len Length of the data in bytes
 * @return Base64 encoded string
 */
std::string base64_encode(const uint8_t* data, size_t len);

/**
 * @brief Decodes a base64 string to binary data
 * @param encoded_string The base64 encoded string
 * @return Decoded binary data as a string
 */
std::string base64_decode(const std::string& encoded_string);
