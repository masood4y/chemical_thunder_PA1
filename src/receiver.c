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

//#define all the defines here
#define LONG_TIMER_MS 500 // 0.5s

static unsigned int receiver_current_state;

static unsigned long long int receiver_write_rate;
static FILE *receiver_file;
static int receiver_socket;

static time_t timer_start;


// enum receiver_event
// {
//     /* Connection Setup */
//     Wait_Connection,

//     /* Receive Data*/
//     SYNC_RECEIVED,
//     DUP

//     /* Connection Teardown */
//     LONG_TIMER_FINISH,
//     FIN_RECEIVED
// };

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

// Initialization.
bool receiver_init(unsigned short int myUDPport, char* destinationFile, unsigned long long int writeRate);
// Closing file, socket, etc.
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
    if (fcntl(receiver_socket, F_SETFL, fcntl(receiver_socket, F_GETL, 0) | O_NONBLOCK)) {
        perror("Error with setting socket flags.");
        return false;
    }

    // Set socket address for receiving.
    struct sockaddr_in receiver_socket_addr;
    receiver_socket_addr.sin_family = AF_INET;
    receiver_socket_addr.sin_port = htons(myUDPport);
    receiver_socket_addr.sin_addr = INADDR_ANY; // no-specific IP-address

    // Bind the socket to the address and port.
    if (bind(receiver_socket, (struct sockaddr *)receiver_socket_addr, sizeof(receiver_socket_addr)) < 0) {
        perror("Error binding to the port.");
        return false;
    }

    // Open file for writing.
    FILE *filePointer;
    filePointer = fopen(destinationFile, "w");

    if (filePointer == NULL) {
        perror("Error opening file.");
        return false;
    }

    // Set static variables.
    receiver_file = filePointer;
    receiver_write_rate = writeRate;
    
    // Set default values.
    receiver_current_state = Wait_Connection;
    return true;

    return true;
}

// TODO
void receiver_finish(void) {
    return;
}

// Checks if incoming packet is valid SYNC packet.
bool is_SYNC(const char* buffer) {
    if (sizeof < 0) {
        return false;
    }
    
    struct protocol_Header header = ((struct protocol_Packet *)packet)->header;
    uint8_t SYNC_bit = header.management_byte & 0x80; // SYNC is upper-most bit.
    return SYNC_bit == 1;
}

// Checks if incoming packet is data packet (management byte is required to be zero for data).
bool is_data(const char* buffer) {
    if (size < 0) {
        return false;
    }

    struct protocol_Header header = ((struct protocol_Packet *)packet)->header;
    return header.management_byte == 0;
}

// Checks if incoming packet is valid FIN packet.
bool is_FIN(const char* buffer) {
    if (size < 0) {
        return false;
    }
    
    struct protocol_Header header = ((struct protocol_Packet *)packet)->header;
    uint8_t FIN_bit = header.management_byte & 0x1; // FIN is second lower-most bit.
    return FIN_bit == 1;
}

// TODO: Finish me!
void receiver_action_Wait_Connection(void) {
    char buffer[1024]; // FIXME: this is an arbitary value for now.
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

            // TODO: set up Receive Window

            // Send SYNC_ACK back to sender to complete handshaking.
            char SYNC_ACK_packet[] = "SYNC_ACK"; // FIXME!
            size_t packet_size = sizeof(SYNC_ACK_packet);
            if (send(receiver_socket, sync_ack_packet, packet_size, 0) < 0) {
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

// TODO
void receiver_action_Wait_for_Packet(void) {
    char buffer[1024]; // FIXME: this is an arbitary value for now.

    // Check for any incoming packets
    ssize_t packet_size = recv(receiver_socket, buffer, sizeof(buffer), 0);

    if (packet_size > 0) {
        // Handle checking if valid seq packet, duplicate, finish, etc.
        if (is_data(buffer)) {
            // Check if valid sequence packet or is a duplicate.
            if (is_duplicate(buffer)) {
                // TODO: Duplicate, send cumulative ACK right away.

            } else {
                // Start small countdown-timer, adjust receive window, take care of data, update Ack #...
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
    char buffer[1024]; // FIXME: this is an arbitary value for now.
    ssize_t packet_size = recv(receiver_socket, buffer, sizeof(buffer), 0);

    if (packet_size > 0 && is_data(buffer))
        // TODO: Check if valid seq data packet...
    
    } else if (errno == EAGAIN || errno == EWOULDBLOCK) {
        // TODO: No packet received.

    else {
        perror("Error with recv while waiting for pipeline.");
        // FIXME: handle this?
    }
}

// Send FIN_ACK back to sender.
void receiver_action_Send_Fin_Ack(void) {
    // Construct FIN_ACK packet.
    struct FIN_ACK_packet;
    memset(&FIN_ack_packet, 0, sizeof(fin_ack_packet));

    FIN_ACK_packet.header.management_byte = 0x1; // FIN_ACK bit
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
    char buffer[1024]; // FIXME: this is an arbitary value for now.
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
