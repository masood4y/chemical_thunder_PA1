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
#include <fcntl.h>

#define LONG_TIMER_MS 5000 // 5s
#define SHORT_TIMER_MS 0.5
#define BUFFER_SIZE 1455 // FIXME: this is arbitrary for now...
#define MAX_PACKETS_IN_WINDOW (MAX_WINDOW_SIZE / PACKET_SIZE)


static unsigned int receiver_current_state;
static unsigned long long int receiver_write_rate; // Not actually used for now...
static FILE *receiver_file;
static int receiver_socket;
static time_t timer_start;


static char *buffered_bytes;
static uint16_t next_needed_seq_num;
static uint16_t received[2];
static uint16_t anticipate_next[2];

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
int receiver_init(unsigned short int myUDPport, char* destinationFile, unsigned long long int writeRate);

/* Closing file, socket, etc. */
void receiver_finish(void);

/* Checking packets */
int is_SYNC(const char* packet);
int is_data(const char* packet);
int is_FIN(const char* packet);
int is_duplicate(uint16_t seq_num);

/* Connection Setup */
void receiver_action_Wait_Connection(void);

/* Receive Data*/
void receiver_action_Wait_for_Packet(void);
void receiver_action_Wait_for_Pipeline(void);

/* Connection Teardown */
void receiver_action_Send_Fin_Ack(void);
void receiver_action_Wait_inCase(void);

/* Helper function for sorting buffered packets */
//int compare_packets(const void *a, const void *b);
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
int receiver_init(unsigned short int myUDPport, 
            char* destinationFile, 
            unsigned long long int writeRate) {
    // Create the UDP socket.
    receiver_socket = socket(AF_INET, SOCK_DGRAM, 0);
    if (receiver_socket < 0) {
        perror("Error with creating socket.");
        return 0;
    }
    
    // Set the socket as non-blocking.
    if (fcntl(receiver_socket, F_SETFL, fcntl(receiver_socket, F_GETFL, 0) | O_NONBLOCK)) {
        perror("Error with setting socket flags.");
        return 0;
    }

    // Set socket address for receiving.
    struct sockaddr_in receiver_socket_addr;
    memset(&receiver_socket_addr, 0, sizeof(struct sockaddr_in));
    receiver_socket_addr.sin_family = AF_INET;
    receiver_socket_addr.sin_port = htons(myUDPport);
    receiver_socket_addr.sin_addr.s_addr = htonl(INADDR_ANY); // no-specific IP-address

    // Bind the socket to the address and port.
    if (bind(receiver_socket, (struct sockaddr *)&receiver_socket_addr, sizeof(receiver_socket_addr)) < 0) {
        perror("Error binding to the port.");
        return 0;
    }

    // Open file for writing.
    FILE *filePointer;
    filePointer = fopen(destinationFile, "wb+");
    printf("opening file\n");

    if (filePointer == NULL) {
        perror("Error opening file.");
        return 0;
    }

    // Set static variables.
    receiver_file = filePointer;
    receiver_write_rate = writeRate;

    buffered_bytes = malloc(MAX_WINDOW_SIZE);
    if (buffered_bytes == NULL) 
    {
        perror("Failed to malloc for buffered bytes.");
            // FIXME: Should error out.
    }
    // fill buffered bytes with EOFs to signify theyre empty
    for (int i = 0; i < MAX_WINDOW_SIZE; i++)
    {
        buffered_bytes[i] = EOF;
    }

    
    // Set default values.
    receiver_current_state = Wait_Connection;
    
    next_needed_seq_num = 0;
    anticipate_next[0] = next_needed_seq_num;
    anticipate_next[1] = anticipate_next[0] + MAX_WINDOW_SIZE;
    received[0] = anticipate_next[1] + 1;
    received[1] = anticipate_next[0] - 1;
    
    return 1;
}

// FIXME: Potentially more?
void receiver_finish(void) {
    // Close the file.
    if (buffered_bytes != NULL)
    {
        free(buffered_bytes);
    }
    

    if (receiver_file != NULL) {
        fclose(receiver_file);
        receiver_file = NULL;
    }

    // Close the socket
    if (receiver_socket >= 0) {
        close(receiver_socket);
    }
}

// Checks if incoming packet is valid SYNC packet.
int is_SYNC(const char* packet) {
    struct protocol_Header header = ((struct protocol_Packet *)packet)->header;
    uint8_t SYNC_bit = header.management_byte & 0x80; // SYNC is upper-most bit.
    return SYNC_bit == 0x80;
}

// Checks if incoming packet is data packet (management byte is required to be zero for data).
int is_data(const char* packet) {
    struct protocol_Header header = ((struct protocol_Packet *)packet)->header;
    return header.management_byte == 0;
}

// Checks if incoming packet is valid FIN packet.
int is_FIN(const char* packet) {
    struct protocol_Header header = ((struct protocol_Packet *)packet)->header;
    uint8_t FIN_bit = header.management_byte & 0x2; // FIN is second lower-most bit.
    return FIN_bit == 0x2;
}

int is_duplicate(uint16_t seq_num) 
{

    if (received[0] < received[1])
    {
        return ((seq_num >= received[0]) && (seq_num <= received[1]));
    }
    else if (received[0] > received[1]) 
    {
        return ((seq_num >= received[0]) || (seq_num <= received[1]));
    }
    return 0;
}

void receiver_action_Wait_Connection(void) 
{
    char buffer[BUFFER_SIZE];
    struct sockaddr_in sender_addr;
    socklen_t addr_size = sizeof(sender_addr);

    // Check for any incoming packets
    ssize_t packet_size = recvfrom(receiver_socket, buffer, sizeof(buffer), 0, (struct sockaddr *)&sender_addr, &addr_size);

    if (packet_size > 0) {
        // Check if was a SYNC packet.
        if (is_SYNC(buffer)) {
            printf("Connecting to Sender\n");
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
            printf("sending sync ack\n");
            
            receiver_current_state = Wait_for_Packet;
        }
    } else if ((packet_size < 0)  && (errno != EAGAIN && errno != EWOULDBLOCK)) {
        perror("Error with recvfrom.");
        // FIXME: handle this?
    }
    // Otherwise, no data received. Stay in Wait_Connection.
}

void receiver_action_Wait_for_Packet(void) {
    // Check for any incoming packets...
    char buffer[BUFFER_SIZE];
    ssize_t packet_size = recv(receiver_socket, buffer, sizeof(buffer), 0);

    if (packet_size > 0) 
    {
        // Handle checking if valid seq packet, duplicate, finish, etc.
        if (is_SYNC(buffer)) 
        {
            printf("received another sync\n");
            // Send SYNC_ACK back to sender to complete handshaking.
            struct protocol_Header SYNC_ACK_packet;
            memset(&SYNC_ACK_packet, 0, sizeof(SYNC_ACK_packet));
            
            SYNC_ACK_packet.management_byte = 0x40; // set second-highest bit for SYNC ACK.
            // Everything else should already be zero'd...

            if (send(receiver_socket, &SYNC_ACK_packet, sizeof(SYNC_ACK_packet), 0) < 0) {
                perror("Error with sending SYNC_ACK.");
                // FIXME: Handle this?
            }
        }
        else if (is_data(buffer)) {
            // Check if valid sequence packet or is a duplicate.
            uint16_t sequence_num_received = ((struct protocol_Packet *)&buffer)->header.seq_ack_num;
            printf("received Packet %d\n", sequence_num_received);

            if (is_duplicate(sequence_num_received))
            {   
                printf("rPacket %d was a duplicate\n", sequence_num_received);
                // Duplicate or invalid, send cumulative ACK right away.
                struct protocol_Header ACK_packet;
                memset(&ACK_packet, 0, sizeof(ACK_packet));

                ACK_packet.seq_ack_num = next_needed_seq_num;
                // Everything else should already be zero'd...
                
                if (send(receiver_socket, &ACK_packet, sizeof(ACK_packet), 0) < 0) {
                    perror("Error with sending ACK.");
                    // FIXME: Handle this?
                }
            } 
            else {                
                uint16_t buffer_index;
                uint16_t local_seq_num = sequence_num_received;
                 // buffered_bytes_index = seqnum_byte - next_anticipated_byte
                for(int i = 0; i < PROTOCOL_DATA_SIZE; i++) {
                    buffer_index = local_seq_num - next_needed_seq_num;
                    buffered_bytes[buffer_index] = ((struct protocol_Packet *)buffer)->data[i];
                    local_seq_num++;
                }              

                // Start small countdown-timer and now wait for pipeline.
                timer_start = clock();
                receiver_current_state = Wait_for_Pipeline;
            }
        } 
        else if (is_FIN(buffer)) 
        {
            receiver_current_state = Send_Fin_Ack;
            printf("received Fin\n");
        }
    } 
    else if ((packet_size == -1) && (errno != EAGAIN && errno != EWOULDBLOCK)) 
    {
        perror("Error with recv.");
        // FIXME: handle this?
    }
    // Otherwise, no data received. Stay in Wait_for_Packet.
}





// TODO
void receiver_action_Wait_for_Pipeline(void) 
{
    // Check for any incoming FINs (just in-case)...
    char buffer[BUFFER_SIZE];
    ssize_t packet_size = recv(receiver_socket, buffer, sizeof(buffer), MSG_DONTWAIT);

    if (packet_size > 0 && is_data(buffer)) 
    {
        uint16_t sequence_num_received = ((struct protocol_Packet *)buffer)->header.seq_ack_num;
        printf("Received packet %d\n", sequence_num_received);
        if (!is_duplicate(sequence_num_received))
        {
            uint16_t buffer_index;
            uint16_t local_seq_num = sequence_num_received;
            // buffered_bytes_index = seqnum_byte - next_anticipated_byte
            for(int i = 0; i < PROTOCOL_DATA_SIZE; i++) 
            {
                buffer_index = local_seq_num - next_needed_seq_num;
                buffered_bytes[buffer_index] = ((struct protocol_Packet *)buffer)->data[i];
                local_seq_num++;
            } 
        }            
    }
    else if ((packet_size == -1) && (errno != EAGAIN && errno != EWOULDBLOCK)) 
    {
        perror("Error with recv while waiting for pipeline.");
        // FIXME: handle this?
    }

    clock_t time_elapsed_ms = (clock() - timer_start) * 1000 / CLOCKS_PER_SEC;
    
    if (time_elapsed_ms > SHORT_TIMER_MS) 
    {

        printf("Pipeline timer expired\n");
        for (int i = 0; i < MAX_WINDOW_SIZE; i++)
        {
            //if ((buffered_bytes[i] == '\0') || (buffered_bytes[i] == '\x1A'))
            if (buffered_bytes[i] == EOF)
            {
                break;
            }
            fputc(buffered_bytes[i], receiver_file);
            next_needed_seq_num++;
            buffered_bytes[i] = EOF;
        }


        // Send Cumulative ACK
        struct protocol_Header ACK_packet;
        memset(&ACK_packet, 0, sizeof(ACK_packet));

        ACK_packet.seq_ack_num = next_needed_seq_num;
        // Everything else should already be zero'd...
        
        if (send(receiver_socket, &ACK_packet, sizeof(ACK_packet), 0) < 0) {
            perror("Error with sending ACK.");
            // FIXME: Handle this?
        }
        printf("sending ack, next needed byte: %d\n", next_needed_seq_num);

        anticipate_next[0] = next_needed_seq_num;
        anticipate_next[1] = anticipate_next[0] + MAX_WINDOW_SIZE;
        received[0] = anticipate_next[1] + 1;
        received[1] = anticipate_next[0] - 1;

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
    printf("Sending Fin Ack\n");

    // Start the long timer and goto wait in-case...
    timer_start = clock();
    receiver_current_state = Wait_inCase;
}

// Wait for a long time just in-case we get another FIN packet.
void receiver_action_Wait_inCase(void) {
    // Check for any incoming FINs (just in-case)...
    char buffer[BUFFER_SIZE];
    clock_t time_elapsed_ms = (clock() - timer_start) * 1000 / CLOCKS_PER_SEC;

    ssize_t packet_size = recv(receiver_socket, buffer, sizeof(buffer), MSG_DONTWAIT);

    if (packet_size > 0 && is_FIN(buffer)) 
    {   
        printf("received another fin\n");
        receiver_current_state = Send_Fin_Ack;
    } 
    else if (time_elapsed_ms > LONG_TIMER_MS) 
    {
        printf("Long timer expired, so we're done\n");
        receiver_current_state = Finished;
    } 
    else if ((packet_size == -1) && (errno != EAGAIN && errno != EWOULDBLOCK))
    {
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
// int compare_packets(const void *a, const void *b) {
//     struct protocol_Packet *packet1 = (struct protocol_Packet *)a;
//     struct protocol_Packet *packet2 = (struct protocol_Packet *)b;

//     uint16_t packet1_seq = packet1->header.seq_ack_num;
//     uint16_t packet2_seq = packet2->header.seq_ack_num;

//     // Sequence numbers may overflow and wrap-around.
//     if (packet1_seq > packet2_seq) {
//         if (packet1_seq - packet2_seq > MAX_PACKETS_IN_WINDOW) {
//             // Overflowed.
//             return -1;
//         } else {
//             return 1;
//         }
//     } else if (packet1_seq < packet2_seq) {
//         if (packet2_seq - packet1_seq > MAX_PACKETS_IN_WINDOW) {
//             // Overflowed.
//             return 1;
//         } else {
//             return -1;
//         }
//     } else {
//         return 0;
//     }
// }
