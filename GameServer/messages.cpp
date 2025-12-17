#include "messages.h"
/**
 * Encodes a GameServerSessionStartInternalMessage into a binary buffer
 *
 * @param message The message to encode
 * @return A newly allocated buffer containing the binary data
 *         Caller is responsible for freeing this memory
 */
CHAR* EncodeLobbySessionStartV4(const GUID& lobbySessionId, const GUID& groupId, BYTE playerLimit, BYTE entrantCount,
                                BYTE lobbyType, BYTE pad1, const CHAR* settingsJson) {
  // Calculate required buffer size
  size_t settingsJsonLen = strlen(settingsJson) + 1;  // Include null terminator

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
  memcpy(buffer + offset, &lobbySessionId, sizeof(GUID));
  offset += sizeof(GUID);

  memcpy(buffer + offset, &groupId, sizeof(GUID));
  offset += sizeof(GUID);

  buffer[offset++] = playerLimit;
  buffer[offset++] = entrantCount;
  buffer[offset++] = lobbyType;
  buffer[offset++] = pad1;

  // Copy the settings JSON string including null terminator
  memcpy(buffer + offset, settingsJson, settingsJsonLen);

  return buffer;
}

// TODO: Implement this function
CHAR* EncodeLobbySessionSuccessV5() { return nullptr; }