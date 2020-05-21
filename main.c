#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <string.h>

#include <jack/jack.h>
#include <jack/midiport.h>

#define MAX_BUFFER_SIZE (8192) /* should be plenty, I guess */
#define MAX_MIDI_EVENTS (1000) /* should also be plenty, maybe */

const char JACK_NAME[] = "midi2midi";

typedef struct midi_event {
    struct midi_event *next;

    uint64_t time;
    unsigned int size;
    unsigned char buffer[MAX_BUFFER_SIZE];

    unsigned int sysex;
} midi_event;

/* probably awful crappy ring buffer but I have no idea how to do these so this
   will have to do */
typedef struct {
    jack_port_t *in, *out;

    uint64_t runningTime;

    midi_event events[MAX_MIDI_EVENTS];
    midi_event *head;

    unsigned int sysex;
} ThreadCTX;

#define EVENT_IDX_CLAMP(EVIDX) (EVIDX % EVENT_RING_COUNT)

midi_event *next_free_event(ThreadCTX *ctx) {
    int i;

    for(i = 0; i < MAX_MIDI_EVENTS; i++) {
        if(ctx->events[i].size == 0) {
            return(&(ctx->events[i]));
        }
    }

    return(NULL);
}

int schedule_event(ThreadCTX *ctx, uint64_t time, size_t size, unsigned char * buffer) {
    midi_event *event, *newEvent;

    if(size > MAX_BUFFER_SIZE) {
        printf("Event is larger (%lu) than MAX_BUFFER SIZE (%d).\n",
               size, MAX_BUFFER_SIZE);
        return(-1);
    }

    /* if we're still handling a sysex message, find the first non-sysex message
       and do like above, remove any messages which may be scheduled during the
       sysex message */
    if(ctx->sysex > 0) {
        /* don't try to find the first non-sysex event when there are no events,
           and later just attach the new event to the head rather than trying to
           insert it. */
        if(ctx->head != NULL) {
            event = ctx->head;

            while(event->next != NULL) {
                if(event->next->sysex == 0) {
                    break;
                }

                event = event->next;
            }
        }

        newEvent = next_free_event(ctx);
        if(newEvent == NULL) {
            printf("No more free events.\n");
            return(-1);
        }

        newEvent->time = time;
        newEvent->size = size;
        newEvent->sysex = ctx->sysex;
        memcpy(newEvent->buffer, buffer, size);

        /* if there's still sysex message but all events have been consumed,
           add a new head and since it's the only event, there's nothing more to
           do. */
        if(ctx->head == NULL) {
            ctx->head = newEvent;
            newEvent->next = NULL;
        } else {
            event->next = newEvent;
            newEvent->next = event->next;

            /* don't evaluate the message we just inserted */
            event = newEvent->next;
            while(event != NULL) {
                if(event->time > time + size) {
                    break;
                }

                newEvent->next = event->next;
                event->size = 0;

                event = event->next;
            }
        }

        /* end of message reached, return to normal operation */
        if(buffer[size - 1] == 0xF7) {
            ctx->sysex = 0;
        } else {
            ctx->sysex++;
        }

        return(0);
    }

    /* always assume sysex messages will be scheduled for the present, since
       this program shouldn't be generating any of them on its own, so always
       attach to the head and delete any existing messages which may be
       scheduled to send during the message stream. */
    if(buffer[0] == 0xF0) {
        if(buffer[size - 1] != 0xF7) {
            ctx->sysex = 1;
        }

        newEvent = next_free_event(ctx);
        if(newEvent == NULL) {
            printf("No more free events.\n");
            return(-1);
        }
        newEvent->next = ctx->head;
        newEvent->time = time;
        newEvent->size = size;
        newEvent->sysex = ctx->sysex;
        memcpy(newEvent->buffer, buffer, size);
        ctx->head = newEvent;

        /* don't evaluate the message we just inserted */
        event = newEvent->next;
        while(event != NULL) {
            if(event->time > time + size) {
                break;
            }

            newEvent->next = event->next;
            event->size = 0;

            event = event->next;
        }

        return(0);
    }

    /* no scheduled events */
    if(ctx->head == NULL) {
        event = next_free_event(ctx);
        if(event == NULL) {
            printf("No more free events.\n");
            return(-1);
        }
        event->next = NULL;
        event->time = time;
        event->size = size;
        event->sysex = 0;
        memcpy(event->buffer, buffer, size);
        ctx->head = event;
        return(0);
    }

    /* find first event following requested scheduled time */
    event = ctx->head;
    while(event->next != NULL) {
        if(event->next->time > time) {
            /* Don't insert an event within a contiguous sysex message */
            if(event->sysex > 0) {
                if(event->next->sysex == event->sysex + 1) {
                    printf("Event would be scheduled during sysex message.\n");
                    return(-1);
                }
            }
            newEvent = next_free_event(ctx);
            newEvent->next = event->next;
            event->next = newEvent;
            newEvent->time = time;
            newEvent->size = size;
            newEvent->sysex = 0;
            memcpy(newEvent->buffer, buffer, size);
            return(0);
        }
        
        event = event->next;
    }

    /* none found, so just schedule it as the last event */
    event->next = next_free_event(ctx);
    event = event->next;
    event->time = time;
    event->size = size;
    event->sysex = 0;
    memcpy(event->buffer, buffer, size);
    event->next = NULL;
    return(0);
}

midi_event *get_next_event(ThreadCTX *ctx) {
    return(ctx->head);
}

void next_event_done(ThreadCTX *ctx) {
    ctx->head->size = 0;
    ctx->head = ctx->head->next;
}

/* simple function to just transfer data through */
int process(jack_nframes_t nframes, void *arg) {
    ThreadCTX *ctx = arg;
    jack_midi_event_t jackEvent;
    midi_event *event;

    char *in, *out;
    uint32_t i, j;

    in = jack_port_get_buffer(ctx->in, nframes);

    for(i = 0;; i++) {
        if(jack_midi_event_get(&jackEvent, in, i)) {
            break;
        }

/*
        printf("Storing message type %02X at %u.\n",
               jackEvent.buffer[0],
               jackEvent.time);
*/

        if(jackEvent.buffer[0] == 0x90) {
            int velocity = 127;
            for(j = 0; j < 5; j++) {
                jackEvent.buffer[0] = 0x90 | j;
                jackEvent.buffer[2] = velocity;

                if(schedule_event(ctx,
                                  ctx->runningTime + jackEvent.time + (j * 10000 - 8000),
                                  jackEvent.size,
                                  jackEvent.buffer)) {
                    printf("Failed to schedule event.\n");
                }

                jackEvent.buffer[0] = 0x80 | j;
                if(schedule_event(ctx,
                                  ctx->runningTime + jackEvent.time + (j * 10000),
                                  jackEvent.size,
                                  jackEvent.buffer)) {
                    printf("Failed to schedule event.\n");
                }

                velocity /= 2;
            }
        } else if(jackEvent.buffer[0] == 0x80) {
        } else {
            if(schedule_event(ctx,
                              ctx->runningTime + jackEvent.time + j,
                              jackEvent.size,
                              jackEvent.buffer)) {
                printf("Failed to schedule event.\n");
            }
        }        
    }

    out = jack_port_get_buffer(ctx->out, nframes);
    jack_midi_clear_buffer(out);

    for(;;) {
        event = get_next_event(ctx);
        if(event == NULL) {
            break;
        }
        if(event->time > ctx->runningTime + nframes) {
            /* nothing more to do for this frame */
            break;
        }
/*
        printf("Recalled message type %02X at %lu.\n",
               event->buffer[0],
               event->time);
*/
        for(j = 0; j < event->size; j++) {
            printf("%02X ", event->buffer[j]);
        }
        printf("\n");
        if(jack_midi_event_write(out, event->time - ctx->runningTime, event->buffer, event->size)) {
            printf("Failed to write event.\n");
        }

        next_event_done(ctx);
    }

    ctx->runningTime += nframes;
    return(0);
}

/* must be global so signal handlers work. */
jack_client_t *jack;

void jack_close() {
    if(jack_client_close(jack)) {
        fprintf(stderr, "Error closing JACK connection.\n");
    } else {
        fprintf(stderr, "JACK connection closed.\n");
    }
}

void jack_cleanup() {
    if(jack_deactivate(jack)) {
        fprintf(stderr, "Failed to deactivate JACK client.\n");
    } else {
        fprintf(stderr, "JACK client deactivated.\n");
    }

    jack_close();
}

void jack_cleanup_and_exit(int signum) {
    jack_cleanup();

    exit(EXIT_SUCCESS);
}

int main(int argc, char **argv) {
    jack_status_t jstatus;
    ThreadCTX tctx;
    int i;

    struct sigaction sa;
    sa.sa_handler = jack_cleanup_and_exit;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;

    jack = jack_client_open(JACK_NAME, JackNoStartServer, &jstatus);
    if(jack == NULL) {
        fprintf(stderr, "Failed to open JACK connection.\n");
        return(EXIT_FAILURE);
    }
    if(sigaction(SIGHUP, &sa, NULL) != 0 ||
       sigaction(SIGINT, &sa, NULL) != 0 ||
       sigaction(SIGTERM, &sa, NULL) != 0) {
        fprintf(stderr, "Failed to set signal handler.\n");
    }
    fprintf(stderr, "JACK connection opened.\n");

    tctx.in = jack_port_register(jack,
                                 "in",
                                 JACK_DEFAULT_MIDI_TYPE,
                                 JackPortIsInput,
                                 0);
    if(tctx.in == NULL) {
        fprintf(stderr, "Failed to register in port.\n");
        jack_close();
        return(EXIT_FAILURE);
    }

    tctx.out = jack_port_register(jack,
                             "out",
                             JACK_DEFAULT_MIDI_TYPE,
                             JackPortIsOutput,
                             0);
    if(tctx.out == NULL) {
        fprintf(stderr, "Failed to register out port.\n");
        jack_cleanup();
        return(EXIT_FAILURE);
    }
    for(i = 0; i < MAX_MIDI_EVENTS; i++) {
        tctx.events[i].size = 0;
    }
    tctx.head = NULL;
    tctx.runningTime = 0;
    tctx.sysex = 0;

    if(jack_set_process_callback(jack, process, &tctx)) {
        fprintf(stderr, "Failed to set JACK process callback.\n");
        jack_cleanup();
        return(EXIT_FAILURE);
    }
    fprintf(stderr, "Registered ports and callbacks.\n");

    if(jack_activate(jack)) {
        fprintf(stderr, "Failed to activate JACK client.\n");
        jack_cleanup();
        return(EXIT_FAILURE);
    }
    fprintf(stderr, "JACK client activated, waiting...\n");

    for(;;) {
        fflush(stdout); /* flush stdout outside of realtime thread,
                           I have no idea how breaky this is */
        sleep(1);
    }

    return(EXIT_SUCCESS);
}
