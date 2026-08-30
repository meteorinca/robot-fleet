#ifndef WEBSERVER_H
#define WEBSERVER_H

// Start the HTTP server (called after WiFi connects)
void webserver_start(void);

// Execute a named action string (used by webserver + scheduler)
void execute_named_action(const char *action);

#endif
