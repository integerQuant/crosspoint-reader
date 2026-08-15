#pragma once

#include <Arduino.h>
#include <NetworkClient.h>
#include <NetworkServer.h>
#include <NetworkUdp.h>

#include <cstddef>
#include <cstdint>

class TerminalWifi {
 public:
  enum class State : uint8_t { Offline, Waiting, Negotiating, ApprovalPending, Connected };

  static constexpr uint16_t PORT = 29380;

  void begin();
  void end();
  void poll();

  int available();
  int read();
  size_t write(uint8_t byte);
  size_t write(const uint8_t* data, size_t length);

  void acceptRequest(uint8_t columns, uint8_t rows);
  void denyRequest();
  bool formatHostTime(char* buffer, size_t bufferSize) const;

  State getState() const { return state; }
  bool isConnected() { return state == State::Connected && client.connected(); }
  const char* getClientName() const { return clientName; }
  const char* getClientIp() const { return clientIp; }
  const char* getHostname() const { return hostname; }
  const char* getLocalIp() const { return localIp; }
  uint32_t getGeneration() const { return generation; }

 private:
  static constexpr uint32_t HELLO_TIMEOUT_MS = 5000;
  static constexpr size_t HELLO_BUFFER_SIZE = 96;
  static constexpr size_t CLIENT_NAME_SIZE = 33;

  NetworkServer server{PORT, 1};
  NetworkClient client;
  NetworkUDP discovery;
  State state = State::Offline;
  bool active = false;
  bool serviceStarted = false;
  bool mdnsStarted = false;
  bool discoveryStarted = false;
  uint32_t helloDeadline = 0;
  uint32_t generation = 0;
  size_t helloLength = 0;
  char helloBuffer[HELLO_BUFFER_SIZE]{};
  char clientName[CLIENT_NAME_SIZE]{};
  char clientIp[16]{};
  char hostname[32]{};
  char localIp[16]{};
  uint64_t hostEpochSeconds = 0;
  int16_t hostUtcOffsetMinutes = 0;
  uint32_t hostTimeCapturedAt = 0;
  uint8_t helloVersion = 1;
  bool hasHostTime = false;

  void setState(State next);
  void startService();
  void stopService();
  void disconnectClient();
  void acceptIncoming();
  void pollDiscovery();
  void pollHandshake();
  bool parseHello();
  void rejectIncoming(NetworkClient& incoming, const char* response);
};
