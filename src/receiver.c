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


void rrecv(unsigned short int myUDPport, 
            char* destinationFile, 
            unsigned long long int writeRate) 
            {

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

int setup_file(char* destinationFile)
{
    /* Open file for writing.  */
    FILE *filePointer;
    filePointer = fopen(destinationFile, "wb+");

    if (filePointer == NULL) {
        perror("Error opening file.");
        return 0;
    }
    receiver_file = filePointer;
    return 1;
}
void setup_recv_window(void)
{
    next_needed_seq_num = 0;
    anticipate_next[0] = next_needed_seq_num;
    anticipate_next[1] = anticipate_next[0] + (MAX_WINDOW_SIZE - 1);
    received[0] = anticipate_next[1] + 1;
    received[1] = anticipate_next[0] - 1;
}


void receiver_finish(void) {
    
    // free this if it exists
    if (buffered_bytes != NULL)
    {
        free(buffered_bytes);
    }
    // Close the file.
    if (receiver_file != NULL) {
        fclose(receiver_file);
        receiver_file = NULL;
    }

    // Close the socket
    if (receiver_socket >= 0) {
        close(receiver_socket);
    }
}

/*Checks if incoming packet is valid SYNC packet. */
int is_SYNC(struct protocol_Packet *receive_buffer) {
    
    uint8_t SYNC_bit = receive_buffer->header.management_byte & 0x80; // SYNC is upper-most bit.
    return SYNC_bit == 0x80;
}

/* Checks if incoming packet is data packet */
int is_data(struct protocol_Packet *receive_buffer) 
{
    return receive_buffer->header.management_byte == 0;
}

/* Checks if incoming packet is valid FIN packet. */
int is_FIN(struct protocol_Packet *receive_buffer) {
    uint8_t FIN_bit = receive_buffer->header.management_byte & 0x2; // SYNC is upper-most bit.
    return FIN_bit == 0x2;
}


int is_duplicate(uint32_t seq_num) 
{
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
                //perror("Error connecting to sender.");
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
    } else if ((packet_size < 0)  && (errno != EAGAIN && errno != EWOULDBLOCK)) {
        perror("Error with recvfrom.");
        receiver_current_state = Finished;
    }
    // Otherwise, no data received. Stay in Wait_Connection.
}

void receiver_action_Wait_for_Packet(void) {
    // Check for any incoming packets...
    struct protocol_Packet receive_buffer;
    ssize_t bytes_received = recv(receiver_socket, &receive_buffer, 
                                    sizeof(struct protocol_Packet), MSG_DONTWAIT);


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
            perror("Error with sending SYNC_ACK.");
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





// TODO
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
void add_data_to_buffer(struct protocol_Packet *receive_buffer)
{

    uint32_t buffer_index;
    uint32_t local_seq_num = receive_buffer->header.seq_ack_num;
    uint16_t bytes_data_in_packet = receive_buffer->header.bytes_of_data;
    buffer_index = local_seq_num - next_needed_seq_num;
    if (buffer_index == 0) 
    {
        first_valid_buffer_index = 0;
    }

    for(int i = 0; i < bytes_data_in_packet; i++) 
    {
        buffer_index = local_seq_num - next_needed_seq_num;
        
        buffered_bytes[buffer_index] = receive_buffer->data[i];
        local_seq_num++;
    }
    if (buffer_index >= last_valid_buffer_index) 
    {
        last_valid_buffer_index = buffer_index;       
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
        receiver_current_state = Finished;
    }
    
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

int main(int argc, char** argv) {
    // This is a skeleton of a main function.
    // You should implement this function more completely
    // so that one can invoke the file transfer from the
    // command line.

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
