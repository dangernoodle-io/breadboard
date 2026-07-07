#pragma once

#include <stddef.h>

// Pure log-line parser. Compiled on both host and ESP-IDF (no platform deps).
// Parses ESP-IDF console format: "<L> (<ts>) <tag>: <msg>"
// Strips leading ANSI CSI escape sequences and trailing CR/LF.
// On parse failure: level_out='?', tag_out="", msg_out=<trimmed line>.
// msg is bounded to 160 bytes before copying into msg_out.
void bb_log_event_parse(const char *line, size_t len,
                        char *level_out,
                        char *tag_out, size_t tag_cap,
                        char *msg_out, size_t msg_cap);
