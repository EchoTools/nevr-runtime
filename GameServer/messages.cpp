#include "messages.h"
/**
 * Encodes a GameServerSessionStartInternalMessage into a binary buffer
 *
 * @param message The message to encode
 * @return A newly allocated buffer containing the binary data
 *         Caller is responsible for freeing this memory
 */
CHAR* EncodeSessionStartMessage(const GameServerSessionStartInternalMessage& message) {
  // Calculate required buffer size
  size_t settingsJsonLen = strlen(message.SettingsJson) + 1;  // Include null terminator

  // Calculate total buffer size with all fields concatenated
  size_t bufferSize = sizeof(GUID) +    // LobbySesssionId
                      sizeof(GUID) +    // GroupId
                      sizeof(BYTE) +    // PlayerLimit
                      sizeof(BYTE) +    // EntrantCount
                      sizeof(BYTE) +    // LobbyType
                      sizeof(BYTE) +    // Pad1
                      settingsJsonLen;  // SettingsJson with null terminator

  // Allocate buffer for the serialized data
  CHAR* buffer = new CHAR[bufferSize];
  memset(buffer, 0, bufferSize);

  // Track current position in buffer
  size_t offset = 0;

  // Copy each field one by one
  memcpy(buffer + offset, &message.LobbySesssionId, sizeof(GUID));
  offset += sizeof(GUID);

  memcpy(buffer + offset, &message.GroupId, sizeof(GUID));
  offset += sizeof(GUID);

  buffer[offset++] = message.PlayerLimit;
  buffer[offset++] = message.EntrantCount;
  buffer[offset++] = message.LobbyType;
  buffer[offset++] = message.Pad1;

  // Copy the settings JSON string including null terminator
  memcpy(buffer + offset, message.SettingsJson, settingsJsonLen);

  return buffer;
}