// Copyright 2026 Mark Oxley. All rights reserved.
//
// Project: Skink — A self-hosted, statically-typed programming language
//          targeting LLVM IR, designed for systems programming with
//          modern ergonomics and a Go-inspired syntax.
//
// File:    std/runtime/mqtt_rt.c
// Purpose: Stubs for the Mosquitto MQTT runtime.
// Author:  Mark Oxley
// Created: 2026
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

// Stubs for the Mosquitto MQTT runtime.  These let the std/mqtt_test suite
// run without libmosquitto installed, failing invalid calls gracefully.

#include <stdint.h>

// mosquitto_lib_init is a stub that initializes the MQTT library (no-op).
int mosquitto_lib_init(void) { return 0; }
// mosquitto_lib_cleanup is a stub that cleans up the MQTT library (no-op).
int mosquitto_lib_cleanup(void) { return 0; }

// mosquitto_new is a stub that creates a dummy MQTT client handle.
int64_t mosquitto_new(const char *id, int clean_session, int64_t obj) {
  (void)id;
  (void)clean_session;
  (void)obj;
  // Return a non-zero dummy handle so NewClient succeeds.
  return 1;
}

// mosquitto_destroy is a stub that destroys an MQTT client handle (no-op).
int mosquitto_destroy(int64_t mosq) {
  (void)mosq;
  return 0;
}

// mosquitto_connect is a stub that connects an MQTT client to a broker.
int mosquitto_connect(int64_t mosq, const char *host, int port, int keepalive) {
  (void)host;
  (void)port;
  (void)keepalive;
  if (mosq == 0)
    return -1;
  return 0;
}

// mosquitto_disconnect is a stub that disconnects an MQTT client from a broker.
int mosquitto_disconnect(int64_t mosq) {
  (void)mosq;
  return 0;
}

// mosquitto_publish is a stub that publishes a message to an MQTT topic.
int mosquitto_publish(int64_t mosq, int64_t mid, const char *topic,
                      int payloadlen, const char *payload, int qos,
                      int retain) {
  (void)mid;
  (void)topic;
  (void)payloadlen;
  (void)payload;
  (void)qos;
  (void)retain;
  if (mosq == 0)
    return -1;
  return 0;
}

// mosquitto_subscribe is a stub that subscribes an MQTT client to a topic.
int mosquitto_subscribe(int64_t mosq, int64_t mid, const char *topic, int qos) {
  (void)mid;
  (void)topic;
  (void)qos;
  if (mosq == 0)
    return -1;
  return 0;
}

// mosquitto_loop_start is a stub that starts the MQTT network loop thread.
int mosquitto_loop_start(int64_t mosq) {
  if (mosq == 0)
    return -1;
  return 0;
}

// mosquitto_loop_stop is a stub that stops the MQTT network loop thread.
int mosquitto_loop_stop(int64_t mosq, int force) {
  (void)force;
  if (mosq == 0)
    return -1;
  return 0;
}

// mosquitto_loop is a stub that processes MQTT network events for one iteration.
int mosquitto_loop(int64_t mosq, int timeout, int max_packets) {
  (void)timeout;
  (void)max_packets;
  if (mosq == 0)
    return -1;
  return 0;
}
