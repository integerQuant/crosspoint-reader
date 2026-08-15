#include "TerminalWifi.h"

#include <ESPmDNS.h>
#include <Logging.h>
#include <WiFi.h>
#include <esp_mac.h>

#include <cctype>
#include <cstdlib>
#include <cstdio>
#include <cstring>

namespace {

constexpr char HELLO_V1_PREFIX[] = "KNIETTY/1 HELLO ";
constexpr char HELLO_V2_PREFIX[] = "KNIETTY/2 HELLO ";
constexpr char RESPONSE_ACCEPT_FORMAT[] = "KNIETTY/%u ACCEPT %u %u\n";
constexpr char RESPONSE_DENY[] = "KNIETTY/1 DENY\n";
constexpr char RESPONSE_BUSY[] = "KNIETTY/1 BUSY\n";
constexpr char RESPONSE_ERROR[] = "KNIETTY/1 ERROR\n";
constexpr char DISCOVERY_REQUEST[] = "KNIETTY/1 DISCOVER";
constexpr char DISCOVERY_RESPONSE_FORMAT[] = "KNIETTY/1 HERE %s %u\n";

}  // namespace

void TerminalWifi::setState(const State next) {
  if (state == next) return;
  state = next;
  ++generation;
}

void TerminalWifi::begin() {
  active = true;
  WiFi.setAutoReconnect(true);
  WiFi.setSleep(false);

  uint8_t mac[6]{};
  if (esp_read_mac(mac, ESP_MAC_WIFI_STA) == ESP_OK) {
    std::snprintf(hostname, sizeof(hostname), "knietty-%02x%02x%02x", mac[3], mac[4], mac[5]);
  } else {
    std::snprintf(hostname, sizeof(hostname), "knietty-x4");
  }

  poll();
}

void TerminalWifi::end() {
  active = false;
  stopService();
  WiFi.setSleep(true);
}

void TerminalWifi::startService() {
  if (serviceStarted || WiFi.status() != WL_CONNECTED) return;

  server.begin();
  server.setNoDelay(true);
  serviceStarted = static_cast<bool>(server);
  if (!serviceStarted) {
    setState(State::Offline);
    LOG_ERR("KNIETTY", "Could not start TCP server on port %u", PORT);
    return;
  }

  MDNS.end();
  mdnsStarted = MDNS.begin(hostname);
  if (mdnsStarted) {
    MDNS.addService("knietty", "tcp", PORT);
    MDNS.addServiceTxt("knietty", "tcp", "proto", "2");
    MDNS.addServiceTxt("knietty", "tcp", "id", static_cast<const char*>(hostname));
    MDNS.addServiceTxt("knietty", "tcp", "cols", "80");
    MDNS.addServiceTxt("knietty", "tcp", "rows", "24");
    MDNS.addServiceTxt("knietty", "tcp", "approval", "required");
  } else {
    LOG_ERR("KNIETTY", "Could not start mDNS; explicit IP connections remain available");
  }
  discoveryStarted = discovery.begin(PORT) != 0;
  if (!discoveryStarted) LOG_ERR("KNIETTY", "Could not start UDP discovery on port %u", PORT);

  setState(State::Waiting);
}

void TerminalWifi::stopService() {
  disconnectClient();
  if (serviceStarted) {
    server.end();
    serviceStarted = false;
  }
  if (mdnsStarted) {
    MDNS.end();
    mdnsStarted = false;
  }
  if (discoveryStarted) {
    discovery.stop();
    discoveryStarted = false;
  }
  setState(State::Offline);
}

void TerminalWifi::disconnectClient() {
  if (client) client.stop();
  client = NetworkClient{};
  helloLength = 0;
  helloBuffer[0] = '\0';
  clientName[0] = '\0';
  clientIp[0] = '\0';
  hostEpochSeconds = 0;
  hostUtcOffsetMinutes = 0;
  hostTimeCapturedAt = 0;
  helloVersion = 1;
  hasHostTime = false;
}

void TerminalWifi::rejectIncoming(NetworkClient& incoming, const char* response) {
  incoming.setNoDelay(true);
  incoming.write(reinterpret_cast<const uint8_t*>(response), std::strlen(response));
  incoming.stop();
}

void TerminalWifi::acceptIncoming() {
  NetworkClient incoming = server.accept();
  if (!incoming) return;

  if (state != State::Waiting) {
    rejectIncoming(incoming, RESPONSE_BUSY);
    return;
  }

  client = incoming;
  client.setNoDelay(true);
  const IPAddress remote = client.remoteIP();
  std::snprintf(clientIp, sizeof(clientIp), "%u.%u.%u.%u", remote[0], remote[1], remote[2], remote[3]);
  helloLength = 0;
  helloBuffer[0] = '\0';
  helloDeadline = millis() + HELLO_TIMEOUT_MS;
  setState(State::Negotiating);
}

void TerminalWifi::pollDiscovery() {
  if (!discoveryStarted || discovery.parsePacket() <= 0) return;

  char request[32];
  int length = discovery.read(request, sizeof(request) - 1);
  if (length <= 0) return;
  request[length] = '\0';
  while (length > 0 && (request[length - 1] == '\r' || request[length - 1] == '\n')) {
    request[--length] = '\0';
  }
  if (std::strcmp(request, DISCOVERY_REQUEST) != 0) return;

  char response[64];
  const int responseLength = std::snprintf(response, sizeof(response), DISCOVERY_RESPONSE_FORMAT, hostname, PORT);
  if (responseLength <= 0 || responseLength >= static_cast<int>(sizeof(response))) return;
  if (!discovery.beginPacket(discovery.remoteIP(), discovery.remotePort())) return;
  discovery.write(reinterpret_cast<const uint8_t*>(response), static_cast<size_t>(responseLength));
  discovery.endPacket();
}

bool TerminalWifi::parseHello() {
  const char* nameStart = nullptr;
  if (std::strncmp(helloBuffer, HELLO_V2_PREFIX, sizeof(HELLO_V2_PREFIX) - 1) == 0) {
    helloVersion = 2;
    char* cursor = helloBuffer + sizeof(HELLO_V2_PREFIX) - 1;
    char* end = nullptr;
    const unsigned long long epoch = std::strtoull(cursor, &end, 10);
    if (end == cursor || *end != ' ') return false;
    cursor = end + 1;
    const long offset = std::strtol(cursor, &end, 10);
    if (end == cursor || *end != ' ' || epoch < 946684800ULL || epoch > 4102444800ULL || offset < -840 ||
        offset > 840) {
      return false;
    }
    nameStart = end + 1;
    hostEpochSeconds = static_cast<uint64_t>(epoch);
    hostUtcOffsetMinutes = static_cast<int16_t>(offset);
    hostTimeCapturedAt = millis();
    hasHostTime = true;
  } else if (std::strncmp(helloBuffer, HELLO_V1_PREFIX, sizeof(HELLO_V1_PREFIX) - 1) == 0) {
    helloVersion = 1;
    nameStart = helloBuffer + sizeof(HELLO_V1_PREFIX) - 1;
    hasHostTime = false;
  } else {
    return false;
  }

  size_t output = 0;
  for (const char* input = nameStart; *input != '\0' && output + 1 < sizeof(clientName); ++input) {
    const unsigned char byte = static_cast<unsigned char>(*input);
    if (byte == '\r' || byte == '\n') break;
    if (std::isalnum(byte) || byte == ' ' || byte == '-' || byte == '_' || byte == '.') {
      clientName[output++] = static_cast<char>(byte);
    } else {
      clientName[output++] = '?';
    }
  }
  clientName[output] = '\0';
  return output != 0;
}

void TerminalWifi::pollHandshake() {
  while (client.available() > 0 && helloLength + 1 < sizeof(helloBuffer)) {
    const int byte = client.read();
    if (byte < 0) break;
    helloBuffer[helloLength++] = static_cast<char>(byte);
    helloBuffer[helloLength] = '\0';
    if (byte == '\n') {
      if (parseHello()) {
        setState(State::ApprovalPending);
      } else {
        client.write(reinterpret_cast<const uint8_t*>(RESPONSE_ERROR), sizeof(RESPONSE_ERROR) - 1);
        disconnectClient();
        setState(State::Waiting);
      }
      return;
    }
  }

  if (helloLength + 1 >= sizeof(helloBuffer) || static_cast<int32_t>(millis() - helloDeadline) >= 0) {
    client.write(reinterpret_cast<const uint8_t*>(RESPONSE_ERROR), sizeof(RESPONSE_ERROR) - 1);
    disconnectClient();
    setState(State::Waiting);
  }
}

void TerminalWifi::poll() {
  if (!active) return;
  if (WiFi.status() != WL_CONNECTED) {
    if (serviceStarted) stopService();
    return;
  }
  if (!serviceStarted) startService();
  if (!serviceStarted) return;

  if (state != State::Waiting && !client.connected()) {
    disconnectClient();
    setState(State::Waiting);
  }

  acceptIncoming();
  pollDiscovery();
  if (state == State::Negotiating) pollHandshake();
}

int TerminalWifi::available() {
  return isConnected() ? client.available() : 0;
}

int TerminalWifi::read() {
  return isConnected() ? client.read() : -1;
}

size_t TerminalWifi::write(const uint8_t byte) { return write(&byte, 1); }

size_t TerminalWifi::write(const uint8_t* data, const size_t length) {
  return isConnected() ? client.write(data, length) : 0;
}

void TerminalWifi::acceptRequest(const uint8_t columns, const uint8_t rows) {
  if (state != State::ApprovalPending || !client.connected()) return;
  char response[40];
  const int length = std::snprintf(response, sizeof(response), RESPONSE_ACCEPT_FORMAT, helloVersion, columns, rows);
  if (length <= 0 || client.write(reinterpret_cast<const uint8_t*>(response), static_cast<size_t>(length)) == 0) {
    disconnectClient();
    setState(State::Waiting);
    return;
  }
  setState(State::Connected);
}

bool TerminalWifi::formatHostTime(char* buffer, const size_t bufferSize) const {
  if (!hasHostTime || buffer == nullptr || bufferSize < 6) return false;
  const uint64_t elapsed = static_cast<uint32_t>(millis() - hostTimeCapturedAt) / 1000ULL;
  int64_t localSeconds = static_cast<int64_t>(hostEpochSeconds + elapsed) +
                         static_cast<int64_t>(hostUtcOffsetMinutes) * 60;
  localSeconds %= 86400;
  if (localSeconds < 0) localSeconds += 86400;
  const unsigned hour = static_cast<unsigned>(localSeconds / 3600);
  const unsigned minute = static_cast<unsigned>((localSeconds / 60) % 60);
  std::snprintf(buffer, bufferSize, "%02u:%02u", hour, minute);
  return true;
}

void TerminalWifi::denyRequest() {
  if (state != State::ApprovalPending) return;
  client.write(reinterpret_cast<const uint8_t*>(RESPONSE_DENY), sizeof(RESPONSE_DENY) - 1);
  disconnectClient();
  setState(State::Waiting);
}
