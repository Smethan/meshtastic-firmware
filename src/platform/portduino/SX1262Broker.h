#pragma once

#if defined(MESHTASTIC_WDG_API) && defined(__linux__)

namespace meshtastic::portduino
{

inline constexpr int SX1262_BROKER_API_MAJOR = 1;
inline constexpr int SX1262_BROKER_API_MINOR = 0;
inline constexpr int SX1262_BROKER_MAX_PACKET = 4096;
inline constexpr int SX1262_BROKER_MAX_PAYLOAD = 255;

/// Run the single-owner SX1262 service. This function does not return during
/// normal operation and is entered from portduinoSetup after GPIO/SPI binding.
int runSX1262Broker(const char *socketPath, const char *forcedOffPath, bool fakeRadio);

} // namespace meshtastic::portduino

#endif
