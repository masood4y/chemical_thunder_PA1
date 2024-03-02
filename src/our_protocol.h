#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <arpa/inet.h>
#include <netinet/in.h>

#include <sys/types.h>
#include <sys/socket.h>
#include <unistd.h>

#include <pthread.h>
#include <errno.h>
#include <time.h>

#define PROTOCOL_DATA_SIZE 1450
#define MAX_WINDOW_SIZE 21750  /* Set as (uint16_t / 3) */ 
#define PACKET_SIZE 1450 // Just data.

struct protocol_Header
{
    /* Sync bit:7, Sync Ack bit:6, 0:5, 0:4, 0:3, 0:2, Fin bit:1, Fin ack bit:0 */
    uint8_t management_byte;

    /* Servers as Seq num for sender, and Ack num for Receiver */
    uint16_t seq_ack_num;
};

struct protocol_Packet
{
    struct protocol_Header header;
    char data[PROTOCOL_DATA_SIZE];
};