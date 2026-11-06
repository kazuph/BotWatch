#pragma once
// Handles only uppercase network commands; caller clears the input buffer afterward.
bool network_serial_command(char* line);
