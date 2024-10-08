#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <fcntl.h>
#include <netdb.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <unistd.h>
#include <pthread.h>
#include <errno.h>
#include <time.h>
#include <math.h>
#include "our_protocol.h"

#define ALPHA 0.125
#define BETA 0.25

static unsigned int sender_current_state;
static unsigned long long int bytes_left_to_send;
static FILE *file_pointer;
static int sockfd;

static uint32_t acknowledged[2];
static uint32_t in_Flight[2];
static uint32_t current_window_size;
static double RTT_in_ms;
static double timeoutInterval_in_ms;
static double devRTT;
static uint8_t timer_valid;

static clock_t start, end;
static double cpu_time_used_in_seconds;
static double cpu_time_used_in_ms;
static long long int file_offset_for_sending;
static uint8_t duplicate_ack_count;
     
enum sender_state
{
    /* Connection Setup */
    Start_Connection,

    /* Send Data*/
    Send_N_Packets,
    Wait_for_Ack,

    /* Connection Teardown */
    Send_Fin,
    Wait_Fin_Ack,
    sender_Done
};

/* ================ Function Declarations Start ================ */
/* Initialization */
int sender_init(char* filename, unsigned long long int bytesToTransfer,
                        char* hostname, unsigned short int hostUDPport);
int open_file(char* filename, unsigned long long int bytesToTransfer); 
int setup_socket(char* hostname, unsigned short int hostUDPport);
void setup_cwindow(void);
void updateRTT(double sampleRTT);
void handle_timeout(void);

/* Closing file, socket, etc. */
void sender_finish(void);

/* Connection Setup */
void sender_action_Start_Connection(void);
int is_Sync_Ack(struct protocol_Header* receive_buffer);
void init_rtt(void);

/* Send Data*/
void sender_action_Send_N_Packets(void);
int valid_ack_num(uint32_t ack_num);
int sending_index_in_range(uint32_t sending_index);

void sender_action_Wait_for_Ack(void);
void increment_cwindow(void);
void half_cwindow(void);
void quarter_cwindow(void);

/* Connection Teardown */
void sender_action_Send_Fin(void);
void sender_action_Wait_Fin_Ack(void);
/* ================ Function Declarations END ================ */

/**
 * @brief Initializes the sender with the specified file, transfer size, hostname, and UDP port.
 *
 * This function sets up the file to be sent, initializes the socket for communication
 * with the receiver, and sets up the congestion window. It also initializes the sender's
 * state machine.
 *
 * @param filename The path to the file to be sent.
 * @param bytesToTransfer The number of bytes to transfer from the file.
 * @param hostname The hostname or IP address of the receiver.
 * @param hostUDPport The UDP port number of the receiver.
 * @return Returns 0 on successful initialization, -1 on failure.
 */
int sender_init(char* filename, unsigned long long int bytesToTransfer,
                        char* hostname, unsigned short int hostUDPport)
{
    sockfd = -1;
    /* File related initialization */
    if (open_file(filename, bytesToTransfer))
    {   
        printf("Could not open file\n");
        return -1;
    }


    /* Socket Set up for Listening and Sending to hostname */
    if (setup_socket(hostname, hostUDPport))
    {
        printf("Could not setup_socket\n");
        return -1;
    }

    setup_cwindow();
    /* Set up State machine */
    sender_current_state = Start_Connection;
    return 0;
}

/**
 * @brief Opens the file to be sent and calculates the bytes to transfer.
 *
 * This function opens the specified file for reading and sets the bytes_left_to_send
 * based on the file size and the requested bytes to transfer.
 *
 * @param filename The path to the file to be sent.
 * @param bytesToTransfer The number of bytes to transfer from the file.
 * @return Returns 0 on success, -1 on failure.
 */
int open_file(char* filename, unsigned long long int bytesToTransfer) 
{
    file_pointer = fopen(filename, "r");
    if (file_pointer == NULL) {
        fprintf(stderr, "Error: Could not open filename.\n");
        return -1;
    }
    
    /* Get File Size */
    long long file_size;
    
    fseek(file_pointer, 0, SEEK_END);
    file_size = ftell(file_pointer);
    fseek(file_pointer, 0, SEEK_SET);
    file_offset_for_sending = 0;
    if (file_size < 1) 
    {
        printf("File too small\n");
        return -1;
    }
    
    /* Set bytes_left_to_send as MIN(bytesToTransfer, Filesize) */
    if ((unsigned long long int)file_size <= bytesToTransfer) {
        bytes_left_to_send = (unsigned long long int)(file_size);
    }
    else {
        bytes_left_to_send = bytesToTransfer;
    }
    if (bytes_left_to_send == 0) 
    {
        return -1;
    }
    return 0;
}

/**
 * @brief Sets up the UDP socket for communication with the receiver.
 *
 * This function creates a UDP socket, resolves the receiver's hostname to an IP address,
 * and connects the socket to the receiver's address and port. It also sets the socket
 * to non-blocking mode.
 *
 * @param hostname The hostname or IP address of the receiver.
 * @param hostUDPport The UDP port number of the receiver.
 * @return Returns 0 on success, -1 on failure.
 */
int setup_socket(char* hostname, unsigned short int hostUDPport) {

    struct sockaddr_in receiver_address;
    struct hostent *host;
    sockfd = -1;

    /* Resolve the hostname */
    host = gethostbyname(hostname);
    if (host == NULL) {
        fprintf(stderr, "Error: Could not resolve hostname.\n");
        return -1;
    }

    /* Create a UDP socket */
    sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd < 0) {
        perror("Error opening socket");
        return -1;
    }

    /* Set up the receiver struct */
    memset(&receiver_address, 0, sizeof(receiver_address));
    receiver_address.sin_family = AF_INET;
    receiver_address.sin_port = htons(hostUDPport);
    memcpy(&receiver_address.sin_addr, host->h_addr_list[0], host->h_length);

    /* Connect other end of socket to the Receiver */ 
    if (connect(sockfd, (struct sockaddr *)&receiver_address, sizeof(receiver_address)) < 0) {
        perror("Error connecting to server");
        close(sockfd);
        return -1;
    }

    /* Set the socket to non-blocking mode for receiving */
    int flags = fcntl(sockfd, F_GETFL, 0);
    if (flags == -1) {
        perror("Error getting socket flags");
        return -1;
    }
    if (fcntl(sockfd, F_SETFL, flags | O_NONBLOCK) == -1) {
        perror("Error setting socket to non-blocking mode");
        return -1;
    }
    return 0;
}

/**
 * @brief Sets up the congestion window for the sender.
 *
 * Initializes the congestion window size and the tracking arrays for in-flight and
 * acknowledged packets. The window size is set to either the protocol data size or the
 * remaining bytes to send, whichever is smaller.
 */
void setup_cwindow(void)
{
    /* Initialize to 1 packet. */
    current_window_size = PROTOCOL_DATA_SIZE;
    if (bytes_left_to_send < current_window_size)
    {
        current_window_size = bytes_left_to_send;
    }

    in_Flight[0] = 0;
    in_Flight[1] = in_Flight[0] + (current_window_size - 1);
    acknowledged[0] = in_Flight[1] + 1;
    acknowledged[1] = in_Flight[0] - 1;
    duplicate_ack_count = 0;
}

/**
 * @brief Updates the Round-Trip Time (RTT) estimates.
 *
 * Uses the sample RTT value to update the estimated RTT and the deviation (devRTT),
 * and recalculates the timeout interval accordingly.
 *
 * @param sampleRTT The sampled RTT value for the latest acknowledged packet.
 */
void updateRTT(double sampleRTT) {
    // Update estimated RTT using new sample RTT value.
    RTT_in_ms = (1- ALPHA) * RTT_in_ms + ALPHA * sampleRTT;
    
    // Update "safety margin" for timeout intervals.
    devRTT = (1 - BETA) * devRTT + BETA * fabs(sampleRTT - RTT_in_ms);

    // Update timeout interval using this newly estimated RTT and safety margin.
    timeoutInterval_in_ms = RTT_in_ms + 4 * devRTT;
    
    timer_valid = 0;
}

/**
 * @brief Handles the timeout event.
 *
 * Doubles the timeout interval as part of the exponential backoff strategy in case
 * of a timeout event.
 */
void handle_timeout(void) {
    // Exponential backoff, double the timeout interval
     timeoutInterval_in_ms *= 2;
     // We could also double the deviation, but this might be more than we need for now
     // devRTT *= 2;
}

/**
 * @brief Initiates the connection setup process by sending a SYNC packet.
 *
 * Sends a SYNC packet to the receiver to start the connection setup. After sending,
 * it waits for a SYNC_ACK response from the receiver and transitions the sender's state
 * machine based on the response.
 */
void sender_action_Start_Connection(void)
{
    /* send SYNC = 1 to receiver */ 
    struct protocol_Packet sync_packet;

    /* Sync bit:7, Sync Ack bit:6, 0:5, 0:4, 0:3, 0:2, Fin bit:1, Fin ack bit:0 */
    memset(&sync_packet, 0, sizeof(sync_packet));
    sync_packet.header.management_byte = sync_packet.header.management_byte | 0x80;

    ssize_t bytes_sent = send(sockfd, &sync_packet, sizeof(struct protocol_Packet), 0);

    if (bytes_sent < 0) {
        perror("Error sending data");
        sender_current_state = sender_Done;
        return;
    }
    
    /* Start 2 second timer */
    start = clock();
    while(1)
    {
        end = clock();
        cpu_time_used_in_ms = ((double) (end - start)) / (CLOCKS_PER_SEC / 1000);

        /* Check Socket for response */
        struct protocol_Header receive_buffer;
        ssize_t bytes_received = recv(sockfd, &receive_buffer, 512, MSG_DONTWAIT);
        if (bytes_received > 0) 
        {
            /* If its a Sync Ack*/            
            if (is_Sync_Ack(&receive_buffer)) 
            {
                //TODO: set up sliding window, current packet size, RTT?
                init_rtt();

                sender_current_state = Send_N_Packets;
                break;
            }
        } 
        else if (bytes_received == 0) 
        {
            printf("Connection closed by peer\n");
            sender_current_state = sender_Done;
            break;
        } 
        /* Check if the error is due to the socket being non-blocking */
        else if ((bytes_received == -1) && (errno != EAGAIN && errno != EWOULDBLOCK))
        {
            perror("Error receiving data");
            sender_current_state = sender_Done;
            break;
        }

        /* Check Timer for timeout */
        else if (cpu_time_used_in_ms >= 2000)
        {
            break;
        }
    }
    return;    
}

/**
 * @brief Checks if the received packet is a valid SYNC_ACK packet.
 *
 * Verifies if the SYNC_ACK bit is set in the management byte of the received packet header.
 *
 * @param receive_buffer Pointer to the received protocol header.
 * @return Returns 1 if it's a SYNC_ACK packet, 0 otherwise.
 */
int is_Sync_Ack(struct protocol_Header* receive_buffer)
{
    return ((receive_buffer->management_byte & 0x40) == 0x40);
}

/**
 * @brief Initializes Round-Trip Time (RTT) values based on the initial measurement.
 *
 * Sets the initial RTT, deviation, and timeout interval values based on the first
 * measured RTT.
 */
void init_rtt(void) 
{
    RTT_in_ms = cpu_time_used_in_ms * 6;
    devRTT = RTT_in_ms /2;
    timeoutInterval_in_ms = RTT_in_ms + (4 * devRTT);
    timer_valid = 0;
}

/**
 * @brief Handles the process of sending a number of data packets.
 *
 * Reads data from the file and sends packets sequentially according to the current
 * congestion window. Updates the sender's state machine to wait for acknowledgments.
 */
void sender_action_Send_N_Packets(void) 
{
    uint32_t sending_index;
    struct protocol_Packet packet_being_sent;
    sending_index = in_Flight[0];
    
    while (sending_index_in_range(sending_index))
    {
        unsigned int i;
        memset(&packet_being_sent, 0, sizeof(packet_being_sent));
        packet_being_sent.header.seq_ack_num = sending_index;

        for (i = 0; (i < PROTOCOL_DATA_SIZE) && (sending_index_in_range(sending_index)); i++, sending_index++)
        {
            packet_being_sent.data[i] = fgetc(file_pointer);
        }
        
        if (i < PROTOCOL_DATA_SIZE) {
            for (unsigned int j = i; j < PROTOCOL_DATA_SIZE; j++){
                packet_being_sent.data[j] = EOF;
            }
        }
        packet_being_sent.header.bytes_of_data = i;
        ssize_t bytes_sent = send(sockfd, &packet_being_sent, sizeof(struct protocol_Packet), 0);
        
        if (bytes_sent == -1){
            sender_current_state = sender_Done;
            break;
        }
        if (!timer_valid)
        {
            start = clock();
            timer_valid = 1;
        }
    }
    sender_current_state = Wait_for_Ack;
    return;
}

/**
 * @brief Checks if a given acknowledgment number is valid.
 *
 * Validates the acknowledgment number against the range of sequence numbers in the
 * current congestion window.
 *
 * @param ack_num The acknowledgment number to validate.
 * @return Returns 1 if the acknowledgment number is valid, 0 otherwise.
 */
int valid_ack_num(uint32_t ack_num) 
{

    if (in_Flight[0] < in_Flight[1]){
        if ((ack_num > in_Flight[0]) && (ack_num <= (in_Flight[1] + 1))){
            return 1;
        }
        return 0;
    }

    else if (in_Flight[0] > in_Flight[1])
    {   
        if ((ack_num > in_Flight[0]) || (ack_num <= (in_Flight[1] + 1)))
        {
            return 1;
        }
        return 0;
    }
    else if (in_Flight[0] == in_Flight[1]) 
    {
        if (ack_num == (in_Flight[1] + 1))
        {
            return 1;
        }
        return 0;
    }
    return 0;
}

/**
 * @brief Checks if the sending index is within the current congestion window range.
 *
 * Determines if a given index for sending data is within the bounds of the current
 * congestion window.
 *
 * @param sending_index The index to be checked.
 * @return Returns 1 if the index is within range, 0 otherwise.
 */
int sending_index_in_range(uint32_t sending_index)
{

    if (in_Flight[1] > in_Flight[0]) {
        return ((sending_index <= in_Flight[1]) && (sending_index >= in_Flight[0]));
    }
    else if (in_Flight[0] > in_Flight[1])
    {
        return ((sending_index <= in_Flight[1]) || (sending_index >= in_Flight[0]));
    }
    else if (in_Flight[0] == in_Flight[1])
    {
        return (in_Flight[0] == sending_index); 
    }
    return 0;
}

/**
 * @brief Handles the state of waiting for packet acknowledgments.
 *
 * Monitors for incoming acknowledgments, updates the congestion window, and handles
 * timeout events. Transitions the sender's state machine based on received
 * acknowledgments or timeouts.
 */
void sender_action_Wait_for_Ack(void)
{
    if (bytes_left_to_send == 0) {
        sender_current_state = Send_Fin;
        return;
    }

    while(1)
    {
        end = clock();
        cpu_time_used_in_ms = ((double) (end - start)) / (CLOCKS_PER_SEC / 1000 );
        
        /* Check Socket for response */
        struct protocol_Header receive_buffer;
        ssize_t bytes_received = recv(sockfd, &receive_buffer, sizeof(struct protocol_Header), MSG_DONTWAIT);
        if (bytes_received > 0) 
        {
            /* If its a Valid Seq number */
            uint32_t ack_num = receive_buffer.seq_ack_num;
            if (valid_ack_num(ack_num)) 
            {
                updateRTT(cpu_time_used_in_ms);
                uint32_t old_acked = acknowledged[1];
                acknowledged[1] = ack_num - 1;
                
                // update bytes left, if bytes left to send == 0, goto Send_FIN
                uint32_t gained = (acknowledged[1] - old_acked);
		        bytes_left_to_send = bytes_left_to_send - (gained);
                in_Flight[0] = ack_num;
                if (bytes_left_to_send == 0){
                    sender_current_state = Send_Fin;
                    break;
                }

                file_offset_for_sending = file_offset_for_sending + (gained);
                fseek(file_pointer, file_offset_for_sending, SEEK_SET);
                                
                //update current window size based on bytes left, AMID, theoretical max
                increment_cwindow();

                sender_current_state = Send_N_Packets;
                break;
            }
            
            /* If its a Duplicate Ack */
            else 
            {
                duplicate_ack_count++;
                if (duplicate_ack_count >= 3)
                {
                    half_cwindow();
                }
            }
        } 
        else if (bytes_received == 0) 
        {
            printf("Connection closed by peer\n");
            sender_current_state = sender_Done;
            break;
        } 
        /* Check if the error is due to the socket being non-blocking */
        else if ((bytes_received == -1) && (errno != EAGAIN && errno != EWOULDBLOCK))
        {
            perror("Error receiving data");
            sender_current_state = sender_Done;
            break;
        }

        if(cpu_time_used_in_ms > timeoutInterval_in_ms) //TODO: figure out time to use
        {
            quarter_cwindow();
            handle_timeout();
            
            fseek(file_pointer, file_offset_for_sending, SEEK_SET);
            sender_current_state = Send_N_Packets;
            break;
        }
    }
    return;
}

/**
 * @brief Increases the current congestion window size.
 *
 * Expands the size of the congestion window incrementally based on successful
 * packet transmissions, up to a maximum window size.
 */
void increment_cwindow(void){
    
    if (current_window_size < MAX_WINDOW_SIZE) 
    {
        current_window_size = current_window_size + PROTOCOL_DATA_SIZE - (current_window_size % PROTOCOL_DATA_SIZE);
    }
    else if (current_window_size >= MAX_WINDOW_SIZE)
    {
        current_window_size = MAX_WINDOW_SIZE;
    }
    if (bytes_left_to_send < current_window_size){
        current_window_size = bytes_left_to_send;
    }
    duplicate_ack_count = 0;
    in_Flight[1] = in_Flight[0] + (current_window_size - 1);
    acknowledged[0] = in_Flight[1] + 1;
}

/**
 * @brief Halves the current congestion window size.
 *
 * Reduces the current window size by half to adjust the flow control in response to network conditions.
 * It ensures that the window size aligns with the protocol data size and does not exceed the bytes left to send.
 */
void half_cwindow(void)
{
    current_window_size = current_window_size/2;
    if ((current_window_size % PROTOCOL_DATA_SIZE) != 0)
    {
        current_window_size = current_window_size + PROTOCOL_DATA_SIZE - (current_window_size % PROTOCOL_DATA_SIZE);
    }
    if (bytes_left_to_send < current_window_size)
    {
        current_window_size = bytes_left_to_send;
    }
    in_Flight[1] = in_Flight[0] + (current_window_size - 1);
    acknowledged[0] = in_Flight[1] + 1;
    duplicate_ack_count = 0;
}

/**
 * @brief Reduces the current congestion window size to a quarter of its size.
 *
 * Drastically decreases the current window size to a quarter in response to network events like triple duplicate ACKs.
 * Adjusts the window size to align with the protocol data size and ensures it does not exceed the remaining bytes to send.
 */
void quarter_cwindow(void)
{
    current_window_size = current_window_size/4;
    if ((current_window_size % PROTOCOL_DATA_SIZE) != 0)
    {
        current_window_size = current_window_size + PROTOCOL_DATA_SIZE - (current_window_size % PROTOCOL_DATA_SIZE);
    }
    if (bytes_left_to_send < current_window_size)
    {
        current_window_size = bytes_left_to_send;
    }
    in_Flight[1] = in_Flight[0] + (current_window_size - 1);
    acknowledged[0] = in_Flight[1] + 1;
    duplicate_ack_count = 0;
}

/**
 * @brief Initiates the connection teardown process by sending a FIN packet.
 *
 * Constructs and sends a FIN packet to the receiver to signal the end of data transmission.
 * Transitions the sender's state to waiting for a FIN_ACK response.
 */
void sender_action_Send_Fin(void)
{
    /* send FIN = 1 to receiver */ 
    struct protocol_Packet fin_packet;

    /* Sync bit:7, Sync Ack bit:6, 0:5, 0:4, 0:3, 0:2, Fin bit:1, Fin ack bit:0 */
    fin_packet.header.management_byte = 0;
    fin_packet.header.seq_ack_num = 0;
    fin_packet.header.management_byte = fin_packet.header.management_byte | 0x02;

    ssize_t bytes_sent = send(sockfd, &fin_packet, sizeof(struct protocol_Packet), 0);
    printf("Sending Fin Packet\n");

    if (bytes_sent < 0) {
        perror("Error sending data");
        sender_current_state = sender_Done;
        return;
    }
    start = clock();

    sender_current_state = Wait_Fin_Ack;
    return;
}

/**
 * @brief Waits for a FIN_ACK packet from the receiver.
 *
 * In this state, the sender waits for a FIN_ACK packet indicating that the receiver has acknowledged the end of transmission.
 * If a FIN_ACK is received or a timeout occurs, it transitions to the appropriate next state.
 */
void sender_action_Wait_Fin_Ack(void)
{
    // Wait_FIN_Ack: do nothing/wait
    //         if (timeout), goto: Send_FIN
    //         else if (FIN_ACK = 1 received), done 
    
    while(1)
    {
        end = clock();
        cpu_time_used_in_seconds = ((double) (end - start)) / CLOCKS_PER_SEC;

        /* Check Socket for response */
        struct protocol_Header receive_buffer;
        ssize_t bytes_received = recv(sockfd, &receive_buffer, sizeof(struct protocol_Header), MSG_DONTWAIT);
        if (bytes_received > 0) 
        {
            /* If its a Fin Ack*/
            if ((receive_buffer.management_byte & 0x1) == 0x1) {
                sender_current_state = sender_Done;
                break;
            }
        } 
        else if (bytes_received == 0) 
        {
            printf("Connection closed by peer\n");
            sender_current_state = sender_Done;
            break;
        } 
        /* Check if the error is due to the socket being non-blocking */
        else if ((bytes_received == -1) && (errno != EAGAIN && errno != EWOULDBLOCK))
        {
            perror("Error receiving data\n");
            sender_current_state = sender_Done;
            break;
        }

        /* Check Timer for timeout */ 
        else if (cpu_time_used_in_seconds >= 2)
        {   
            sender_current_state = Send_Fin;
            break;
        }
    }
    return;    
}

/**
 * @brief Cleans up resources used by the sender.
 *
 * This function closes the socket and the file associated with the sender. It is used
 * to clean up resources before the sender shuts down.
 */
void sender_finish(void){
    if (sockfd != -1) {
        close(sockfd);
    }
    if (file_pointer != NULL)
    {
        fclose(file_pointer);
    }
}

/**
 * @brief Main state machine function for the sender.
 *
 * This function initializes the sender with the specified file, transfer size, hostname,
 * and UDP port. It then enters a state machine loop, handling various states like
 * connection setup, data sending, waiting for acknowledgments, and connection teardown.
 *
 * @param hostname The hostname or IP address of the receiver.
 * @param hostUDPport The UDP port number of the receiver.
 * @param filename The path to the file to be sent.
 * @param bytesToTransfer The number of bytes to transfer from the file.
 */
void rsend(char* hostname, 
            unsigned short int hostUDPport, 
            char* filename, 
            unsigned long long int bytesToTransfer) 
{
    if (sender_init(filename, bytesToTransfer,hostname, hostUDPport))
    {   
        sender_finish();
        return;
    }

    while (sender_current_state != sender_Done) 
    {
        //TODO: figure out how to break from while(1) loop at the end
        switch (sender_current_state) 
        {
            /* Connection Setup */
            case Start_Connection:
                sender_action_Start_Connection();
                break;


            /* Send Data*/
            case Send_N_Packets:
                sender_action_Send_N_Packets();
                break;

            case Wait_for_Ack:
                sender_action_Wait_for_Ack();
                break;


            /* Connection Teardown */
            case Send_Fin:
                sender_action_Send_Fin();
                break;

            case Wait_Fin_Ack:
                sender_action_Wait_Fin_Ack();
                break;

            default: 
        }    
    }
    sender_finish();
    return;
}

/**
 * @brief Main function for the sender application.
 *
 * Parses command-line arguments to set up the receiver's hostname, UDP port, file to send,
 * and the number of bytes to transfer. Then calls the rsend function to start the sending process.
 *
 * @param argc Number of command-line arguments.
 * @param argv Array of command-line arguments.
 * @return Returns the exit status.
 */
int main(int argc, char** argv) {

    int hostUDPport;
    unsigned long long int bytesToTransfer;
    char* hostname = NULL;
    char* filename = NULL;

    if (argc != 5) {
        fprintf(stderr, "usage: %s receiver_hostname receiver_port filename_to_xfer bytes_to_xfer\n\n", argv[0]);
        exit(1);
    }
    hostUDPport = (unsigned short int) atoi(argv[2]);
    hostname = argv[1];
    filename = argv[3];
    bytesToTransfer = atoll(argv[4]);

    rsend(hostname, hostUDPport, filename, bytesToTransfer);

    return (EXIT_SUCCESS);
}
