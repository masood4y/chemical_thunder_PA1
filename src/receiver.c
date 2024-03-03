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

#define LONG_TIMER_MS 5000 // 2.5s
#define SHORT_TIMER_MS 3
#define BUFFER_SIZE (sizeof(struct protocol_Packet) + 16) 
#define MAX_PACKETS_IN_WINDOW (MAX_WINDOW_SIZE / PACKET_SIZE)

static unsigned int receiver_current_state;
static unsigned long long int receiver_write_rate;
static FILE *receiver_file;
static int receiver_socket;
static time_t timer_start;
static char *buffered_bytes;
static int64_t last_valid_buffer_index;  
static int64_t first_valid_buffer_index;
static uint32_t next_needed_seq_num;
static uint32_t received[2];
static uint32_t anticipate_next[2];

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
int setup_socket(unsigned short int myUDPport);
int setup_file(char* destinationFile);
void setup_recv_window(void);

/* Closing file, socket, etc. */
void receiver_finish(void);

/* Checking packets */
int is_SYNC(struct protocol_Packet *receive_buffer);
int is_data(struct protocol_Packet *receive_buffer);
int is_FIN(struct protocol_Packet *receive_buffer);
int is_duplicate(uint32_t seq_num);

/* Connection Setup */
void receiver_action_Wait_Connection(void);

/* Receive Data*/
void receiver_action_Wait_for_Packet(void);
void receiver_action_Wait_for_Pipeline(void);
void add_data_to_buffer(struct protocol_Packet *receive_buffer);

/* Connection Teardown */
void receiver_action_Send_Fin_Ack(void);
void receiver_action_Wait_inCase(void);
/* ================ Function Declarations END ================ */

/**
 * @brief Main state machine for the receiver.
 * 
 * This function initializes the receiver and then continually processes states
 * as part of the main state machine. Handles different states like waiting for connection,
 * waiting for a packet, processing the packet pipeline, sending a FIN ACK, and a waiting state
 * post-FIN ACK. This exits once the receiver reaches the Finished state.
 *
 * @param myUDPport The UDP port to bind the receiver socket to.
 * @param destinationFile The path to the file where the received data will be written.
 * @param writeRate The rate at which data will be written to the file.
 */
void rrecv(unsigned short int myUDPport, char* destinationFile, unsigned long long int writeRate) {

    // Initialize and handle if failed initialization.
    if (!receiver_init(myUDPport, destinationFile, writeRate)) {
        receiver_finish();
        return;
    }

    // Main state machine loop.
    while(receiver_current_state != Finished) {
        switch(receiver_current_state) {
            case Wait_Connection:
                receiver_action_Wait_Connection();
                break;
            case Wait_for_Packet:
                receiver_action_Wait_for_Packet();
                break;
            case Wait_for_Pipeline:
                receiver_action_Wait_for_Pipeline();
                break;
            case Send_Fin_Ack:
                receiver_action_Send_Fin_Ack();
                break;
            case Wait_inCase:
                receiver_action_Wait_inCase();
                break;
        }
    }

    // Clean up resources and exit once finished.
    receiver_finish();
}


/**
 * @brief Initializes the receiver with the specified UDP port, destination file, and write rate.
 *
 * Sets up a UDP socket, prepares the destination file for writing, initializes
 * the receiver's window and state, and allocates memory for buffering incoming data.
 *
 * @param myUDPport The UDP port to bind the receiver socket to.
 * @param destinationFile The path to the file where the received data will be written.
 * @param writeRate The rate at which data will be written to the file.
 * @return Returns 1 on successful initialization, 0 on failure.
 */
int receiver_init(unsigned short int myUDPport, 
                  char* destinationFile, 
                  unsigned long long int writeRate) {
    // Set up UDP Socket
    if (!setup_socket(myUDPport)) {
        return 0;
    }

    // Setup File for Writing
    if (!setup_file(destinationFile)) {
        return 0;
    }
    receiver_write_rate = writeRate;

    // Allocate memory for buffered bytes
    buffered_bytes = malloc(MAX_WINDOW_SIZE);
    if (buffered_bytes == NULL) {
        perror("Failed to malloc for buffered bytes.\n");
        return 0;
    }
    // Initialize buffered bytes
    for (int i = 0; i < MAX_WINDOW_SIZE; i++) {
        buffered_bytes[i] = EOF;
    }
    
    // Setup receive window
    setup_recv_window();
        
    // Set initial receiver state
    receiver_current_state = Wait_Connection;
    return 1;
}

/**
 * @brief Sets up the UDP socket for the receiver.
 *
 * Creates a UDP socket, sets it to non-blocking mode, and binds it to the specified
 * UDP port. Prepares the socket for receiving data on the given port.
 *
 * @param myUDPport The UDP port number to bind the socket to.
 * @return Returns 1 if the socket is set up successfully, 0 otherwise.
 */
int setup_socket(unsigned short int myUDPport) {
    // Create the UDP socket
    receiver_socket = socket(AF_INET, SOCK_DGRAM, 0);
    if (receiver_socket < 0) {
        perror("Error with creating socket.\n");
        return 0;
    }
    
    // Set the socket as non-blocking
    if (fcntl(receiver_socket, F_SETFL, fcntl(receiver_socket, F_GETFL, 0) | O_NONBLOCK)) {
        perror("Error with setting socket flags.\n");
        return 0;
    }

    // Set socket address for receiving
    struct sockaddr_in receiver_socket_addr;
    memset(&receiver_socket_addr, 0, sizeof(struct sockaddr_in));
    receiver_socket_addr.sin_family = AF_INET;
    receiver_socket_addr.sin_port = htons(myUDPport);
    receiver_socket_addr.sin_addr.s_addr = htonl(INADDR_ANY); // Accept connections on any IP address.

    // Bind the socket to the address and port
    if (bind(receiver_socket, (struct sockaddr *)&receiver_socket_addr, sizeof(receiver_socket_addr)) < 0) {
        perror("Error binding to the port.\n");
        return 0;
    }
    return 1;
}

/**
 * @brief Sets up the file for writing received data.
 * 
 * This function opens the specified file for writing. If the file cannot be opened,
 * an error message is displayed, and the function returns 0. On successful opening,
 * the file pointer is stored in a global variable for later use.
 *
 * @param destinationFile The path to the file where the received data will be written.
 * @return Returns 1 if the file is successfully opened, 0 otherwise.
 */
int setup_file(char* destinationFile)
{
    // Open file for writing
    FILE *filePointer;
    filePointer = fopen(destinationFile, "wb+");

    if (filePointer == NULL) {
        perror("Error opening file.\n");
        return 0;
    }
    receiver_file = filePointer;
    return 1;
}

/**
 * @brief Initializes the receiver's window for packet processing.
 * 
 * This function sets up the initial state for the receiver's window, including the 
 * sequence number of the next needed packet and the anticipated range of packet sequence 
 * numbers.
 */
void setup_recv_window(void)
{
    next_needed_seq_num = 0;
    anticipate_next[0] = next_needed_seq_num;
    anticipate_next[1] = anticipate_next[0] + (MAX_WINDOW_SIZE - 1);
    received[0] = anticipate_next[1] + 1;
    received[1] = anticipate_next[0] - 1;
}

/**
 * @brief Cleans up resources used by the receiver.
 * 
 * This function frees the allocated buffer for received bytes, closes the open file (if any),
 * and closes the socket. It is used to clean up resources before the receiver shuts down.
 */
void receiver_finish(void) {
    // Free the buffer if it exists
    if (buffered_bytes != NULL) {
        free(buffered_bytes);
    }

    // Close the file if it's open
    if (receiver_file != NULL) {
        fclose(receiver_file);
        receiver_file = NULL;
    }

    // Close the socket if it's open
    if (receiver_socket >= 0) {
        close(receiver_socket);
    }
}

/**
 * @brief Checks if the incoming packet is a valid SYNC packet.
 * 
 * This function checks the management_byte in the packet header to determine
 * if the SYNC bit (upper-most bit) is set, indicating a SYNC packet.
 *
 * @param receive_buffer Pointer to the received protocol packet.
 * @return Returns 1 if it's a SYNC packet, 0 otherwise.
 */
int is_SYNC(struct protocol_Packet *receive_buffer) {
    uint8_t SYNC_bit = receive_buffer->header.management_byte & 0x80; // SYNC is upper-most bit.
    return SYNC_bit == 0x80;
}

/**
 * @brief Checks if the incoming packet is a data packet.
 * 
 * This function checks if the management_byte in the packet header is zero,
 * indicating a data packet.
 *
 * @param receive_buffer Pointer to the received protocol packet.
 * @return Returns 1 if it's a data packet, 0 otherwise.
 */
int is_data(struct protocol_Packet *receive_buffer) {
    return receive_buffer->header.management_byte == 0;
}

/**
 * @brief Checks if the incoming packet is a valid FIN packet.
 * 
 * This function checks the management_byte in the packet header to determine
 * if the FIN bit (second from the right) is set, indicating a FIN packet.
 *
 * @param receive_buffer Pointer to the received protocol packet.
 * @return Returns 1 if it's a FIN packet, 0 otherwise.
 */
int is_FIN(struct protocol_Packet *receive_buffer) {
    uint8_t FIN_bit = receive_buffer->header.management_byte & 0x2; // FIN bit is second from the right.
    return FIN_bit == 0x2;
}

/**
 * @brief Checks if the incoming sequence number is a duplicate.
 *
 * Determines whether the specified sequence number has already been received.
 * This is used to identify and handle duplicate packets.
 *
 * @param seq_num The sequence number to check.
 * @return Returns 1 if the sequence number is a duplicate, 0 otherwise.
 */
int is_duplicate(uint32_t seq_num) {
    if (received[0] < received[1])
    {
        return ((seq_num >= received[0]) && (seq_num <= received[1]));
    }
    else if (received[0] > received[1]) 
    {
        return ((seq_num >= received[0]) || (seq_num <= received[1]));
    }
    else if (received[0] == received[1])
    {
        return (seq_num == received[0]);
    }
    return 0;
}

/**
 * @brief Handles the Wait Connection state of the receiver.
 *
 * Waits for a SYNC packet from the sender to establish a connection.
 * Upon receiving a SYNC packet, sends a SYNC ACK back to the sender.
 */
void receiver_action_Wait_Connection(void) 
{
    char buffer[BUFFER_SIZE];
    struct sockaddr_in sender_addr;
    socklen_t addr_size = sizeof(sender_addr);

    // Check for any incoming packets
    ssize_t packet_size = recvfrom(receiver_socket, buffer, sizeof(buffer), 0, (struct sockaddr *)&sender_addr, &addr_size);

    if (packet_size > 0) {
        // Check if was a SYNC packet.
        if (is_SYNC((struct protocol_Packet *)buffer)) {
            
            // Now connect to the sender, as we only want to communicate with this sender.
            if (connect(receiver_socket, (struct sockaddr *)&sender_addr, addr_size) < 0) {
                perror("Error connecting to sender.\n");
            }
                // Send SYNC_ACK back to sender to complete handshaking.
            struct protocol_Header SYNC_ACK_packet;
            memset(&SYNC_ACK_packet, 0, sizeof(SYNC_ACK_packet));
            
            SYNC_ACK_packet.management_byte = 0x40; // set second-highest bit for SYNC ACK.
            // Everything else should already be zero'd...

            if (send(receiver_socket, &SYNC_ACK_packet, sizeof(SYNC_ACK_packet), 0) < 0) {
                perror("Error with sending SYNC_ACK.\n");
            }
            receiver_current_state = Wait_for_Packet;
        }
    } else if ((packet_size < 0)  && (errno != EAGAIN && errno != EWOULDBLOCK)) {
        perror("Error with recvfrom.\n");
        receiver_current_state = Finished;
    }
    // Otherwise, no data received. Stay in Wait_Connection.
}

/**
 * @brief Handles the Wait for Packet state of the receiver.
 *
 * Waits for data packets from the sender. Processes received packets,
 * checks for duplicates, and handles SYNC and FIN packets.
 */
void receiver_action_Wait_for_Packet(void) {
    // Check for any incoming packets...
    struct protocol_Packet receive_buffer;
    ssize_t bytes_received = recv(receiver_socket, &receive_buffer, sizeof(struct protocol_Packet), MSG_DONTWAIT);

    if (bytes_received > 0) 
    {
        // Handle checking if valid seq packet, duplicate, finish, etc.
        if (is_SYNC(&receive_buffer)) 
        {
            // Send SYNC_ACK back to sender to complete handshaking.
            struct protocol_Header SYNC_ACK_packet;
            memset(&SYNC_ACK_packet, 0, sizeof(SYNC_ACK_packet));
            
            SYNC_ACK_packet.management_byte = 0x40; // set second-highest bit for SYNC ACK.
            // Everything else should already be zero'd...

            if (send(receiver_socket, &SYNC_ACK_packet, sizeof(SYNC_ACK_packet), 0) < 0) {
                perror("Error with sending SYNC_ACK.\n");
                receiver_current_state = Finished;
            }
        }
        else if (is_data(&receive_buffer)) 
        {
            // Check if valid sequence packet or is a duplicate.
            uint32_t sequence_num_received = receive_buffer.header.seq_ack_num;
            uint32_t bytes_data_in_packet = receive_buffer.header.bytes_of_data;

            if (is_duplicate(sequence_num_received))
            {   
                // Duplicate or invalid, send cumulative ACK right away.
                struct protocol_Header ACK_packet;
                memset(&ACK_packet, 0, sizeof(ACK_packet));
                                
                ACK_packet.seq_ack_num = sequence_num_received + bytes_data_in_packet;
                // Everything else should already be zero'd...
                
                if (send(receiver_socket, &ACK_packet, sizeof(ACK_packet), 0) < 0) {
                    perror("Error with sending ACK.");
                    receiver_current_state = Finished;
                }
            } 
            else {      
                // ADD it to the buffered_bytes
                last_valid_buffer_index = -1;  
                first_valid_buffer_index = MAX_WINDOW_SIZE;

                add_data_to_buffer(&receive_buffer);
                // Start small countdown-timer and now wait for pipeline.
                timer_start = clock();
                receiver_current_state = Wait_for_Pipeline;
            }
        } 
        else if (is_FIN(&receive_buffer)) 
        {
            receiver_current_state = Send_Fin_Ack;
        }
    } 
    else if ((bytes_received == -1) && (errno != EAGAIN && errno != EWOULDBLOCK)) 
    {
        perror("Error with recv.");
        receiver_current_state = Finished;
    }
    // Otherwise, no data received. Stay in Wait_for_Packet.
}

/**
 * @brief Handles the Wait for Pipeline state of the receiver.
 *
 * Waits for additional packets in the pipeline. Processes received data packets,
 * checks for duplicates, and handles writing data to the file after a short timer.
 */
void receiver_action_Wait_for_Pipeline(void) 
{
    // Check for any incoming FINs (just in-case)...
    struct protocol_Packet receive_buffer;
    ssize_t bytes_received = recv(receiver_socket, &receive_buffer, 
                                    sizeof(struct protocol_Packet), MSG_DONTWAIT);
    
    if (bytes_received > 0 && is_data(&receive_buffer)) 
    {
        uint32_t sequence_num_received = receive_buffer.header.seq_ack_num;
        if (!is_duplicate(sequence_num_received))
        {       
            add_data_to_buffer(&receive_buffer);
        }            
    }
    else if ((bytes_received == -1) && (errno != EAGAIN && errno != EWOULDBLOCK)) 
    {
        perror("Error with recv while waiting for pipeline.");
        receiver_current_state = Finished;
    }

    clock_t time_elapsed_ms = (clock() - timer_start) * 1000 / CLOCKS_PER_SEC;
    
    if (time_elapsed_ms > SHORT_TIMER_MS) 
    {
        if (first_valid_buffer_index == 0)
        {
            for (int i = 0; (i <= last_valid_buffer_index && i < MAX_WINDOW_SIZE); i++)
            {   
                fputc(buffered_bytes[i], receiver_file);
                next_needed_seq_num++;
                buffered_bytes[i] = EOF;
            }
        }
        
        // Send Cumulative ACK
        struct protocol_Header ACK_packet;
        memset(&ACK_packet, 0, sizeof(ACK_packet));

        ACK_packet.seq_ack_num = next_needed_seq_num;
        // Everything else should already be zero'd...
        
        if (send(receiver_socket, &ACK_packet, sizeof(ACK_packet), 0) < 0) {
            perror("Error with sending ACK.");
            receiver_current_state = Finished;
        }

        anticipate_next[0] = next_needed_seq_num;
        anticipate_next[1] = anticipate_next[0] + (MAX_WINDOW_SIZE - 1);
        received[0] = anticipate_next[1] + 1;
        received[1] = anticipate_next[0] - 1;

        receiver_current_state = Wait_for_Packet;
    }
}


/**
 * @brief Adds data from a received packet to the buffer.
 * 
 * This function processes the received packet and adds its data to the buffer. 
 * It calculates the buffer index based on the sequence number and stores the data 
 * accordingly. It also updates the indices for the first and last valid buffer positions.
 *
 * @param receive_buffer Pointer to the received protocol packet.
 */
void add_data_to_buffer(struct protocol_Packet *receive_buffer) {
    uint32_t buffer_index;
    uint32_t local_seq_num = receive_buffer->header.seq_ack_num;
    uint16_t bytes_data_in_packet = receive_buffer->header.bytes_of_data;
    buffer_index = local_seq_num - next_needed_seq_num;
    if (buffer_index == 0) {
        first_valid_buffer_index = 0;
    }

    for (int i = 0; i < bytes_data_in_packet; i++) {
        buffer_index = local_seq_num - next_needed_seq_num;
        buffered_bytes[buffer_index] = receive_buffer->data[i];
        local_seq_num++;
    }
    if (buffer_index >= last_valid_buffer_index) 
    {
        last_valid_buffer_index = buffer_index;       
    }
}


/**
 * @brief Sends a FIN_ACK packet to the sender.
 * 
 * This function constructs a FIN_ACK packet and sends it to the sender. It is called
 * when a FIN packet is received, indicating the end of data transmission. The function
 * also starts a long timer and sets the receiver's state to Wait_inCase.
 */
void receiver_action_Send_Fin_Ack(void) {
    // Construct FIN_ACK packet.
    struct protocol_Header FIN_ACK_packet;
    memset(&FIN_ACK_packet, 0, sizeof(FIN_ACK_packet));
    FIN_ACK_packet.management_byte = 0x1; // FIN_ACK bit
    // Everything else should already be zero'd...

    if (send(receiver_socket, &FIN_ACK_packet, sizeof(FIN_ACK_packet), 0) < 0) {
        perror("Error with sending FIN_ACK.");
        receiver_current_state = Finished;
    }
    
    // Start the long timer and goto wait in-case...
    timer_start = clock();
    receiver_current_state = Wait_inCase;
}

/**
 * @brief Waits for a long duration in case another FIN packet is received.
 * 
 * This function waits for a specified long duration to handle any additional FIN packets 
 * that may arrive. It ensures that the receiver properly finalizes the connection.
 */
void receiver_action_Wait_inCase(void) {
    // Check for any incoming FINs (just in-case)...
    char buffer[BUFFER_SIZE];
    clock_t time_elapsed_ms = (clock() - timer_start) * 1000 / CLOCKS_PER_SEC;

    ssize_t packet_size = recv(receiver_socket, buffer, sizeof(buffer), MSG_DONTWAIT);

    if (packet_size > 0 && is_FIN((struct protocol_Packet *)buffer))
    {
        receiver_current_state = Send_Fin_Ack;
    } 
    else if (time_elapsed_ms > LONG_TIMER_MS) 
    {
        receiver_current_state = Finished;
    } 
    else if ((packet_size == -1) && (errno != EAGAIN && errno != EWOULDBLOCK))
    {
        perror("Error with recv while waiting in-case.");
        receiver_current_state = Finished;
    }
}

/**
 * @brief Main function for the receiver application.
 * 
 * This function parses command line arguments to set up the UDP port and destination file.
 * It then calls the rrecv function to start the receiver process.
 *
 * @param argc Number of command-line arguments.
 * @param argv Array of command-line arguments.
 * @return Returns the exit status.
 */
int main(int argc, char** argv) {
    unsigned short int udpPort;
    char* filename = NULL;
    unsigned long long int writeRate = 0;
    if (argc != 3) {
        fprintf(stderr, "usage: %s UDP_port filename_to_write\n\n", argv[0]);
        exit(1);
    }

    udpPort = (unsigned short int) atoi(argv[1]);
    filename = argv[2];

    rrecv(udpPort, filename, writeRate);
}
