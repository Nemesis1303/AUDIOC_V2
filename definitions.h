#include <sys/stat.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <stdarg.h>

#define PACKET_SENT 0
#define PACKET_RECEIVED 1
#define AUDIO_BLOCK_TAKEN 2
#define SILENCE_AUDIO_BLOCK_INSERTED 3 
#define PACKET_LOST 4
#define SELECT_TIMER_EXPIRES 5

#define PORT_UNICAST 5004
#define MAX_BUFFER_SIZE 65536

#define ZERO_8 128 // Range 0 - 255
#define ZERO_16 0  // Range -32768 - 32767


