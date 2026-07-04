#ifndef WEBSERVER_H
#define WEBSERVER_H

// Start the async HTTP server (call after WiFi is connected)
void webserver_start(void);

// Execute a named action string (used by webserver + scheduler)
void execute_named_action(const char *action);

#endif
