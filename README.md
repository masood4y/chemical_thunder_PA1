# Protocol README

Our protocol is built around a state-machine model for both the sender and the receiver, aiming for simplicity and efficiency. We've meticulously separated functionalities such as Sliding Window management, Round-Trip Time (RTT) calculations, and timeout handling from the core state-machine logic.

## Main Categories

The protocol states for both the sender and the receiver are organized into three main categories:

- **Connection Setup**
- **Data Exchange**
- **Connection Teardown**

### Connection Setup

#### Sender
- **Sender Start Connection State**: Initiates the connection by sending a SYNC bit (connection request) to the destination port, starts a timer, and waits for a response.
  
#### Receiver
- **Receiver Wait_Connection**: Waits for a connection request (SYNC bit), sets up UDP port, establishes receive window, and responds with SYNC_ACK.

### Data Exchange

#### Sender
- **Send_N_Packets**: Sends packets within the current window size, starts a timer for the first packet sent, and awaits acknowledgments.
- **Wait_for_ACK**: Waits for acknowledgments and adjusts the window size accordingly. Handles timeouts and duplicate acknowledgments.

#### Receiver
- **Wait_for_Packet**: Receives and buffers incoming packets, updates receive window, and sends cumulative acknowledgments.
- **Wait_for_Pipeline**: Waits for further packets or sends cumulative acknowledgment after a small timer.

### Connection Teardown

#### Sender
- **Send_FIN**: Initiates connection teardown by sending an empty packet with FIN = 1, and awaits acknowledgment.
- **Wait_FIN_Ack**: Waits for acknowledgment of the FIN packet or handles timeouts.

#### Receiver
- **Send FIN_ACK**: Sends acknowledgment for the FIN packet and initiates a long timer for any unexpected packets.
- **Wait_inCase**: Waits for the long timer to expire or handles incoming FIN packets.

## Sliding Window

We manage congestion control using a sliding window approach. 
- The Receiver expects the maximum theoretical number of bytes and considers any out-of-range byte as invalid or duplicate.
- The Sender adjusts its window size dynamically based on acknowledgments, timeouts, and duplicate acknowledgments. Initial size is set to one packet, and adjustments follow based on feedback.

## RTT Calculations

We employ rolling RTT calculations for timeout values by sampling the RTT of the first packet sent in a pipeline.

This protocol design ensures efficient and reliable data transmission while handling connection setup, data exchange, and teardown seamlessly.
