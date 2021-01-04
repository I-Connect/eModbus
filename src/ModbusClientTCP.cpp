// =================================================================================================
// eModbus: Copyright 2020 by Michael Harwerth, Bert Melis and the contributors to eModbus
//               MIT license - see license.md for details
// =================================================================================================
#include "ModbusClientTCP.h"

#if HAS_FREERTOS

#undef LOCAL_LOG_LEVEL
// #define LOCAL_LOG_LEVEL LOG_LEVEL_VERBOSE
#include "Logging.h"

// Constructor takes reference to Client (EthernetClient or WiFiClient)
ModbusClientTCP::ModbusClientTCP(Client& client, uint16_t queueLimit) :
  ModbusClient(),
  MT_client(client),
  MT_lastTarget(IPAddress(0, 0, 0, 0), 0, DEFAULTTIMEOUT, TARGETHOSTINTERVAL),
  MT_target(IPAddress(0, 0, 0, 0), 0, DEFAULTTIMEOUT, TARGETHOSTINTERVAL),
  MT_defaultTimeout(DEFAULTTIMEOUT),
  MT_defaultInterval(TARGETHOSTINTERVAL),
  MT_qLimit(queueLimit)
  { }

// Alternative Constructor takes reference to Client (EthernetClient or WiFiClient) plus initial target host
ModbusClientTCP::ModbusClientTCP(Client& client, IPAddress host, uint16_t port, uint16_t queueLimit) :
  ModbusClient(),
  MT_client(client),
  MT_lastTarget(IPAddress(0, 0, 0, 0), 0, DEFAULTTIMEOUT, TARGETHOSTINTERVAL),
  MT_target(host, port, DEFAULTTIMEOUT, TARGETHOSTINTERVAL),
  MT_defaultTimeout(DEFAULTTIMEOUT),
  MT_defaultInterval(TARGETHOSTINTERVAL),
  MT_qLimit(queueLimit)
  { }

// Destructor: clean up queue, task etc.
ModbusClientTCP::~ModbusClientTCP() {
  // Clean up queue
  {
    // Safely lock access
    #if USE_MUTEX
    lock_guard<mutex> lockGuard(qLock);
    #endif
    // Get all queue entries one by one
    while (!requests.empty()) {
      requests.pop();
    }
  }
  LOG_D("TCP client worker killed.\n");
  // Kill task
  vTaskDelete(worker);
}

// begin: start worker task
void ModbusClientTCP::begin(int coreID) {
  // Create unique task name
  char taskName[12];
  snprintf(taskName, 12, "Modbus%02XTCP", instanceCounter);
  // Start task to handle the queue
  xTaskCreatePinnedToCore((TaskFunction_t)&handleConnection, taskName, 4096, this, 5, &worker, coreID >= 0 ? coreID : NULL);
  LOG_D("TCP client worker %s started\n", taskName);
}

// Set default timeout value (and interval)
void ModbusClientTCP::setTimeout(uint32_t timeout, uint32_t interval) {
  MT_defaultTimeout = timeout;
  MT_defaultInterval = interval;
}

// Switch target host (if necessary)
// Return true, if host/port is different from last host/port used
bool ModbusClientTCP::setTarget(IPAddress host, uint16_t port, uint32_t timeout, uint32_t interval) {
  MT_target.host = host;
  MT_target.port = port;
  MT_target.timeout = timeout ? timeout : MT_defaultTimeout;
  MT_target.interval = interval ? interval : MT_defaultInterval;
  LOG_D("Target set: %d.%d.%d.%d:%d\n", host[0], host[1], host[2], host[3], port);
  if (MT_target.host == MT_lastTarget.host && MT_target.port == MT_lastTarget.port) return false;
  return true;
}

// Base addRequest for preformatted ModbusMessage and last set target
Error ModbusClientTCP::addRequest(ModbusMessage msg, uint32_t token) {
  Error rc = SUCCESS;        // Return value

  // Add it to the queue, if valid
  if (msg) {
    // Queue add successful?
    if (!addToQueue(token, msg, MT_target)) {
      // No. Return error after deleting the allocated request.
      rc = REQUEST_QUEUE_FULL;
    }
  }

  LOG_D("Add TCP request result: %02X\n", rc);
  return rc;
}

// addToQueue: send freshly created request to queue
bool ModbusClientTCP::addToQueue(uint32_t token, ModbusMessage request, TargetHost target) {
  bool rc = false;
  // Did we get one?
  LOG_D("Queue size: %d\n", requests.size());
  HEXDUMP_V("Enqueue", request.data(), request.size());
  if (request) {
    if (requests.size()<MT_qLimit) {
      RequestEntry *re = new RequestEntry(token, request, target);
      // inject proper transactionID
      re->head.transactionID = messageCount++;
      re->head.len = request.size();
      // Safely lock queue and push request to queue
      rc = true;
      #if USE_MUTEX
      lock_guard<mutex> lockGuard(qLock);
      #endif
      requests.push(re);
    }
  }

  return rc;
}

// handleConnection: worker task
// This was created in begin() to handle the queue entries
void ModbusClientTCP::handleConnection(ModbusClientTCP *instance) {
  const uint8_t RETRIES(2);
  uint8_t retryCounter = RETRIES;
  bool doNotPop;
  uint32_t lastRequest = millis();

  // Loop forever - or until task is killed
  while (1) {
    // Do we have a request in queue?
    if (!instance->requests.empty()) {
      // Yes. pull it.
      RequestEntry *request = instance->requests.front();
      doNotPop = false;
      LOG_D("Got request from queue\n");

      // Do we have a connection open?
      if (instance->MT_client.connected()) {
        // check if lastHost/lastPort!=host/port off the queued request
        if (instance->MT_lastTarget != request->target) {
          // It is different. Disconnect it.
          instance->MT_client.stop();
          LOG_D("Target different, disconnect\n");
          delay(1);  // Give scheduler room to breathe
        } else {
          // it is the same host/port.
          // Empty the RX buffer in case there is a stray response left
          if (instance->MT_client.connected()) {
            while (instance->MT_client.available()) { instance->MT_client.read(); }
          }
          // Give it some slack to get ready again
          while (millis() - lastRequest < request->target.interval) { delay(1); }
        }
      }
      // if client is disconnected (we will have to switch hosts)
      if (!instance->MT_client.connected()) {
        // Serial.println("Client reconnecting");
        // It is disconnected. connect to host/port from queue
        instance->MT_client.connect(request->target.host, request->target.port);
        LOG_D("Target connect (%d.%d.%d.%d:%d).\n", request->target.host[0], request->target.host[1], request->target.host[2], request->target.host[3], request->target.port);

        delay(1);  // Give scheduler room to breathe
      }
      // Are we connected (again)?
      if (instance->MT_client.connected()) {
        LOG_D("Is connected. Send request.\n");
        // Yes. Send the request via IP
        instance->send(request);

        // Get the response - if any
        ModbusMessage response = instance->receive(request);

        // Did we get a normal response?
        if (response.getError()==SUCCESS) {
          // Yes. Do we have an onData handler registered?
          LOG_D("Data response.\n");
          if (instance->onData) {
            // Yes. call it
            instance->onData(response, request->token);
          } else {
            LOG_D("No onData handler\n");
          }
        } else {
          // No, something went wrong. All we have is an error
          if (response.getError() == TIMEOUT && retryCounter--) {
            LOG_D("Retry on timeout...\n");
            doNotPop = true;
          } else {
            // Do we have an onError handler?
            LOG_D("Error response.\n");
            if (instance->onError) {
              // Yes. Forward the error code to it
              instance->onError(response.getError(), request->token);
            } else {
              LOG_D("No onError handler\n");
            }
          }
        }
        //   set lastHost/lastPort tp host/port
        instance->MT_lastTarget = request->target;
      } else {
        // Oops. Connection failed
        // Retry, if attempts are left or report error.
        if (retryCounter--) {
          instance->MT_client.stop();
          delay(10);
          LOG_D("Retry on connect failure...\n");
          doNotPop = true;
        } else {
          // Do we have an onError handler?
          if (instance->onError) {
            // Yes. Forward the error code to it
            instance->onError(IP_CONNECTION_FAILED, request->token);
          }
        }
      }
      // Clean-up time. 
      if (!doNotPop)
      {
        // Safely lock the queue
        #if USE_MUTEX
        lock_guard<mutex> lockGuard(instance->qLock);
        #endif
        // Remove the front queue entry
        instance->requests.pop();
        retryCounter = RETRIES;
        // Delete request
        delete request;
        LOG_D("Request popped from queue.\n");
      }
      lastRequest = millis();
    } else {
      delay(1);  // Give scheduler room to breathe
    }
  }
}

// send: send request via Client connection
void ModbusClientTCP::send(RequestEntry *request) {
  // We have a established connection here, so we can write right away.
  // Move tcpHead and request into one continuous buffer, since the very first request tends to 
  // take too long to be sent to be recognized.
  ModbusMessage m;
  m.add((const uint8_t *)request->head, 6);
  m.append(request->msg);

  MT_client.write(m.data(), m.size());
  // Done. Are we?
  MT_client.flush();
  HEXDUMP_V("Request packet", m.data(), m.size());
}

// receive: get response via Client connection
ModbusMessage ModbusClientTCP::receive(RequestEntry *request) {
  uint32_t lastMillis = millis();     // Timer to check for timeout
  bool hadData = false;               // flag data received
  const uint16_t dataLen(300);        // Modbus Packet supposedly will fit (260<300)
  uint8_t data[dataLen];              // Local buffer to collect received data
  uint16_t dataPtr = 0;               // Pointer into data
  ModbusMessage response;             // Response structure to be returned

  // wait for packet data, overflow or timeout
  while (millis() - lastMillis < request->target.timeout && dataPtr < dataLen && !hadData) {
    // Is there data waiting?
    if (MT_client.available()) {
      // Yes. catch as much as is there and fits into buffer
      while (MT_client.available() && dataPtr < dataLen) {
        data[dataPtr++] = MT_client.read();
      }
      // Register data received
      hadData = true;
      // Rewind EOT and timeout timers
      lastMillis = millis();
    }
    delay(1); // Give scheduler room to breathe
  }
  // Did we get some data?
  if (hadData) {
    LOG_D("Received response.\n");
    HEXDUMP_V("Response packet", data, dataPtr);
    // Yes. check it for validity
    // First transactionID and protocolID shall be identical, length has to match the remainder.
    ModbusTCPhead head(request->head.transactionID, request->head.protocolID, dataPtr - 6);
    // Matching head?
    if (memcmp((const uint8_t *)head, data, 6)) {
      // No. return Error response
      response.setError(request->msg.getServerID(), request->msg.getFunctionCode(), TCP_HEAD_MISMATCH);
      // If the server id does not match that of the request, report error
    } else if (data[6] != request->msg.getServerID()) {
      response.setError(request->msg.getServerID(), request->msg.getFunctionCode(), SERVER_ID_MISMATCH);
      // If the function code does not match that of the request, report error
    } else if ((data[7] & 0x7F) != request->msg.getFunctionCode()) {
      response.setError(request->msg.getServerID(), request->msg.getFunctionCode(), FC_MISMATCH);
    } else {
      // Looks good.
      response.add(data + 6, dataPtr - 6);
    }
  } else {
    // No, timeout must have struck
    response.setError(request->msg.getServerID(), request->msg.getFunctionCode(), TIMEOUT);
  }
  return response;
}

#endif
