#ifndef WEBSERVER_H
#define WEBSERVER_H

#include <stdbool.h>

// Start the async HTTP server (call after WiFi is initialized/connected)
void webserver_start(void);

// Execute a named action string (used by webserver + scheduler)
void execute_named_action(const char *action);

// Push TTS text to all SSE-connected browsers
void sse_broadcast_tts(const char *text);

#endif
