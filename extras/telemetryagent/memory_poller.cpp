/**
 * @file memory_poller.cpp
 * @brief Memory-based data source for direct game state access
 *
 * Reads game state directly from memory by following pointer chains.
 * Populates protobuf messages from HTTP_Export_GameState_ToJSON @ 0x140155c80
 *
 * Memory layout documentation from ~/src/evr-reconstruction/docs/analysis/
 * Reference implementation: src/gameserver/gameserver.cpp lines 410-539
 */

#include "memory_poller.h"

#include <Windows.h>
#include <google/protobuf/util/json_util.h>

#include <chrono>
#include <cstdint>
#include <iomanip>
#include <sstream>

namespace TelemetryAgent {

MemoryPoller::MemoryPoller(uintptr_t gameBaseAddress)
    : m_gameBaseAddress(gameBaseAddress), m_active(false), m_frameIndex(0) {}

MemoryPoller::~MemoryPoller() { Stop(); }

bool MemoryPoller::Start() {
  std::lock_guard<std::mutex> lock(m_mutex);

  if (m_active) {
    return true;
  }

  // Validate that we can read from the base address
  if (m_gameBaseAddress == 0) {
    return false;
  }

  // Validate memory access
  if (!ValidateMemoryAccess()) {
    return false;
  }

  m_active = true;
  m_frameIndex = 0;
  m_cachedSessionUUID.clear();

  return true;
}

void MemoryPoller::Stop() {
  std::lock_guard<std::mutex> lock(m_mutex);
  m_active = false;
  m_cachedSessionUUID.clear();
}

bool MemoryPoller::IsActive() const { return m_active; }

bool MemoryPoller::GetFrameData(FrameData& data) {
  if (!m_active) {
    return false;
  }

  auto now = std::chrono::system_clock::now();
  auto timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();

  try {
    // Populate session data via protobuf, then serialize to JSON
    engine::v1::SessionResponse sessionProto;
    if (!PopulateSessionResponse(&sessionProto)) {
      data.session.valid = false;
      return false;
    }

    // Serialize protobuf to JSON string
    std::string sessionJson;
    google::protobuf::util::MessageToJsonString(sessionProto, &sessionJson);

    data.session.json = sessionJson;
    data.session.valid = true;
    data.session.timestamp_ms = timestamp;

    // Populate player bones data via protobuf, then serialize to JSON
    engine::v1::PlayerBonesResponse playerBonesProto;
    if (!PopulatePlayerBonesResponse(&playerBonesProto)) {
      data.playerBones.valid = false;
    } else {
      // Serialize protobuf to JSON string
      std::string playerBonesJson;
      google::protobuf::util::MessageToJsonString(playerBonesProto, &playerBonesJson);

      data.playerBones.json = playerBonesJson;
      data.playerBones.valid = true;
      data.playerBones.timestamp_ms = timestamp;
    }

    data.frameIndex = m_frameIndex++;
    return true;
  } catch (...) {
    return false;
  }
}

std::string MemoryPoller::GetSessionUUID() const {
  std::lock_guard<std::mutex> lock(m_mutex);
  return m_cachedSessionUUID;
}

void MemoryPoller::SetOffsets(const MemoryOffsets& offsets) {
  std::lock_guard<std::mutex> lock(m_mutex);
  m_offsets = offsets;
}

std::string MemoryPoller::ReadString(uintptr_t address, size_t maxLength) const {
  if (!IsMemoryReadable(address, maxLength)) {
    return "";
  }

  std::string result;
  result.reserve(maxLength);

  const char* ptr = reinterpret_cast<const char*>(address);
  for (size_t i = 0; i < maxLength && ptr[i] != '\0'; ++i) {
    result += ptr[i];
  }

  return result;
}

bool MemoryPoller::ValidateMemoryAccess() const {
  if (m_gameBaseAddress == 0) {
    return false;
  }

  if (!IsMemoryReadable(m_gameBaseAddress + m_offsets.gameContextBase, sizeof(uintptr_t))) {
    return false;
  }

  uintptr_t contextPtr = *reinterpret_cast<uintptr_t*>(m_gameBaseAddress + m_offsets.gameContextBase);
  if (contextPtr == 0 || !IsMemoryReadable(contextPtr, 64)) {
    return false;
  }

  return true;
}

std::string MemoryPoller::ReadGUID(uintptr_t address) const {
  if (!IsMemoryReadable(address, 16)) {
    return "";
  }

  const GUID* guid = reinterpret_cast<const GUID*>(address);

  std::ostringstream ss;
  ss << std::hex << std::setfill('0') << std::setw(8) << guid->Data1 << '-' << std::setw(4) << guid->Data2 << '-'
     << std::setw(4) << guid->Data3 << '-' << std::setw(2) << static_cast<int>(guid->Data4[0]) << std::setw(2)
     << static_cast<int>(guid->Data4[1]) << '-' << std::setw(2) << static_cast<int>(guid->Data4[2]) << std::setw(2)
     << static_cast<int>(guid->Data4[3]) << std::setw(2) << static_cast<int>(guid->Data4[4]) << std::setw(2)
     << static_cast<int>(guid->Data4[5]) << std::setw(2) << static_cast<int>(guid->Data4[6]) << std::setw(2)
     << static_cast<int>(guid->Data4[7]);

  return ss.str();
}

bool MemoryPoller::PopulateSessionResponse(engine::v1::SessionResponse* response) const {
  if (m_gameBaseAddress == 0 || response == nullptr) {
    return false;
  }

  CHAR* baseAddr = reinterpret_cast<CHAR*>(m_gameBaseAddress);
  VOID** contextPtr = reinterpret_cast<VOID**>(baseAddr + m_offsets.gameContextBase);

  if (!IsMemoryReadable(reinterpret_cast<uintptr_t>(contextPtr), sizeof(VOID*))) {
    return false;
  }

  VOID* gameContext = *contextPtr;
  if (gameContext == 0) {
    return false;
  }

  CHAR* contextBase = reinterpret_cast<CHAR*>(gameContext);

  VOID** lobbyPtr = reinterpret_cast<VOID**>(contextBase + m_offsets.lobbyOffset);
  VOID** gameStatePtr = reinterpret_cast<VOID**>(contextBase + m_offsets.gameStateOffset);

  if (!IsMemoryReadable(reinterpret_cast<uintptr_t>(lobbyPtr), sizeof(VOID*)) ||
      !IsMemoryReadable(reinterpret_cast<uintptr_t>(gameStatePtr), sizeof(VOID*))) {
    return false;
  }

  CHAR* lobby = reinterpret_cast<CHAR*>(*lobbyPtr);
  CHAR* gameState = reinterpret_cast<CHAR*>(*gameStatePtr);

  if (lobby == nullptr || gameState == nullptr) {
    return false;
  }

  response->set_session_id("00000000-0000-0000-0000-000000000000");

  // Placeholder: game_status requires hash lookup via FUN_140d5ced0
  response->set_game_status("pre_match");

  // game_clock from gameState+0x0 (or from hash via FUN_140d36750)
  if (IsMemoryReadable(reinterpret_cast<uintptr_t>(gameState + 0x0), 4)) {
    float* gameClockPtr = reinterpret_cast<float*>(gameState + 0x0);
    float gameClock = *gameClockPtr;
    response->set_game_clock(gameClock);

    // Format as MM:SS.DD
    int minutes = static_cast<int>(gameClock) / 60;
    int seconds = static_cast<int>(gameClock) % 60;
    int centiseconds = static_cast<int>((gameClock - static_cast<int>(gameClock)) * 100);
    char clockDisplay[16] = {0};
    snprintf(clockDisplay, sizeof(clockDisplay), "%02d:%02d.%02d", minutes, seconds, centiseconds);
    response->set_game_clock_display(clockDisplay);
  } else {
    response->set_game_clock(0.0f);
    response->set_game_clock_display("00:00.00");
  }

  // Placeholder: blue_points and orange_points require hash lookup
  // Via FUN_140d05730(session, hash) or direct offset reads
  response->set_blue_points(0);
  response->set_orange_points(0);

  auto* disc = response->mutable_disc();
  if (IsMemoryReadable(reinterpret_cast<uintptr_t>(gameState + 0xf4), 12)) {
    float* pos = reinterpret_cast<float*>(gameState + 0xf4);
    disc->add_position(pos[0]);
    disc->add_position(pos[1]);
    disc->add_position(pos[2]);
  } else {
    disc->add_position(0.0f);
    disc->add_position(0.0f);
    disc->add_position(0.0f);
  }

  if (IsMemoryReadable(reinterpret_cast<uintptr_t>(gameState + 0x104), 12)) {
    float* vel = reinterpret_cast<float*>(gameState + 0x104);
    disc->add_velocity(vel[0]);
    disc->add_velocity(vel[1]);
    disc->add_velocity(vel[2]);
  } else {
    disc->add_velocity(0.0f);
    disc->add_velocity(0.0f);
    disc->add_velocity(0.0f);
  }

  // Convert quaternion to forward/up/left vectors
  if (IsMemoryReadable(reinterpret_cast<uintptr_t>(gameState + 0xe4), 16)) {
    float* quat = reinterpret_cast<float*>(gameState + 0xe4);
    float qx = quat[0], qy = quat[1], qz = quat[2], qw = quat[3];

    float fVar33 = qx * 2.0f;
    float fVar34 = qy * 2.0f;
    float fVar37 = qz * 2.0f;

    // Forward vector
    disc->add_forward(qz * fVar34 + qw * fVar33);
    disc->add_forward(qz * fVar33 - (qw * fVar34));
    disc->add_forward(1.0f - (qy * fVar34 + qx * fVar33));

    // Up vector
    disc->add_up(1.0f - (qz * fVar37 + qx * fVar33));
    disc->add_up(qy * fVar33 + qw * fVar37);
    disc->add_up(qy * fVar37 - qw * fVar33);

    // Left vector
    disc->add_left(fVar37 * qx + (qw * fVar34));
    disc->add_left(fVar34 * qx - qw * fVar37);
    disc->add_left(1.0f - (qz * fVar37 + qy * fVar34));
  } else {
    // Default identity orientation
    disc->add_forward(0.0f);
    disc->add_forward(0.0f);
    disc->add_forward(1.0f);

    disc->add_up(0.0f);
    disc->add_up(1.0f);
    disc->add_up(0.0f);

    disc->add_left(-1.0f);
    disc->add_left(0.0f);
    disc->add_left(0.0f);
  }

  if (IsMemoryReadable(reinterpret_cast<uintptr_t>(gameState + 0x158), 8)) {
    uint64_t* bounceCount = reinterpret_cast<uint64_t*>(gameState + 0x158);
    disc->set_bounce_count(*bounceCount);
  } else {
    disc->set_bounce_count(0);
  }

  uint16_t playerCount = 0;
  if (IsMemoryReadable(reinterpret_cast<uintptr_t>(lobby + 0xe2), 2)) {
    playerCount = *reinterpret_cast<uint16_t*>(lobby + 0xe2);
  }

  struct TeamData {
    std::vector<uint16_t> playerSlots;
    std::string teamName;
  };

  TeamData teams[3];
  teams[0].teamName = "blue";
  teams[1].teamName = "orange";
  teams[2].teamName = "spectator";

  for (uint16_t slot = 0; slot < playerCount && slot < 16; ++slot) {
    CHAR* playerBase = lobby + 0x178 + (slot * 0x250);

    if (!IsMemoryReadable(reinterpret_cast<uintptr_t>(playerBase), 2)) {
      continue;
    }

    uint16_t playerFlags = *reinterpret_cast<uint16_t*>(playerBase);
    uint8_t teamId = (playerFlags >> 10) & 7;

    if (teamId >= 5) {
      teamId = 0xff;
    }

    if (teamId < 3) {
      teams[teamId].playerSlots.push_back(slot);
    }
  }

  for (int teamIdx = 0; teamIdx < 3; ++teamIdx) {
    auto* team = response->add_teams();
    team->set_has_possession(false);

    for (uint16_t slot : teams[teamIdx].playerSlots) {
      auto* player = team->add_players();

      // Read EntrantData from Lobby+0x178+(slot*0x250)
      CHAR* entrantBase = lobby + 0x178 + (slot * 0x250);

      player->set_slot_number(static_cast<int32_t>(slot));

      // userId.accountId at +0x08
      if (IsMemoryReadable(reinterpret_cast<uintptr_t>(entrantBase + 0x08), 8)) {
        uint64_t* accountId = reinterpret_cast<uint64_t*>(entrantBase + 0x08);
        player->set_account_number(static_cast<int64_t>(*accountId));
      } else {
        player->set_account_number(0);
      }

      // displayName at +0x3C (36 bytes)
      if (IsMemoryReadable(reinterpret_cast<uintptr_t>(entrantBase + 0x3C), 36)) {
        char* displayName = reinterpret_cast<char*>(entrantBase + 0x3C);
        std::string name(displayName, strnlen(displayName, 35));
        player->set_display_name(name);
      } else {
        player->set_display_name("Player_" + std::to_string(slot));
      }

      player->set_jersey_number(0);
      player->set_level(0);

      // ping at +0x8A (uint16_t)
      if (IsMemoryReadable(reinterpret_cast<uintptr_t>(entrantBase + 0x8A), 2)) {
        uint16_t* ping = reinterpret_cast<uint16_t*>(entrantBase + 0x8A);
        player->set_ping(*ping);
      } else {
        player->set_ping(0);
      }

      player->set_packet_loss_ratio(0.0f);
      player->set_has_possession(false);
      player->set_is_stunned(false);
      player->set_is_invulnerable(false);
      player->set_is_blocking(false);
      player->set_is_emote_playing(false);

      CHAR* statsBase = lobby + 0x72ac + (slot * 0x4d08);
      auto* stats = player->mutable_stats();

      bool statsReadable = IsMemoryReadable(reinterpret_cast<uintptr_t>(statsBase), 0x100) &&
                           IsMemoryReadable(reinterpret_cast<uintptr_t>(statsBase + 0x300), 4);

      if (!statsReadable) {
        stats->set_possession_time(0.0);
        stats->set_points(0);
        stats->set_goals(0);
        stats->set_stuns(0);
        stats->set_blocks(0);
        stats->set_assists(0);
        stats->set_saves(0);
        stats->set_interceptions(0);
        stats->set_steals(0);
        stats->set_catches(0);
        stats->set_passes(0);
        stats->set_shots_taken(0);
      } else {
        // Helper lambda to read stat value (handles type field for averaging)
        auto readStat = [this, statsBase](uint32_t typeOffset, uint32_t countOffset, uint32_t valueOffset) -> double {
          if (!IsMemoryReadable(reinterpret_cast<uintptr_t>(statsBase + typeOffset), 4)) {
            return 0.0;
          }
          uint32_t* typePtr = reinterpret_cast<uint32_t*>(statsBase + typeOffset);
          uint32_t type = *typePtr;

          if (!IsMemoryReadable(reinterpret_cast<uintptr_t>(statsBase + countOffset), 4)) {
            return 0.0;
          }
          uint32_t* countPtr = reinterpret_cast<uint32_t*>(statsBase + countOffset);
          uint32_t count = *countPtr;

          if (!IsMemoryReadable(reinterpret_cast<uintptr_t>(statsBase + valueOffset), 8)) {
            return 0.0;
          }
          double* valuePtr = reinterpret_cast<double*>(statsBase + valueOffset);
          double value = *valuePtr;

          // Type 5 = averaged stat (divide by count)
          if (type == 5 && count > 0) {
            return value / count;
          }
          return value;
        };

        // Read all 12 stats
        stats->set_possession_time(readStat(0x300, 0x304, 0x308));
        stats->set_points(static_cast<int32_t>(readStat(0x0, 0x4, 0x8)));
        stats->set_goals(static_cast<int32_t>(readStat(0x10, 0x14, 0x18)));
        stats->set_stuns(static_cast<int32_t>(readStat(0x30, 0x34, 0x38)));
        stats->set_blocks(static_cast<int32_t>(readStat(0x40, 0x44, 0x48)));
        stats->set_assists(static_cast<int32_t>(readStat(0x20, 0x24, 0x28)));
        stats->set_saves(static_cast<int32_t>(readStat(0x50, 0x54, 0x58)));
        stats->set_interceptions(static_cast<int32_t>(readStat(0x60, 0x64, 0x68)));
        stats->set_steals(static_cast<int32_t>(readStat(0x70, 0x74, 0x78)));
        stats->set_catches(static_cast<int32_t>(readStat(0x80, 0x84, 0x88)));
        stats->set_passes(static_cast<int32_t>(readStat(0x90, 0x94, 0x98)));
        stats->set_shots_taken(static_cast<int32_t>(readStat(0xa0, 0xa4, 0xa8)));
      }
    }
  }

  response->add_possession(-1);
  response->add_possession(-1);
  response->set_blue_team_restart_request(false);
  response->set_orange_team_restart_request(false);

  return true;
}

bool MemoryPoller::PopulatePlayerBonesResponse(engine::v1::PlayerBonesResponse* response) const {
  if (m_gameBaseAddress == 0 || response == nullptr) {
    return false;
  }

  response->set_err_code(0);

  return true;
}

bool MemoryPoller::IsMemoryReadable(uintptr_t address, size_t size) const {
  if (address == 0) {
    return false;
  }

  // Use VirtualQuery to check if memory is readable
  MEMORY_BASIC_INFORMATION mbi;
  if (VirtualQuery(reinterpret_cast<LPCVOID>(address), &mbi, sizeof(mbi)) == 0) {
    return false;
  }

  // Check if the memory is committed and readable
  if (mbi.State != MEM_COMMIT) {
    return false;
  }

  // Check if we have read access
  DWORD protect = mbi.Protect;
  if (protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE |
                 PAGE_EXECUTE_WRITECOPY)) {
    // Check if the entire region we want to read is within this memory block
    uintptr_t regionEnd = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
    return (address + size) <= regionEnd;
  }

  return false;
}

}  // namespace TelemetryAgent
