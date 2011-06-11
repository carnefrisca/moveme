// Authors: Mike Taylor and Kenny Hoff

#define _REENTRANT
#include <stdlib.h>
#include <stdio.h>
#include <sys/types.h>
#include <sys/time.h>
#include <fcntl.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/socket.h>
#include <string.h>
#include <netdb.h>
#include <errno.h>
#include "moveclient.h"

static MoveServerPacket client_move_server_packet;
static MoveServerCameraFrameSlicePacket client_move_server_camera_frame_slice_packet;
pthread_mutex_t move_state_mutex;

static int sendRequestPacket(uint32_t, uint32_t);
static int sendRequestPacketRumble(uint32_t, uint32_t);
static int sendRequestPacketForceRGB(uint32_t, float, float, float);
static int sendRequestPacketTrackHues(uint32_t, uint32_t, uint32_t, uint32_t);
static int sendRequestPacketPrepareCamera(uint32_t, float);

static void deserializeMoveServerPacketHeader(MoveServerPacketHeader *);
static void deserializeMoveServerPacket(MoveServerPacket *);

enum {
  TRUE = (1==1),
  FALSE = (1==0),
};


// Local variables
static int s_control_sockdf, s_transfer_sockfd;
static pthread_t s_update_move_state_thread;
static fd_set s_fd_read, s_fd_except;
static uint8_t s_connected = FALSE;
static uint8_t s_transferring = FALSE;


#define PACKET_PREP(_s, _cr) \
  _s packet_data; \
  int packet_size = sizeof(packet_data); \
  int payload_size = packet_size - sizeof(packet_data.header); \
  /* Setup header */ setupHeader(&packet_data.header, _cr, payload_size);


// Local functions
static inline void setupHeader(MoveServerRequestPacketHeader *header, uint32_t client_request, uint32_t payload_size) {
  header->client_request = htonl(client_request);
  header->payload_size = htonl(payload_size);

}


// Host to network float
float htonf(float f) {
  uint32_t tmp_int = *(uint32_t *)&f;
  tmp_int = htonl(tmp_int);
  f = *(float *)&tmp_int;

  return f;
}


// Network to host float
float ntohf(float f) {
  uint32_t tmp_int = *(uint32_t *)&f;
  tmp_int = ntohl(tmp_int);
  f = *(float *)&tmp_int;

  return f;
}


static inline int sendData(uint8_t *data_pointer, uint32_t data_size) {

  while (0 < data_size) {
    int bytes = send(s_control_sockdf, (void *)data_pointer, data_size, 0);

    if (0 > bytes) {
      break;

    }

    data_pointer += bytes;
    data_size -= bytes;

  }

  return errno;

}


static int sendRequestPacket(uint32_t client_request, uint32_t payload) {

  PACKET_PREP(MoveServerRequestPacket, client_request);

  // Serialize data
  packet_data.payload = htonl(payload);

  // Send
  return sendData((uint8_t *) &packet_data, packet_size);

}


static int sendRequestPacketRumble(uint32_t gem_num, uint32_t rumble) {

  PACKET_PREP(MoveServerRequestPacketRumble, MOVE_CLIENT_REQUEST_SET_RUMBLE);

  // Serialize data
  packet_data.gem_num = htonl(gem_num);
  packet_data.rumble = htonl(rumble);

  // Send
  return sendData((uint8_t *) &packet_data, packet_size);

}


static int sendRequestPacketForceRGB(uint32_t gem_num, float r, float g, float b) {

  PACKET_PREP(MoveServerRequestPacketForceRGB, MOVE_CLIENT_REQUEST_FORCE_RGB);

  // Serialize data
  packet_data.gem_num = htonl(gem_num);
  packet_data.r = htonf(r);
  packet_data.g = htonf(g);
  packet_data.b = htonf(b);

  // Send
  return sendData((uint8_t *) &packet_data, packet_size);

}


static int sendRequestPacketTrackHues(uint32_t req_hue_gem_0, uint32_t req_hue_gem_1, uint32_t req_hue_gem_2, uint32_t req_hue_gem_3) {

  PACKET_PREP(MoveServerRequestPacketTrackHues, MOVE_CLIENT_REQUEST_TRACK_HUES);

  // Serialize data
  packet_data.req_hue_gem_0 = htonl(req_hue_gem_0);
  packet_data.req_hue_gem_1 = htonl(req_hue_gem_1);
  packet_data.req_hue_gem_2 = htonl(req_hue_gem_2);
  packet_data.req_hue_gem_3 = htonl(req_hue_gem_3);

  // Send
  return sendData((uint8_t *) &packet_data, packet_size);

}


static int sendRequestPacketPrepareCamera(uint32_t max_exposure, float image_quality) {

  PACKET_PREP(MoveServerRequestPacketPrepareCamera, MOVE_CLIENT_REQUEST_PREPARE_CAMERA);

  // Serialize data
  packet_data.max_exposure = htonl(max_exposure);
  packet_data.image_quality = htonf(image_quality);

  // Send
  return sendData((uint8_t *) &packet_data, packet_size);

}

#undef PACKET_PREP


// Deserialize the move server packet header
void deserializeMoveServerPacketHeader(MoveServerPacketHeader *move_server_packet_header) {

  move_server_packet_header->magic = ntohl(move_server_packet_header->magic);
  move_server_packet_header->move_me_server_version = ntohl(move_server_packet_header->move_me_server_version);
  move_server_packet_header->packet_code = ntohl(move_server_packet_header->packet_code);
  move_server_packet_header->packet_index = ntohl(move_server_packet_header->packet_index);
  move_server_packet_header->packet_length = ntohl(move_server_packet_header->packet_length);

}


// Deserialize the move server packet
void deserializeMoveServerPacket(MoveServerPacket *move_server_packet) {
  int i, j;

  // Server Config
  move_server_packet->server_config.num_image_slices = ntohl(move_server_packet->server_config.num_image_slices);
  move_server_packet->server_config.image_slice_format = ntohl(move_server_packet->server_config.image_slice_format);

  // Connection Config
  move_server_packet->client_config.ms_delay_between_standard_packets = ntohl(move_server_packet->client_config.ms_delay_between_standard_packets);
  move_server_packet->client_config.ms_delay_between_camera_frame_packets = ntohl(move_server_packet->client_config.ms_delay_between_camera_frame_packets);
  move_server_packet->client_config.camera_frame_packet_paused = ntohl(move_server_packet->client_config.camera_frame_packet_paused);

  // Camera
  move_server_packet->camera_state.exposure = ntohl(move_server_packet->camera_state.exposure);
  move_server_packet->camera_state.exposure_time = ntohf(move_server_packet->camera_state.exposure_time);
  move_server_packet->camera_state.gain = ntohf(move_server_packet->camera_state.gain);
  move_server_packet->camera_state.pitch_angle = ntohf(move_server_packet->camera_state.pitch_angle);
  move_server_packet->camera_state.pitch_angle_estimate = ntohf(move_server_packet->camera_state.pitch_angle_estimate);

  // Nav
  for (i = 0; i < MOVE_SERVER_MAX_NAVS; i++) {
    move_server_packet->pad_info.port_status[i] = ntohl(move_server_packet->pad_info.port_status[i]);

    // NavPadData
    move_server_packet->pad_data[i].len = ntohl(move_server_packet->pad_data[i].len);

    for (j = 0; j < CELL_PAD_MAX_CODES; j++) {
      move_server_packet->pad_data[i].button[j] = ntohs(move_server_packet->pad_data[i].button[j]);

    }

  }

  // Gems
  for (i = 0; i < MOVE_SERVER_MAX_GEMS; i++) {

    // Status
    move_server_packet->status[i].connected = ntohl(move_server_packet->status[i].connected);
    move_server_packet->status[i].code = ntohl(move_server_packet->status[i].code);
    move_server_packet->status[i].flags = ntohll(move_server_packet->status[i].flags);

    if (move_server_packet->status[i].connected == 1) {

      // State
      for (j = 0; j < 4; j++) {
        move_server_packet->state[i].pos[j] = ntohf(move_server_packet->state[i].pos[j]);
        move_server_packet->state[i].vel[j] = ntohf(move_server_packet->state[i].vel[j]);
        move_server_packet->state[i].accel[j] = ntohf(move_server_packet->state[i].accel[j]);
        move_server_packet->state[i].quat[j] = ntohf(move_server_packet->state[i].quat[j]);
        move_server_packet->state[i].angvel[j] = ntohf(move_server_packet->state[i].angvel[j]);
        move_server_packet->state[i].angaccel[j] = ntohf(move_server_packet->state[i].angaccel[j]);
        move_server_packet->state[i].handle_pos[j] = ntohf(move_server_packet->state[i].handle_pos[j]);
        move_server_packet->state[i].handle_vel[j] = ntohf(move_server_packet->state[i].handle_vel[j]);
        move_server_packet->state[i].handle_accel[j] = ntohf(move_server_packet->state[i].handle_accel[j]);

      }

      move_server_packet->state[i].timestamp = (system_time_t)ntohll((uint64_t)move_server_packet->state[i].timestamp);
      move_server_packet->state[i].temperature = ntohf(move_server_packet->state[i].temperature);
      move_server_packet->state[i].camera_pitch_angle = ntohf(move_server_packet->state[i].camera_pitch_angle);
      move_server_packet->state[i].tracking_flags = ntohl(move_server_packet->state[i].tracking_flags);

      // Pad
      move_server_packet->state[i].pad.digital_buttons = ntohs(move_server_packet->state[i].pad.digital_buttons);
      move_server_packet->state[i].pad.analog_trigger = ntohs(move_server_packet->state[i].pad.analog_trigger);

      // Image State
      move_server_packet->image_state[i].frame_timestamp = (system_time_t)ntohll((uint64_t)move_server_packet->image_state[i].frame_timestamp);
      move_server_packet->image_state[i].timestamp = (system_time_t)ntohll((uint64_t)move_server_packet->image_state[i].timestamp);
      move_server_packet->image_state[i].u = ntohf(move_server_packet->image_state[i].u);
      move_server_packet->image_state[i].v = ntohf(move_server_packet->image_state[i].v);
      move_server_packet->image_state[i].r = ntohf(move_server_packet->image_state[i].r);
      move_server_packet->image_state[i].projectionx = ntohf(move_server_packet->image_state[i].projectionx);
      move_server_packet->image_state[i].projectiony = ntohf(move_server_packet->image_state[i].projectiony);
      move_server_packet->image_state[i].distance = ntohf(move_server_packet->image_state[i].distance);

      // Sphere State
      move_server_packet->sphere_state[i].tracking = ntohl(move_server_packet->sphere_state[i].tracking);
      move_server_packet->sphere_state[i].tracking_hue = ntohl(move_server_packet->sphere_state[i].tracking_hue);
      move_server_packet->sphere_state[i].r = ntohf(move_server_packet->sphere_state[i].r);
      move_server_packet->sphere_state[i].g = ntohf(move_server_packet->sphere_state[i].g);
      move_server_packet->sphere_state[i].b = ntohf(move_server_packet->sphere_state[i].b);

      // Pointer State
      move_server_packet->pointer_state[i].valid = ntohl(move_server_packet->pointer_state[i].valid);
      move_server_packet->pointer_state[i].normalized_x = ntohf(move_server_packet->pointer_state[i].normalized_x);
      move_server_packet->pointer_state[i].normalized_y = ntohf(move_server_packet->pointer_state[i].normalized_y);

      // Position Pointer State
      move_server_packet->position_pointer_state[i].valid = ntohl(move_server_packet->position_pointer_state[i].valid);
      move_server_packet->position_pointer_state[i].normalized_x = ntohf(move_server_packet->position_pointer_state[i].normalized_x);
      move_server_packet->position_pointer_state[i].normalized_y = ntohf(move_server_packet->position_pointer_state[i].normalized_y);

    }

  }

}


// Recv from PS3
int updateMoveState(void) {
  int rc, last_error, bytes;
  fd_set fd_read, fd_except;
  uint64_t buffer[MAX_UDP_MSG_SIZE / sizeof(uint64_t)];
  int packet_size = sizeof(MoveServerPacket);
  int camera_packet_size = sizeof(MoveServerCameraFrameSlicePacket);

  MoveServerPacketHeader *move_server_packet_header_ptr;
  struct timeval timeout;

  // Update loop
  while (s_transferring) {

    // Setup file_descriptors
    memcpy(&fd_read, &s_fd_read, sizeof(fd_read));
    memcpy(&fd_except, &s_fd_except, sizeof(fd_except));

    // Reset timeout
    memset((void *) &timeout, 0, sizeof(timeout)); 
    timeout.tv_usec = MOVE_TRANSFER_TIMEOUT;

    // Select
    rc = select(FD_SETSIZE, &fd_read, NULL, &fd_except, &timeout);

    if (0 < rc) {  // Check file descriptors

      if (FD_ISSET(s_transfer_sockfd, &fd_read)) {
        bytes = recvfrom(s_transfer_sockfd, buffer, MAX_UDP_MSG_SIZE, 0, 0, 0);

        if (0 > bytes ) {  // Recv error occurred
          last_error = errno;

          if ((EWOULDBLOCK != last_error) && s_transferring) {  // Ignore would-block
            printf("SOCKET ERROR: recvfrom() returned error #%d\n", last_error);

          }

        } else {
          pthread_mutex_lock(&move_state_mutex);

          // Get pointer to header in the buffer
          move_server_packet_header_ptr = (MoveServerPacketHeader *) &buffer[0];

          // Deserialize header
          deserializeMoveServerPacketHeader(move_server_packet_header_ptr);

          if (move_server_packet_header_ptr->packet_code == MOVE_PACKET_CODE_STANDARD) {
            memcpy(&client_move_server_packet, (void *)move_server_packet_header_ptr, packet_size);

            // Deserialize packet
            deserializeMoveServerPacket(&client_move_server_packet);

          }else if (move_server_packet_header_ptr->packet_code == MOVE_PACKET_CODE_CAMERA_FRAME_SLICE) {
            memcpy(&client_move_server_camera_frame_slice_packet, (void *)move_server_packet_header_ptr, camera_packet_size);

          }

          pthread_mutex_unlock(&move_state_mutex);
        }

        // Cleanup set
        FD_CLR(s_transfer_sockfd, &fd_read);

      }

      if (FD_ISSET(s_transfer_sockfd, &fd_except)) {

        if (s_transferring) {
          printf("SOCKET ERROR: select() set FD_EXCEPT with error #%d\n", errno);

        }

        // Cleanup set
        FD_CLR(s_transfer_sockfd, &fd_except);

      }

    }else if (0 > rc) {  // Select error occurred

      if (s_transferring) {
        printf("SOCKET ERROR: select() returned error #%d\n", errno);

      }

    }

  }

  return MOVE_CLIENT_OK;
}


static int stopTransfer(void) {
  int res;
  void *thread_result;

  if (!s_transferring)
    return MOVE_CLIENT_OK;

  // Update internal state
  s_transferring = FALSE;

  // Wait for thread to finish
  if ((res = pthread_join(s_update_move_state_thread, &thread_result))) {
    pthread_cancel(s_update_move_state_thread); // Best effort

  }

  return errno;

}


static int startTransfer(void) {
  int res;

  if (s_transferring) {
    return MOVE_CLIENT_OK;

  }

  // Update state
  s_transferring = TRUE;

  // Run update loop in a separate thread
  if ((res = pthread_create(&s_update_move_state_thread, NULL, (void *)&updateMoveState, NULL))) {
    stopTransfer();
    return MOVE_CLIENT_ERROR;

  }

  return MOVE_CLIENT_OK;

}


///////////////////
// Public functions
///////////////////

// PS3 Connect
int movemeConnect(const char *server_remote_address, const char *server_port) {
  int rc, last_error;
  unsigned int tsa_len;
  struct sockaddr_in service, transfer;
  struct addrinfo *result = NULL, *addrptr = NULL, hints;
  char remote_address[100], port[6];
  tsa_len = sizeof(transfer);

  if (s_connected) {
    return MOVE_CLIENT_OK;

  }

  if (0 != (rc = pthread_mutex_init(&move_state_mutex, NULL))) {
    return rc;

  }

  memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_protocol = IPPROTO_TCP;

  service.sin_family = AF_INET;
  service.sin_port = 0;
  service.sin_addr.s_addr = htonl(INADDR_ANY);
  memset(service.sin_zero, '\0', sizeof(service.sin_zero));

  strncpy(remote_address, server_remote_address, sizeof(remote_address));
  remote_address[sizeof(remote_address) - 1] = '\0';

  strncpy(port, server_port, sizeof(port));
  port[sizeof(port) - 1] = '\0';

  if (getaddrinfo(remote_address, port, &hints, &result)) {
    return errno;

  }

  // Create transfer socket
  if (-1 == (s_transfer_sockfd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP))) {
    freeaddrinfo(result);
    return errno;

  }

  // Attempt to connect to an address until one succeeds
  for (addrptr = result; addrptr != NULL; addrptr = addrptr->ai_next) {

    if ((s_control_sockdf = socket(addrptr->ai_family, addrptr->ai_socktype, addrptr->ai_protocol)) == -1) {
      freeaddrinfo(result);
      return errno;

    }

    // Connect to server
    if (connect(s_control_sockdf, addrptr->ai_addr, (int)addrptr->ai_addrlen) == -1) {
      close(s_control_sockdf);
      s_control_sockdf = -1;
      continue;

    }
    break;

  }

  freeaddrinfo(result);

  if (-1 == s_control_sockdf) {
    last_error = errno;
    close(s_transfer_sockfd);
    return last_error;

  }

  // Update internal state after all sockets successfully created
  s_connected = TRUE;

  // Initialize socket file descriptors for select
  FD_ZERO(&s_fd_read);
  FD_ZERO(&s_fd_except);
  FD_SET(s_transfer_sockfd, &s_fd_read);
  FD_SET(s_transfer_sockfd, &s_fd_except);

  // Bind transfer Socket
  if (bind(s_transfer_sockfd, (struct sockaddr *)&service, sizeof(service)) == -1) {
    last_error = errno;
    movemeDisconnect();
    return last_error;

  }

  // Change transfer socket mode to non-blocking
  int flags = fcntl(s_transfer_sockfd, F_GETFL, 0);

  if (-1 == fcntl(s_transfer_sockfd, F_SETFL, O_NONBLOCK|flags)) {
    last_error = errno;
    movemeDisconnect();
    return last_error;

  }

  // Get transfer socket port
  if (getsockname(s_transfer_sockfd, (struct sockaddr *)&transfer, &tsa_len)) {
    last_error = errno;
    movemeDisconnect();
    return last_error;

  }

  // Start UpdateMoveServer thread
  if ((rc = startTransfer())) {
    movemeDisconnect();
    return rc;

  }

  // Send initialization request
  if ((rc = sendRequestPacket(MOVE_CLIENT_REQUEST_INIT, ntohs(transfer.sin_port)))) {
    movemeDisconnect();
    return rc;

  }

  return MOVE_CLIENT_OK;

}


// PS3 Disconnect
int movemeDisconnect() {

  if (!s_connected) {
    return MOVE_CLIENT_OK;

  }

  // Update internal state
  s_connected = FALSE;

  // Cleanup mutex
  pthread_mutex_destroy(&move_state_mutex);

  // Stop UpdateMoveState thread
  if (s_transferring) {
    stopTransfer();

  }

  // Clear file descriptor
  FD_CLR(s_transfer_sockfd, &s_fd_read);
  FD_CLR(s_transfer_sockfd, &s_fd_except);

  // Best effort attempt to cleanly close tranfer socket
  close(s_transfer_sockfd);

  // Best effort attempt to cleanly close control socket
  shutdown(s_control_sockdf, 2);
  close(s_control_sockdf);

  return errno;

}


// Copy local client move state into app's copy
void movemeGetState(const int which_gem, MoveState *client_move_state, MoveStatus *client_move_status) {
  pthread_mutex_lock(&move_state_mutex);
	*client_move_status = client_move_server_packet.status[which_gem];
	*client_move_state = client_move_server_packet.state[which_gem];
  pthread_mutex_unlock(&move_state_mutex);

}


int movemePause(void) {

  if (!s_connected || !s_transferring) {
    return MOVE_CLIENT_ERROR;

  }

  return sendRequestPacket(MOVE_CLIENT_REQUEST_PAUSE, 0);

}


int movemeResume(void) {

  if (!s_connected || !s_transferring) {
    return MOVE_CLIENT_ERROR;

  }

  return sendRequestPacket(MOVE_CLIENT_REQUEST_RESUME, 0);

}


int movemePauseCamera(void) {

  if (!s_connected || !s_transferring) {
    return MOVE_CLIENT_ERROR;

  }

  return sendRequestPacket(MOVE_CLIENT_REQUEST_CAMERA_FRAME_PAUSE, 0);

}


int movemeResumeCamera(void) {

  if (!s_connected || !s_transferring) {
    return MOVE_CLIENT_ERROR;

  }

  return sendRequestPacket(MOVE_CLIENT_REQUEST_CAMERA_FRAME_RESUME, 0);

}


int movemeUpdateDelay(uint32_t delay) {

  if (!s_connected) {
    return MOVE_CLIENT_ERROR;

  }

  return sendRequestPacket(MOVE_CLIENT_REQUEST_DELAY_CHANGE, delay);

}


int movemeUpdateCameraDelay(uint32_t delay) {

  if (!s_connected) {
    return MOVE_CLIENT_ERROR;

  }

  return sendRequestPacket(MOVE_CLIENT_REQUEST_CAMERA_FRAME_DELAY_CHANGE, delay);

}


int movemeRumble(uint32_t gem_num, uint32_t rumble) {

  if (!s_connected) {
    return MOVE_CLIENT_ERROR;

  }

  return sendRequestPacketRumble(gem_num, rumble);

}


int movemeForceRGB(uint32_t gem_num, float r, float g, float b )
{
  if (!s_connected) {
    return MOVE_CLIENT_ERROR;

  }

  return sendRequestPacketForceRGB(gem_num, r, g, b);
}


int movemeTrackHues(uint32_t req_hue_gem_0, uint32_t req_hue_gem_1, uint32_t req_hue_gem_2, uint32_t req_hue_gem_3) {

  if (!s_connected) {
    return MOVE_CLIENT_ERROR;

  }

  return sendRequestPacketTrackHues(req_hue_gem_0, req_hue_gem_1, req_hue_gem_2, req_hue_gem_3);

}


int movemePrepareCamera(uint32_t max_exposure, float image_quality) {

  if (!s_connected) {
    return MOVE_CLIENT_ERROR;

  }

  return sendRequestPacketPrepareCamera(max_exposure, image_quality);

}


int movemeCameraSetNumSlices(uint32_t slices) {

  if (!s_connected) {
    return MOVE_CLIENT_ERROR;

  }

  return sendRequestPacket(MOVE_CLIENT_REQUEST_CAMERA_FRAME_CONFIG_NUM_SLICES, slices);

}
