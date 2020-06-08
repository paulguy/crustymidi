#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <string.h>
#include <stdarg.h>

#include <jack/jack.h>
#include <jack/midiport.h>

#include "crustyvm.h"

#define MAX_BUFFER_SIZE (8192) /* should be plenty, I guess */
#define MAX_MIDI_EVENTS (1000) /* should also be plenty, maybe */

/* pick an event unlikely to come from JACK to use for requested timer events
   and just use the MIDI timing clock event I guess */
#define UNLIKELY_JACK_EVENT (0xF8)

const char JACK_NAME[] = "crustymidi";

const unsigned char TIMER_EVENT[] = { UNLIKELY_JACK_EVENT };

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
    midi_event event[MAX_MIDI_EVENTS];
    midi_event *head;
    unsigned int nextfree;
    unsigned int sysex;
} EventList;

typedef struct {
    int good;

    jack_port_t *in, *out;

    unsigned int rate, newrate;

    uint64_t runningTime;

    EventList inEv, outEv;

    jack_client_t *jack;
    CrustyVM *cvm;
    midi_event *curEv;

    unsigned int outTime;
    unsigned int outLen;
    unsigned char outBuff[MAX_BUFFER_SIZE];
} ThreadCTX;

#define EVENT_IDX_CLAMP(EVIDX) (EVIDX % EVENT_RING_COUNT)

/* must be global so signal handlers work. */
ThreadCTX tctx;

midi_event *next_free_event(EventList *ev) {
    unsigned int i;

    for(i = ev->nextfree; i < MAX_MIDI_EVENTS; i++) {
        if(ev->event[i].size == 0) {
            return(&(ev->event[i]));
        }
    }
    for(i = 0; i < ev->nextfree; i++) {
        if(ev->event[i].size == 0) {
            return(&(ev->event[i]));
        }
    }

    return(NULL);
}

int schedule_event(EventList *ev,
                   uint64_t time,
                   size_t size,
                   const unsigned char * buffer) {
    midi_event *event, *newEvent, *lastEvent;

    if(size > MAX_BUFFER_SIZE) {
        printf("Event is larger (%lu) than MAX_BUFFER SIZE (%d).\n",
               size, MAX_BUFFER_SIZE);
        return(-1);
    }

    /* if we're still handling a sysex message, find the first non-sysex message
       and do like above, remove any messages which may be scheduled during the
       sysex message */
    if(ev->sysex > 0) {
        /* don't try to find the first non-sysex event when there are no events,
           and later just attach the new event to the head rather than trying to
           insert it. */
        if(ev->head != NULL) {
            event = ev->head;

            while(event->next != NULL) {
                if(event->next->sysex == 0) {
                    break;
                }

                event = event->next;
            }
        }

        newEvent = next_free_event(ev);
        if(newEvent == NULL) {
            printf("No more free events.\n");
            return(-1);
        }

        newEvent->time = time;
        newEvent->size = size;
        newEvent->sysex = ev->sysex;
        memcpy(newEvent->buffer, buffer, size);

        /* if there's still sysex message but all events have been consumed,
           add a new head and since it's the only event, there's nothing more to
           do. */
        if(ev->head == NULL) {
            ev->head = newEvent;
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
            ev->sysex = 0;
        } else {
            ev->sysex++;
        }

        return(0);
    }

    /* always assume sysex messages will be scheduled for the present, since
       this program shouldn't be generating any of them on its own, so always
       attach to the head and delete any existing messages which may be
       scheduled to send during the message stream. */
    if(buffer[0] == 0xF0) {
        if(buffer[size - 1] != 0xF7) {
            ev->sysex = 1;
        }

        newEvent = next_free_event(ev);
        if(newEvent == NULL) {
            printf("No more free events.\n");
            return(-1);
        }
        newEvent->next = ev->head;
        newEvent->time = time;
        newEvent->size = size;
        newEvent->sysex = ev->sysex;
        memcpy(newEvent->buffer, buffer, size);
        ev->head = newEvent;

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
    if(ev->head == NULL) {
        event = next_free_event(ev);
        if(event == NULL) {
            printf("No more free events.\n");
            return(-1);
        }
        event->next = NULL;
        event->time = time;
        event->size = size;
        event->sysex = 0;
        memcpy(event->buffer, buffer, size);
        ev->head = event;
        return(0);
    }

    /* find first event following requested scheduled time */
    event = ev->head;
    lastEvent = NULL;
    while(event != NULL) {
        if(event->time > time) {
            /* Don't insert an event within a contiguous sysex message */
            if(lastEvent != NULL &&
               lastEvent->sysex != 0 &&
               event->sysex == lastEvent->sysex + 1) {
                printf("Event scheduled during sysex dropped.\n");
            }
            newEvent = next_free_event(ev);
            newEvent->next = event;
            if(lastEvent == NULL) {
                ev->head = newEvent;
            } else {
                lastEvent->next = newEvent;
            }
            newEvent->time = time;
            newEvent->size = size;
            newEvent->sysex = 0;
            memcpy(newEvent->buffer, buffer, size);
            return(0);
        }
        
        lastEvent = event;
        event = event->next;
    }

    /* none found, so just schedule it as the last event */
    lastEvent->next = next_free_event(ev);
    event = lastEvent->next;
    event->time = time;
    event->size = size;
    event->sysex = 0;
    memcpy(event->buffer, buffer, size);
    event->next = NULL;
    return(0);
}

midi_event *get_next_event(EventList *e) {
    return(e->head);
}

void next_event_done(EventList *e) {
    e->head->size    = 0;
    e->head = e->head->next;
}

/* simple function to just transfer data through */
int process(jack_nframes_t nframes, void *arg) {
    jack_midi_event_t jackEvent;
    midi_event *event;

    char *in, *out;
    uint32_t i;

    /* update output events in case there's a new rate */
    if(tctx.rate != tctx.newrate) {
        double mul = (double)(tctx.newrate) / (double)(tctx.rate);

        event = tctx.outEv.head;
        while(event != NULL) {
            /* only multiply by the difference from the present to prevent
               gradual loss of precision over time (aside from events which may
               be scheduled very far in the future). */
            event->time = (int)((double)(event->time - tctx.runningTime) * mul) +
                          tctx.runningTime;
            event = event->next;
        }

        tctx.rate = tctx.newrate;
    }

    in = jack_port_get_buffer(tctx.in, nframes);

    for(i = 0;; i++) {
        if(jack_midi_event_get(&jackEvent, in, i)) {
            break;
        }

        if(schedule_event(&(tctx.inEv),
                          tctx.runningTime + jackEvent.time,
                          jackEvent.size,
                          jackEvent.buffer)) {
            printf("Failed to schedule event.\n");
            tctx.good = 0;
            return(-1);
        }
    }

    for(;;) {
        /* put value in the global so when the program uses the callbacks, the
           event data can be passed in */
        tctx.curEv = get_next_event(&(tctx.inEv));
        if(tctx.curEv == NULL) {
            break;
        }
        if(tctx.curEv->time >= tctx.runningTime + nframes) {
            /* may be timer events scheduled for later */
            break;
        }

        tctx.outTime = 0;
        tctx.outLen = 0;
        if(crustyvm_run(tctx.cvm, "event") < 0) {
            printf("Event handler reached an exception while running: %s\n",
                   crustyvm_statusstr(crustyvm_get_status(tctx.cvm)));
            crustyvm_debugtrace(tctx.cvm, 1);
            tctx.good = 0;
            return(-2);
        }

        next_event_done(&(tctx.inEv));
    }

    out = jack_port_get_buffer(tctx.out, nframes);
    jack_midi_clear_buffer(out);

/*
    if(get_next_event(&(tctx.outEv)) != NULL) {
        printf("%lu\n", tctx.runningTime);
    }
*/
    for(;;) {
        event = get_next_event(&(tctx.outEv));
        if(event == NULL) {
            break;
        }
        if(event->time >= tctx.runningTime + nframes) {
            /* nothing more to do for this frame */
            break;
        }
/*
        printf("%d %lu\n", event->size, event->time);
        for(i = 0; i < event->size; i++) {
            printf("%02hhX ", event->buffer[i]);
        }
        printf("\n");
*/
        if(jack_midi_event_write(out,
                                 event->time - tctx.runningTime,
                                 event->buffer,
                                 event->size)) {
            printf("Failed to write event.\n");
            tctx.good = 0;
            return(-3);
        }

        next_event_done(&(tctx.outEv));
    }

    tctx.runningTime += nframes;
    return(0);
}

void jack_close() {
    if(jack_client_close(tctx.jack)) {
        fprintf(stderr, "Error closing JACK connection.\n");
    } else {
        fprintf(stderr, "JACK connection closed.\n");
    }
}

void cleanup() {
    if(jack_deactivate(tctx.jack)) {
        fprintf(stderr, "Failed to deactivate JACK client.\n");
    } else {
        fprintf(stderr, "JACK client deactivated.\n");
    }

    jack_close();

    if(tctx.cvm != NULL) {
        crustyvm_free(tctx.cvm);
        tctx.cvm = NULL;
    }
}

void cleanup_and_exit(int signum) {
    cleanup();

    exit(EXIT_SUCCESS);
}

int getlength(void *priv, void *val, unsigned int index) {
    *(int *)val = tctx.curEv->size;

    return(0);
}

int getdata(void *priv, void *val, unsigned int index) {
    if(index > tctx.curEv->size) {
        return(-1);
    }

    *(int *)val = tctx.curEv->buffer[index];

    return(0);
}

int gettime(void *priv, void *val, unsigned int index) {
    /* only return 31 lowest bits.  Program may need to be aware of this.
       at 48000hz, this will wrap every 12 hours or so. */
    *(int *)val = (int)(tctx.curEv->time & 0x7FFFFFFF);

    return(0);
}

int getrate(void *priv, void *val, unsigned int index) {
    *(int *)val = tctx.rate;

    return(0);
}

int setlength(void *priv, void *val, unsigned int index) {
    int intval = *(int *)val;

    if(intval < 0 || intval > MAX_BUFFER_SIZE) {
        return(-1);
    }

    /* clear memory in the case of expanding it */
    if(intval > (int)(tctx.outLen)) {
        memset(&(tctx.outBuff[tctx.outLen]), 0, intval - tctx.outLen);
    }

    tctx.outLen = intval;

    return(0);
}

int setdata(void *priv, void *val, unsigned int index) {
    if(index > tctx.outLen) {
        return(-1);
    }

    tctx.outBuff[index] = *(int *)val;

    return(0);
}

int settime(void *priv, void *val, unsigned int index) {
    int intval = *(int *)val;

    /* can't schedule for a time in the past */
    if(intval < 0) {
        return(-1);
    }

    tctx.outTime = intval;

    return(0);
}

int commit(void *priv, void *val, unsigned int index) {
    /* write nonzero to pass value */
    if(*(int *)val == 0) {
        if(tctx.curEv == NULL) { /* no current event during init */
            return(-1);
        }

        if(schedule_event(&(tctx.outEv),
                          tctx.curEv->time,
                          tctx.curEv->size,
                          tctx.curEv->buffer)) {
            return(-1);
        }
    } else {
        if(schedule_event(&(tctx.outEv),
                          tctx.curEv->time + tctx.outTime,
                          tctx.outLen,
                          tctx.outBuff)) {
            return(-1);
        }
    }

    return(0);
}

int timer(void *priv, void *val, unsigned int index) {
    int intval = *(int *)val;

    if(intval < 0) {
        return(-1);
    }

    /* add it to the input event queue */
    if(tctx.curEv == NULL) { /* init */
        if(schedule_event(&(tctx.inEv),
                          intval,
                          1,
                          TIMER_EVENT)) {
            return(-1);
        }
    } else {
        if(schedule_event(&(tctx.inEv),
                          tctx.curEv->time + intval,
                          1,
                          TIMER_EVENT)) {
            return(-1);
        }
    }

    return(0);
}

/* this will make crackles and latency so don't use it normally */
int print_int(void *priv, void *value, unsigned int index) {
    printf("%d\n", *(int *)value);
    return(0);
}

int ratechange(jack_nframes_t nframes, void *arg) {
    tctx.newrate = nframes;

    return(0);
}

void vprintf_cb(void *priv, const char *fmt, ...) {
    va_list ap;
    FILE *out = priv;

    va_start(ap, fmt);
    vfprintf(out, fmt, ap);
}

int main(int argc, char **argv) {
    /* general stuff */
    int i;

    /* jack stuff */
    jack_status_t jstatus;
    struct sigaction sa;
    sa.sa_handler = cleanup_and_exit;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;

    /* crustyvm stuff */
    FILE *in = NULL;
    char *program;
    unsigned long len;
    CrustyCallback cb[] = {
        {
            .name = "debug_print_int", .length = 1, .type = CRUSTY_TYPE_INT,
            .read  = NULL, .readpriv  = NULL,
            .write = print_int, .writepriv = NULL
        },
        {
            .name = "length", .length = 1, .type = CRUSTY_TYPE_INT,
            .read  = getlength, .readpriv  = NULL,
            .write = setlength, .writepriv = NULL
        },
        {
            .name = "data", .length = MAX_BUFFER_SIZE, .type = CRUSTY_TYPE_INT,
            .read  = getdata, .readpriv  = NULL,
            .write = setdata, .writepriv = NULL
        },
        {
            .name = "time", .length = 1, .type = CRUSTY_TYPE_INT,
            .read = gettime,  .readpriv  = NULL,
            .write = settime, .writepriv = NULL
        },
        {
            .name = "rate", .length = 1, .type = CRUSTY_TYPE_INT,
            .read  = getrate, .readpriv  = NULL,
            .write = NULL,    .writepriv = NULL
        },
        {
            .name = "commit", .length = 1, .type = CRUSTY_TYPE_INT,
            .read  = NULL,   .readpriv  = NULL,
            .write = commit, .writepriv = NULL
        },
        {
            .name = "timer", .length = 1, .type = CRUSTY_TYPE_INT,
            .read  = NULL,   .readpriv  = NULL,
            .write = timer,  .writepriv = NULL
        }
    };

    if(argc < 2) {
        fprintf(stderr, "USAGE: %s <filename>\n", argv[0]);
        exit(EXIT_FAILURE);
    }

    in = fopen(argv[1], "rb");
    if(in == NULL) {
        fprintf(stderr, "Failed to open file %s.\n", argv[1]);
        exit(EXIT_FAILURE);
    }

    if(fseek(in, 0, SEEK_END) < 0) {
       fprintf(stderr, "Failed to seek to end of file.\n");
        exit(EXIT_FAILURE);
    }

    len = ftell(in);
    rewind(in);

    program = malloc(len);
    if(program == NULL) {
        fclose(in);
        exit(EXIT_FAILURE);
    }

    if(fread(program, 1, len, in) < len) {
        fprintf(stderr, "Failed to read file.\n");
        fclose(in);
        free(program);
    }

    fclose(in);
    in = NULL;

    tctx.cvm = crustyvm_new(argv[1], program, len,
                            CRUSTY_FLAG_DEFAULTS
                            /* | CRUSTY_FLAG_TRACE */,
                            0,
                            cb, sizeof(cb) / sizeof(CrustyCallback),
                            vprintf_cb, stderr);
    free(program);
    if(tctx.cvm == NULL) {
        fprintf(stderr, "Failed to load program.\n");
        exit(EXIT_FAILURE);
    }
    fprintf(stderr, "Program loaded.\n");

    if(!crustyvm_has_entrypoint(tctx.cvm, "init")) {
        fprintf(stderr, "Program has no valid entrypoint for 'init'.\n");
        crustyvm_free(tctx.cvm);
        exit(EXIT_FAILURE);
    }

    if(!crustyvm_has_entrypoint(tctx.cvm, "event")) {
        fprintf(stderr, "Program has no valid entrypoint for 'event'.\n");
        crustyvm_free(tctx.cvm);
        exit(EXIT_FAILURE);
    }

    fprintf(stderr, "Setting up JACK.\n");

    tctx.jack = jack_client_open(JACK_NAME, JackNoStartServer, &jstatus);
    if(tctx.jack == NULL) {
        fprintf(stderr, "Failed to open JACK connection.\n");
        crustyvm_free(tctx.cvm);
        return(EXIT_FAILURE);
    }
    if(sigaction(SIGHUP, &sa, NULL) != 0 ||
       sigaction(SIGINT, &sa, NULL) != 0 ||
       sigaction(SIGTERM, &sa, NULL) != 0) {
        fprintf(stderr, "Failed to set signal handler.\n");
    }
    fprintf(stderr, "JACK connection opened.\n");

    tctx.in = jack_port_register(tctx.jack,
                                 "in",
                                 JACK_DEFAULT_MIDI_TYPE,
                                 JackPortIsInput,
                                 0);
    if(tctx.in == NULL) {
        fprintf(stderr, "Failed to register in port.\n");
        jack_close();
        crustyvm_free(tctx.cvm);
        return(EXIT_FAILURE);
    }

    tctx.out = jack_port_register(tctx.jack,
                                  "out",
                                  JACK_DEFAULT_MIDI_TYPE,
                                  JackPortIsOutput,
                                  0);
    if(tctx.out == NULL) {
        fprintf(stderr, "Failed to register out port.\n");
        cleanup();
        return(EXIT_FAILURE);
    }
    for(i = 0; i < MAX_MIDI_EVENTS; i++) {
        tctx.inEv.event[i].size = 0;
        tctx.outEv.event[i].size = 0;
    }
    tctx.inEv.head = NULL;
    tctx.inEv.nextfree = 0;
    tctx.inEv.sysex = 0;
    tctx.outEv.head = NULL;
    tctx.outEv.nextfree = 0;
    tctx.outEv.sysex = 0;
    tctx.curEv = NULL;
    tctx.runningTime = 0;
    tctx.rate = jack_get_sample_rate(tctx.jack);
    tctx.newrate = tctx.rate;

    if(crustyvm_run(tctx.cvm, "init") < 0) {
        fprintf(stderr, "Program init reached an exception while running: %s\n",
                crustyvm_statusstr(crustyvm_get_status(tctx.cvm)));
        cleanup();
        exit(EXIT_FAILURE);
    }

    if(jack_set_sample_rate_callback(tctx.jack, ratechange, NULL)) {
        fprintf(stderr, "Failed to set JACK rate change callback.\n");
        cleanup();
        return(EXIT_FAILURE);
    }

    if(jack_set_process_callback(tctx.jack, process, NULL)) {
        fprintf(stderr, "Failed to set JACK process callback.\n");
        cleanup();
        return(EXIT_FAILURE);
    }
    fprintf(stderr, "Registered ports and callbacks.\n");

    if(jack_activate(tctx.jack)) {
        fprintf(stderr, "Failed to activate JACK client.\n");
        cleanup();
        return(EXIT_FAILURE);
    }
    fprintf(stderr, "JACK client activated, waiting...\n");

    /* wait */
    tctx.good = 1;
    for(;;) {
        usleep(1000);
        if(tctx.good == 0) {
            break;
        }
    }

    /* i don't think it ever gets here? */
    return(EXIT_SUCCESS);
}
