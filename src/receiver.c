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

#include "our_protocol.h"
#include <stdbool.h>
#include <fcntl.h>

#define LONG_TIMER_MS 500 // 0.5s
#define SHORT_TIMER_MS 100
#define BUFFER_SIZE 1024 // FIXME: this is arbitrary for now...
#define MAX_PACKETS_IN_WINDOW (MAX_WINDOW_SIZE / PACKET_SIZE)

static unsigned int receiver_current_state;
static unsigned long long int receiver_write_rate; // Not actually used for now...
static FILE *receiver_file;
static int receiver_socket;
static time_t timer_start;

static struct protocol_Packet *buffered_packets;
static uint16_t next_needed_packet_num;

enum receiver_state
{
    /* Connection Setup */
    Wait_Connection,

    /* Receive Data*/
    Wait_for_Packet,
    Wait_for_Pipeline,

    /* Connection Teardown */
    Send_Fin_Ack,
    Wait_inCase,
    Finished
};

/* ================ Function Declarations Start ================ */
/* Initialization */
bool receiver_init(unsigned short int myUDPport, char* destinationFile, unsigned long long int writeRate);

/* Closing file, socket, etc. */
void receiver_finish(void);

/* Checking packets */
bool is_SYNC(const char* packet);
bool is_data(const char* packet);
bool is_FIN(const char* packet);

/* Connection Setup */
void receiver_action_Wait_Connection(void);

/* Receive Data*/
void receiver_action_Wait_for_Packet(void);
void receiver_action_Wait_for_Pipeline(void);

/* Connection Teardown */
void receiver_action_Send_Fin_Ack(void);
void receiver_action_Wait_inCase(void);

/* Helper function for sorting buffered packets */
int compare_packets(const void *a, const void *b);
/* ================ Function Declarations END ================ */


void rrecv(unsigned short int myUDPport, 
            char* destinationFile, 
            unsigned long long int writeRate) {

    if (!receiver_init(myUDPport, destinationFile, writeRate)) {
        // Error'd out somewhere in initialization.
        receiver_finish();
        return;
    }

    while(receiver_current_state != Finished) {
        switch(receiver_current_state) {
            case(Wait_Connection):
                receiver_action_Wait_Connection();
                break;
            case(Wait_for_Packet):
                receiver_action_Wait_for_Packet();
                break;
            case(Wait_for_Pipeline):
                receiver_action_Wait_for_Pipeline();
                break;
            case(Send_Fin_Ack):
                receiver_action_Send_Fin_Ack();
                break;
            case(Wait_inCase):
                receiver_action_Wait_inCase();
                break;
        }
    }

    receiver_finish();
}

// FIXME: IF errors occur, should destroy/close everything.
bool receiver_init(unsigned short int myUDPport, 
            char* destinationFile, 
            unsigned long long int writeRate) {
    // Create the UDP socket.
    receiver_socket = socket(AF_INET, SOCK_DGRAM, 0);
    if (receiver_socket < 0) {
        perror("Error with creating socket.");
        return false;
    }
    
    // Set the socket as non-blocking.
    if (fcntl(receiver_socket, F_SETFL, fcntl(receiver_socket, F_GETLK, 0) | O_NONBLOCK)) {
        perror("Error with setting socket flags.");
        return false;
    }

    // Set socket address for receiving.
    struct sockaddr_in *receiver_socket_addr;
    receiver_socket_addr->sin_family = AF_INET;
    receiver_socket_addr->sin_port = htons(myUDPport);
    receiver_socket_addr->sin_addr.s_addr = htonl(INADDR_ANY); // no-specific IP-address

    // Bind the socket to the address and port.
    if (bind(receiver_socket, (struct sockaddr *)receiver_socket_addr, sizeof(receiver_socket_addr)) < 0) {
        perror("Error binding to the port.");
        return false;
    }

    // Open file for writing.
    FILE *filePointer;
    filePointer = fopen(destinationFile, "wb");

    if (filePointer == NULL) {
        perror("Error opening file.");
        return false;
    }

    // Set static variables.
    receiver_file = filePointer;
    receiver_write_rate = writeRate;
    
    // Set default values.
    receiver_current_state = Wait_Connection;
    buffered_packets = NULL;
    next_needed_packet_num = 0;

    return true;
}

// TODO
void receiver_finish(void) {
    return;
}

// Checks if incoming packet is valid SYNC packet.
bool is_SYNC(const char* packet) {
    struct protocol_Header header = ((struct protocol_Packet *)packet)->header;
    uint8_t SYNC_bit = header.management_byte & 0x80; // SYNC is upper-most bit.
    return SYNC_bit == 1;
}

// Checks if incoming packet is data packet (management byte is required to be zero for data).
bool is_data(const char* packet) {
    struct protocol_Header header = ((struct protocol_Packet *)packet)->header;
    return header.management_byte == 0;
}

// Checks if incoming packet is valid FIN packet.
bool is_FIN(const char* packet) {
    struct protocol_Header header = ((struct protocol_Packet *)packet)->header;
    uint8_t FIN_bit = header.management_byte & 0x1; // FIN is second lower-most bit.
    return FIN_bit == 1;
}

void receiver_action_Wait_Connection(void) {
    char buffer[BUFFER_SIZE];
    struct sockaddr_in sender_addr;
    socklen_t addr_size = sizeof(sender_addr);

    // Check for any incoming packets
    ssize_t packet_size = recvfrom(receiver_socket, buffer, sizeof(buffer), 0, (struct sockaddr *)&sender_addr, &addr_size);

    if (packet_size > 0) {
        // Check if was a SYNC packet.
        if (is_SYNC(buffer)) {
            // Now connect to the sender, as we only want to communicate with this sender.
            if (connect(receiver_socket, (struct sockaddr *)&sender_addr, addr_size) < 0) {
                perror("Error connecting to sender.");
                // FIXME: handle this?
            }

            // Send SYNC_ACK back to sender to complete handshaking.
            struct protocol_Header SYNC_ACK_packet;
            memset(&SYNC_ACK_packet, 0, sizeof(SYNC_ACK_packet));
            
            SYNC_ACK_packet.management_byte = 0x40; // set second-highest bit for SYNC ACK.
            // Everything else should already be zero'd...

            if (send(receiver_socket, &SYNC_ACK_packet, sizeof(SYNC_ACK_packet), 0) < 0) {
                perror("Error with sending SYNC_ACK.");
                // FIXME: Handle this?
            }
            
            receiver_current_state = Wait_for_Packet;
        }
    } else if (packet_size < 0) {
        perror("Error with recvfrom.");
        // FIXME: handle this?
    }
    // Otherwise, no data received. Stay in Wait_Connection.
}

void receiver_action_Wait_for_Packet(void) {
    // Check for any incoming packets...
    char buffer[BUFFER_SIZE];
    ssize_t packet_size = recv(receiver_socket, buffer, sizeof(buffer), 0);

    if (packet_size > 0) {
        // Handle checking if valid seq packet, duplicate, finish, etc.
        if (is_data(buffer)) {
            // Check if valid sequence packet or is a duplicate.
            uint16_t sequence_num = ((struct protocol_Packet *)buffer)->header.seq_ack_num;
            if ((sequence_num >= next_needed_packet_num) && (sequence_num < (next_needed_packet_num + MAX_PACKETS_IN_WINDOW))) {
                // Duplicate or invalid, send cumulative ACK right away.
                struct protocol_Packet ACK_packet;
                memset(&ACK_packet, 0, sizeof(ACK_packet));

                ACK_packet.header.seq_ack_num = next_needed_packet_num;
                // Everything else should already be zero'd...
                
                if (send(receiver_socket, &ACK_packet, sizeof(ACK_packet), 0) < 0) {
                    perror("Error with sending ACK.");
                    // FIXME: Handle this?
                }
            } else {                
                // Valid sequence packet, setup buffer and get ready for more data!
                buffered_packets = malloc(MAX_PACKETS_IN_WINDOW * sizeof(struct protocol_Packet));
                if (buffered_packets == NULL) {
                    perror("Failed to malloc for buffered packets.");
                    // FIXME: Should error out.
                }
                buffered_packets[0] = *(struct protocol_Packet *)buffer;

                // Start small countdown-timer and now wait for pipeline.
                timer_start = clock();
                receiver_current_state = Wait_for_Pipeline;
            }
        } else if (is_FIN(buffer)) {
            receiver_current_state = Send_Fin_Ack;
        }
    } else if (packet_size < 0) {
        perror("Error with recv.");
        // FIXME: handle this?
    }
    // Otherwise, no data received. Stay in Wait_for_Packet.
}

// TODO
void receiver_action_Wait_for_Pipeline(void) {
    // Check for any incoming FINs (just in-case)...
    char buffer[BUFFER_SIZE];
    ssize_t packet_size = recv(receiver_socket, buffer, sizeof(buffer), 0);

    if (packet_size > 0 && is_data(buffer)) {
        uint16_t sequence_num = ((struct protocol_Packet *)buffer)->header.seq_ack_num;
        
        // Check if within window
        if ((sequence_num >= next_needed_packet_num) && (sequence_num < (next_needed_packet_num + MAX_PACKETS_IN_WINDOW))) {
            // If we already received this packet within the timer, it doesn't matter (data will be identical).
            buffered_packets[sequence_num - next_needed_packet_num] = *(struct protocol_Packet *)buffer;
        }
    } else if (packet_size < 0) {
        perror("Error with recv while waiting for pipeline.");
        // FIXME: handle this?
    }

    clock_t time_elapsed_ms = (clock() - timer_start) * 1000 / CLOCKS_PER_SEC;
    
    if (time_elapsed_ms > SHORT_TIMER_MS) {
        // Timer ran out, so we have to deal with data that's been collected in the buffer.
        // Sort the buffered data.
        qsort(buffered_packets, MAX_PACKETS_IN_WINDOW, sizeof(struct protocol_Packet), compare_packets);

        // Now go in-order of sorted data until we find a missing packet or process the entire window
        for (int i = 0; i < MAX_PACKETS_IN_WINDOW; i++) {
            struct protocol_Packet packet = buffered_packets[i];

            // Check if it's the next packet we need (otherwise we are missing one)
            if (packet.header.seq_ack_num != next_needed_packet_num) {
                break;
            }

            // Write data to output file
            fwrite(buffered_packets[i].data, 1, PROTOCOL_DATA_SIZE, receiver_file);

            next_needed_packet_num++;
        }

        // Free the buffer for packets
        free(buffered_packets);
        buffered_packets = NULL;

        // Send Cumulative ACK
        struct protocol_Packet ACK_packet;
        memset(&ACK_packet, 0, sizeof(ACK_packet));

        ACK_packet.header.seq_ack_num = next_needed_packet_num;
        // Everything else should already be zero'd...
        
        if (send(receiver_socket, &ACK_packet, sizeof(ACK_packet), 0) < 0) {
            perror("Error with sending ACK.");
            // FIXME: Handle this?
        }

        receiver_current_state = Wait_for_Packet;
    }
}

// Send FIN_ACK back to sender.
void receiver_action_Send_Fin_Ack(void) {
    // Construct FIN_ACK packet.
    struct protocol_Header FIN_ACK_packet;
    memset(&FIN_ACK_packet, 0, sizeof(FIN_ACK_packet));

    FIN_ACK_packet.management_byte = 0x1; // FIN_ACK bit
    // Everything else should already be zero'd...
    
    if (send(receiver_socket, &FIN_ACK_packet, sizeof(FIN_ACK_packet), 0) < 0) {
        perror("Error with sending FIN_ACK.");
        // FIXME: Handle this?
    }

    // Start the long timer and goto wait in-case...
    timer_start = clock();
    receiver_current_state = Wait_inCase;
}

// Wait for a long time just in-case we get another FIN packet.
void receiver_action_Wait_inCase(void) {
    // Check for any incoming FINs (just in-case)...
    char buffer[BUFFER_SIZE];
    ssize_t packet_size = recv(receiver_socket, buffer, sizeof(buffer), 0);

    if (packet_size > 0 && is_FIN(buffer)) {
        receiver_current_state = Send_Fin_Ack;
    } else if (errno == EAGAIN || errno == EWOULDBLOCK) {
        // No packet received.
        clock_t time_elapsed_ms = (clock() - timer_start) * 1000 / CLOCKS_PER_SEC;
        
        if (time_elapsed_ms > LONG_TIMER_MS) {
            receiver_current_state = Finished;
        }
    } else {
        perror("Error with recv while waiting in-case.");
        // FIXME: Handle this?
    }
}

int main(int argc, char** argv) {
    // This is a skeleton of a main function.
    // You should implement this function more completely
    // so that one can invoke the file transfer from the
    // command line.

    unsigned short int udpPort;
    char* filename = NULL;
    unsigned long long int writeRate = 0; // FIXME: this is just default (no limit), will need to be fixed later on.

    if (argc != 3) {
        fprintf(stderr, "usage: %s UDP_port filename_to_write\n\n", argv[0]);
        exit(1);
    }

    udpPort = (unsigned short int) atoi(argv[1]);
    filename = argv[2];

    rrecv(udpPort, filename, writeRate);
}

// Comparing function for sorting buffered packets.
int compare_packets(const void *a, const void *b) {
    struct protocol_Packet *packet1 = (struct protocol_Packet *)a;
    struct protocol_Packet *packet2 = (struct protocol_Packet *)b;

    if (packet1->header.seq_ack_num < packet2->header.seq_ack_num) {
        return -1;
    } else if (packet1->header.seq_ack_num > packet2->header.seq_ack_num) {
        return 1;
    } else {
        return 0;
    }
}
