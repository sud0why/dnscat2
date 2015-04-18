/* session.c
 * By Ron Bowes
 *
 * See LICENSE.md
 */

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

#ifndef WIN32
#include <unistd.h>
#endif

#include "libs/log.h"
#include "libs/memory.h"
#include "libs/select_group.h"
#include "packet.h"

#include "session.h"

/* Set to TRUE after getting the 'shutdown' message. */
/* static NBBOOL is_shutdown = FALSE;*/

/* Allow the user to override the initial sequence number. */
static uint32_t isn = 0xFFFFFFFF;

/* Enable/disable packet tracing. */
static NBBOOL packet_trace;

#define RETRANSMIT_DELAY 1000 /* Milliseconds */

/* Allow anything to go out. Call this at the start or after receiving legit data. */
#if 0
static void reset_counter(session_t *session)
{
  session->last_transmit = 0;
}
#endif

static double time_ms()
{
  struct timeval tv;
  gettimeofday(&tv, NULL);
  return (tv.tv_sec) * 1000 + (tv.tv_usec) / 1000 ;
}

/* Wait for a delay or incoming data before retransmitting. Call this after transmitting data. */
static void update_counter(session_t *session)
{
  session->last_transmit = time_ms();
}

/* Decide whether or not we should transmit data yet. */
static NBBOOL can_i_transmit_yet(session_t *session)
{
  /*printf("%d\n", (int)(time_ms() - session->last_transmit));*/
  if(time_ms() - session->last_transmit > RETRANSMIT_DELAY)
    return TRUE;
  return FALSE;
}

/* Polls the driver for data and puts it in our own buffer. This is necessary
 * because the session needs to ACK data and such. */
static void poll_for_data(session_t *session)
{
  size_t length = -1;

  /* Read all the data we can. */
  uint8_t *data = driver_get_outgoing(session->driver, &length, -1);

  /* If there's no data left, go into 'shutdown' mode. */
  if(!data)
  {
    if(buffer_get_remaining_bytes(session->outgoing_buffer) == 0)
      session->is_shutdown = TRUE;
  }
  else
  {
    if(length)
      buffer_add_bytes(session->outgoing_buffer, data, length);
  }

  safe_free(data);
}

uint8_t *session_get_outgoing(session_t *session, size_t *length, size_t max_length)
{
  packet_t *packet      = NULL;
  uint8_t  *result      = NULL;
  uint8_t  *data        = NULL;
  size_t    data_length = -1;

  /* Suck in any data we can from the driver. */
  poll_for_data(session);

  /* Don't transmit too quickly without receiving anything. */
  if(!can_i_transmit_yet(session))
  {
    LOG_INFO("Retransmission timer hasn't expired, not re-sending...");
    return NULL;
  }

  switch(session->state)
  {
    case SESSION_STATE_NEW:
      LOG_INFO("In SESSION_STATE_NEW, sending a SYN packet (SEQ = 0x%04x)...", session->my_seq);

      packet = packet_create_syn(session->id, session->my_seq, (options_t)0);

#if 0
      if(session->name)
        packet_syn_set_name(packet, session->name);
      if(session->download)
        packet_syn_set_download(packet, session->download);
      if(session->download_first_chunk)
        packet_syn_set_chunked_download(packet);
      if(session->is_command)
        packet_syn_set_is_command(packet);
#endif

      update_counter(session);
      result = packet_to_bytes(packet, length, session->options);
      packet_destroy(packet);

      break;

    case SESSION_STATE_ESTABLISHED:
      /* Read data without consuming it (ie, leave it in the buffer till it's ACKed) */
      data = buffer_read_remaining_bytes(session->outgoing_buffer, &data_length, max_length - packet_get_msg_size(session->options), FALSE);
      LOG_INFO("In SESSION_STATE_ESTABLISHED, sending a MSG packet (SEQ = 0x%04x, ACK = 0x%04x, %zd bytes of data...", session->my_seq, session->their_seq, data_length);

      /* TODO: Create a FIN if we're shut down. */
      if(data_length == 0 && session->is_shutdown)
        packet = packet_create_fin(session->id, "Stream closed");
      else
        packet = packet_create_msg_normal(session->id, session->my_seq, session->their_seq, data, data_length);

      safe_free(data);

      update_counter(session);
      result = packet_to_bytes(packet, length, session->options);
      packet_destroy(packet);

      break;

    default:
      LOG_FATAL("Wound up in an unknown state: 0x%x", session->state);
      exit(1);
  }

  return result;
}

void session_data_incoming(session_t *session, uint8_t *data, size_t length)
{
  /* Parse the packet to get the session id */
  packet_t *packet = packet_parse(data, length, session->options);

  /* Suck in any data we can from the driver. */
  poll_for_data(session);

  switch(session->state)
  {
    case SESSION_STATE_NEW:
      if(packet->packet_type == PACKET_TYPE_SYN)
      {
        LOG_INFO("In SESSION_STATE_NEW, received SYN (ISN = 0x%04x)", packet->body.syn.seq);
        session->their_seq = packet->body.syn.seq;
        session->options   = (options_t) packet->body.syn.options;
        session->state = SESSION_STATE_ESTABLISHED;
      }
      else if(packet->packet_type == PACKET_TYPE_MSG)
      {
        LOG_WARNING("In SESSION_STATE_NEW, received unexpected MSG (ignoring)");
      }
      else if(packet->packet_type == PACKET_TYPE_FIN)
      {
        /* TODO: I shouldn't exit here. */
        LOG_FATAL("In SESSION_STATE_NEW, received FIN: %s", packet->body.fin.reason);

        exit(0);
      }
      else
      {
        /* TODO: I shouldn't exit here. */
        LOG_FATAL("Unknown packet type: 0x%02x", packet->packet_type);
        exit(1);
      }

      break;
    case SESSION_STATE_ESTABLISHED:
      if(packet->packet_type == PACKET_TYPE_SYN)
      {
        LOG_WARNING("In SESSION_STATE_ESTABLISHED, recieved SYN (ignoring)");
      }
      else if(packet->packet_type == PACKET_TYPE_MSG)
      {
        LOG_INFO("In SESSION_STATE_ESTABLISHED, received a MSG");

        /* Validate the SEQ */
        if(packet->body.msg.options.normal.seq == session->their_seq)
        {
          /* Verify the ACK is sane */
          uint16_t bytes_acked = packet->body.msg.options.normal.ack - session->my_seq;

          /* If there's still bytes waiting in the buffer.. */
          if(bytes_acked <= buffer_get_remaining_bytes(session->outgoing_buffer))
          {
            /* Reset the retransmit counter since we got some valid data. */
            /*reset_counter(session);*/

            /* Increment their sequence number */
            session->their_seq = (session->their_seq + packet->body.msg.data_length) & 0xFFFF;

            /* Remove the acknowledged data from the buffer */
            buffer_consume(session->outgoing_buffer, bytes_acked);

            /* Increment my sequence number */
            if(bytes_acked != 0)
            {
              session->my_seq = (session->my_seq + bytes_acked) & 0xFFFF;
            }

            /* Print the data, if we received any, and then immediately receive more. */
            if(packet->body.msg.data_length > 0)
            {
              driver_data_received(session->driver, packet->body.msg.data, packet->body.msg.data_length);
            }
          }
          else
          {
            LOG_WARNING("Bad ACK received (%d bytes acked; %d bytes in the buffer)", bytes_acked, buffer_get_remaining_bytes(session->outgoing_buffer));
            packet_destroy(packet);
            return;
          }
        }
        else
        {
          LOG_WARNING("Bad SEQ received (Expected %d, received %d)", session->their_seq, packet->body.msg.options.normal.seq);
          packet_destroy(packet);
          return;
        }
      }
      else if(packet->packet_type == PACKET_TYPE_FIN)
      {
        LOG_FATAL("In SESSION_STATE_ESTABLISHED, received FIN: %s - closing session", packet->body.fin.reason);
        session->is_shutdown = TRUE;
        driver_close(session->driver);
      }
      else
      {
        LOG_FATAL("Unknown packet type: 0x%02x - closing session", packet->packet_type);
        session->is_shutdown = TRUE;
        driver_close(session->driver);
      }

      break;
    default:
      LOG_FATAL("Wound up in an unknown state: 0x%x", session->state);
      packet_destroy(packet);
      session->is_shutdown = TRUE;
      driver_close(session->driver);
      exit(1);
  }

  packet_destroy(packet);
}

#if 0
static void do_send_packet(session_t *session, packet_t *packet)
{
  size_t length;
  uint8_t *data = packet_to_bytes(packet, &length, session->options);

  /* Display if appropriate. */
  if(packet_trace)
  {
    printf("OUTGOING: ");
    packet_print(packet, session->options);
  }

  /* TODO: Do something with the data */
  message_post_packet_out(data, length);

  safe_free(data);
}
#endif

void session_destroy(session_t *session)
{
  if(session->name)
    safe_free(session->name);
  if(session->download)
    safe_free(session->download);

  safe_free(session);
}

static session_t *session_create(char *name)
{
  session_t *session     = (session_t*)safe_malloc(sizeof(session_t));

  session->id            = rand() % 0xFFFF;

  /* Check if it's a 16-bit value (I set it to a bigger value to set a random isn) */
  if(isn == (isn & 0xFFFF))
    session->my_seq        = (uint16_t) isn; /* Use the hardcoded one. */
  else
    session->my_seq        = rand() % 0xFFFF; /* Random isn */

  session->state         = SESSION_STATE_NEW;
  session->their_seq     = 0;
  session->is_shutdown   = FALSE;

  session->last_transmit = 0;
  session->outgoing_buffer = buffer_create(BO_LITTLE_ENDIAN);

  session->name = NULL;
  if(name)
  {
    session->name = safe_strdup(name);
    LOG_INFO("Setting session->name to %s", session->name);
  }

#if 0
  session->download               = NULL;
  session->download_first_chunk   = 0;
  session->download_current_chunk = 0;
  session->is_command             = FALSE
#endif

  return session;
}

session_t *session_create_console(select_group_t *group, char *name)
{
  session_t *session     = session_create(name);

  session->driver = driver_create(DRIVER_TYPE_CONSOLE, driver_console_create(group));

  return session;
}

#if 0
static void handle_data_out(uint16_t session_id, uint8_t *data, size_t length)
{
  session_t *session = sessions_get_by_id(session_id);
  if(!session)
  {
    LOG_ERROR("Tried to access a non-existent session (handle_data_out): %d", session_id);
    return;
  }

  /* Add the bytes to the outgoing data buffer. */
  buffer_add_bytes(session->outgoing_data, data, length);

  /* Trigger a send. */
  do_send_stuff(session);
}

static void handle_ping_request(char *ping_data)
{
  packet_t *packet = packet_create_ping(ping_data);
  size_t length;
  uint8_t *data = packet_to_bytes(packet, &length, 0);

  message_post_packet_out(data, length);

  packet_destroy(packet);
  safe_free(data);
}

static void handle_heartbeat()
{
  printf("handle_heartbeat() not implemented\n");
#if 0
  session_entry_t *entry;

  for(entry = first_session; entry; entry = entry->next)
  {
    /* Cleanup the incoming/outgoing buffers, if we can, to save memory */
    if(buffer_get_remaining_bytes(entry->session->outgoing_data) == 0)
      buffer_clear(entry->session->outgoing_data);

    /* Send stuff if we can */
    do_send_stuff(entry->session);
  }

  /* Remove any completed sessions. */
  remove_completed_sessions();
#endif
}
#endif

void debug_set_isn(uint16_t value)
{
  isn = value;

  LOG_WARNING("WARNING: Setting a custom ISN can be dangerous!");
}

void session_enable_packet_trace()
{
  packet_trace = TRUE;
}

NBBOOL session_is_shutdown(session_t *session)
{
  return session->is_shutdown;
}
