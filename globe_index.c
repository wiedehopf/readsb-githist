#include "readsb.h"

static void mark_legs(struct aircraft *a);

void init_globe_index(struct tile *s_tiles) {
    int count = 0;

    // Arctic
    s_tiles[count++] = (struct tile) {
        60, -130,
        90, 150
    };

    // North Pacific
    s_tiles[count++] = (struct tile) {
        10, 150,
        90, -130
    };

    // Northern Canada
    s_tiles[count++] = (struct tile) {
        50, -130,
        60, -70
    };

    // Northwest USA
    s_tiles[count++] = (struct tile) {
        40, -130,
        50, -100
    };

    // West Russia
    s_tiles[count++] = (struct tile) {
        40, 20,
        60, 50
    };

    // Central Russia
    s_tiles[count++] = (struct tile) {
        30, 50,
        60, 90
    };

    // East Russia
    s_tiles[count++] = (struct tile) {
        30, 90,
        60, 120
    };
    // Koreas and Japan and some Russia
    s_tiles[count++] = (struct tile) {
        30, 120,
        60, 150
    };

    // Persian Gulf / Arabian Sea
    s_tiles[count++] = (struct tile) {
        10, 50,
        30, 70
    };

    // India
    s_tiles[count++] = (struct tile) {
        10, 70,
        30, 90
    };

    // South China and ICAO special use
    s_tiles[count++] = (struct tile) {
        10, 90,
        30, 110
    };
    s_tiles[count++] = (struct tile) {
        10, 110,
        30, 150
    };


    // South Atlantic and Indian Ocean
    s_tiles[count++] = (struct tile) {
        -90, -40,
        10, 110
    };

    // Australia
    s_tiles[count++] = (struct tile) {
        -90, 110,
        10, 160
    };

    // South Pacific and NZ
    s_tiles[count++] = (struct tile) {
        -90, 160,
        10, -90
    };

    // North South America
    s_tiles[count++] = (struct tile) {
        -10, -90,
        10, -40
    };

    // South South America
    s_tiles[count++] = (struct tile) {
        -90, -90,
        -10, -40
    };

    // Guatemala / Mexico
    s_tiles[count++] = (struct tile) {
        10, -130,
        30, -90
    };

    // Cuba / Haiti / Honduras
    s_tiles[count++] = (struct tile) {
        10, -90,
        20, -70
    };



    // North Africa
    s_tiles[count++] = (struct tile) {
        10, -10,
        40, 30
    };

    // Middle East
    s_tiles[count++] = (struct tile) {
        10, 30,
        40, 50
    };

    // North Atlantic
    s_tiles[count++] = (struct tile) {
        10, -70,
        60, -10
    };

    if (count + 1 > GLOBE_SPECIAL_INDEX)
        fprintf(stderr, "increase GLOBE_SPECIAL_INDEX please!\n");
}

int globe_index(double lat_in, double lon_in) {
    int grid = GLOBE_INDEX_GRID;
    int lat = grid * ((int) ((lat_in + 90) / grid)) - 90;
    int lon = grid * ((int) ((lon_in + 180) / grid)) - 180;

    struct tile *tiles = Modes.json_globe_special_tiles;

    for (int i = 0; tiles[i].south != 0 || tiles[i].north != 0; i++) {
        struct tile tile = tiles[i];
        if (lat >= tile.south && lat < tile.north) {
            if (tile.west < tile.east && lon >= tile.west && lon < tile.east) {
                return i;
            }
            if (tile.west > tile.east && (lon >= tile.west || lon < tile.east)) {
                return i;
            }
        }
    }


    int i = (lat + 90) / grid;
    int j = (lon + 180) / grid;

    return (i * GLOBE_LAT_MULT + j + 1000);
    // highest number returned: globe_index(90, 180)
    // first 1000 are reserved for special use
}

int globe_index_index(int index) {
    double lat = ((index - 1000) /  GLOBE_LAT_MULT) * GLOBE_INDEX_GRID - 90;
    double lon = ((index - 1000) % GLOBE_LAT_MULT) * GLOBE_INDEX_GRID - 180;
    return globe_index(lat, lon);
}


void write_trace(struct aircraft *a, uint64_t now, int write_history) {
    struct char_buffer recent;
    struct char_buffer full;
    struct char_buffer hist;
    size_t shadow_size = 0;
    char *shadow = NULL;
    char filename[PATH_MAX];

    time_t nowish = now/1000 - GLOBE_OVERLAP;

    recent.len = 0;
    full.len = 0;
    hist.len = 0;

    if (Modes.json_globe_index && a->trace_len == 0 && a->trace_full_write == 0xdead)
        return;

    pthread_mutex_lock(&a->trace_mutex);

    a->trace_write = 0;

    mark_legs(a);

    recent = generateTraceJson(a, (a->trace_len > 142) ? (a->trace_len - 142) : 0);
    // write recent trace to /run

    if (a->trace_full_write > 122 || now > a->trace_next_fw) {
        // write full trace to /run

        full = generateTraceJson(a, 0);

        if (a->trace_full_write == 0xc0ffee) {
            a->trace_next_fw = now + 1000 * (rand() % GLOBE_OVERLAP - 60);
        } else if (!Modes.json_globe_index)  {
            a->trace_next_fw = now + 6 * (GLOBE_OVERLAP - 60 - rand() % GLOBE_OVERLAP / 16) * 1000;
        } else {
            a->trace_next_fw = now + (GLOBE_OVERLAP - 60 - rand() % GLOBE_OVERLAP / 16) * 1000;
        }

        if (write_history && a->trace_full_write > 2) {
            write_history = 2;
        }

        a->trace_full_write = 0;
        //fprintf(stderr, "%06x\n", a->addr);
        if (a->pos_set) {
            shadow_size = sizeof(struct aircraft) + a->trace_len * sizeof(struct state);
            shadow = malloc(shadow_size);
            memcpy(shadow, a, sizeof(struct aircraft));
            if (a->trace_len > 0)
                memcpy(shadow + sizeof(struct aircraft), a->trace, a->trace_len * sizeof(struct state));
        }
        if (a->trace_len == 0) {
            a->trace_full_write = 0xdead - 1;
        }
    }

    a->trace_full_write++;

    if (a->trace_len > 0 && write_history == 2 &&
            Modes.globe_history_dir && !(a->addr & MODES_NON_ICAO_ADDRESS)) {
        // write to permanent storage

        struct tm *utc = gmtime(&nowish);
        utc->tm_sec = 0;
        utc->tm_min = 0;
        utc->tm_hour = 0;
        uint64_t start_of_day = 1000 * (uint64_t) (timegm(utc) - 60);


        int start = -1;
        for (int i = 0; i < a->trace_len; i++) {
            struct state state = a->trace[i];
            if (state.timestamp > start_of_day) {
                start = i;
                break;
            }
        }
        if (start >= 0)
            hist = generateTraceJson(a, start);
    }

    pthread_mutex_unlock(&a->trace_mutex);


    if (recent.len > 0) {
        snprintf(filename, 256, "traces/%02x/trace_recent_%s%06x.json", a->addr % 256, (a->addr & MODES_NON_ICAO_ADDRESS) ? "~" : "", a->addr & 0xFFFFFF);
        writeJsonToGzip(Modes.json_dir, filename, recent, 1);
        free(recent.buffer);
    }

    if (full.len > 0) {
        snprintf(filename, 256, "traces/%02x/trace_full_%s%06x.json", a->addr % 256, (a->addr & MODES_NON_ICAO_ADDRESS) ? "~" : "", a->addr & 0xFFFFFF);

        if (a->addr & MODES_NON_ICAO_ADDRESS)
            writeJsonToGzip(Modes.json_dir, filename, full, 3);
        else
            writeJsonToGzip(Modes.json_dir, filename, full, 7);

        free(full.buffer);
    }

    if (hist.len > 0) {

        static int day; // STATIC !!!!!!!!!!

        char tstring[100];
        struct tm *utc = gmtime(&nowish);
        strftime (tstring, 100, "%Y-%m-%d", utc);

        if (utc->tm_mday != day) {

            day = utc->tm_mday;

            snprintf(filename, PATH_MAX - 200, "%s/%s", Modes.globe_history_dir, tstring);
            filename[PATH_MAX - 201] = 0;

            mkdir(filename, 0755);

            char pathbuf[PATH_MAX+20];
            snprintf(pathbuf, sizeof(pathbuf), "%s/traces", filename);
            pathbuf[PATH_MAX - 1] = 0;
            mkdir(pathbuf, 0755);
            for (int i = 0; i < 256; i++) {
                snprintf(pathbuf, sizeof(pathbuf), "%s/traces/%02x", filename, i);
                mkdir(pathbuf, 0755);
            }
        }

        snprintf(filename, PATH_MAX, "%s/traces/%02x/trace_full_%s%06x.json", tstring, a->addr % 256, (a->addr & MODES_NON_ICAO_ADDRESS) ? "~" : "", a->addr & 0xFFFFFF);
        filename[PATH_MAX - 101] = 0;
        writeJsonToGzip(Modes.globe_history_dir, filename, hist, 9);

        free(hist.buffer);
    }

    if (shadow && shadow_size > 0) {
        snprintf(filename, 1024, "%s/internal_state/%02x/%06x", Modes.globe_history_dir, a->addr % 256, a->addr);

        int fd = open(filename, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        int res;
        res = write(fd, shadow, shadow_size);
        res++;
        close(fd);
    }
    free(shadow);
}

void *save_state(void *arg) {
    int thread_number = *((int *) arg);
    for (int j = 0; j < AIRCRAFTS_BUCKETS; j++) {
        if (j % 8 != thread_number)
            continue;
        for (struct aircraft *a = Modes.aircrafts[j]; a; a = a->next) {
            if (!a->pos_set)
                continue;
            if (a->addr & MODES_NON_ICAO_ADDRESS)
                continue;
            if (a->messages < 2)
                continue;

            char filename[1024];
            snprintf(filename, 1024, "%s/internal_state/%02x/%06x", Modes.globe_history_dir, a->addr % 256, a->addr);

            int fd = open(filename, O_WRONLY | O_CREAT | O_TRUNC, 0644);
            int res;
            res = write(fd, a, sizeof(struct aircraft));
            if (a->trace_len > 0)
                res = write(fd, a->trace, a->trace_len * sizeof(struct state));
            res++;
            /*
               size_t shadow_size = 0;
               char *shadow = NULL;
               shadow_size = sizeof(struct aircraft) + a->trace_len * sizeof(struct state);
               shadow = malloc(shadow_size);
               memcpy(shadow, a, sizeof(struct aircraft));
               if (a->trace_len > 0)
               memcpy(shadow + sizeof(struct aircraft), a->trace, a->trace_len * sizeof(struct state));

               res = write(fd, shadow, shadow_size);
               */
            close(fd);
        }
    }
    return NULL;
}


void *load_state(void *arg) {
    uint64_t now = mstime();
    char pathbuf[PATH_MAX];
    struct stat fileinfo = {0};
    int thread_number = *((int *) arg);
    srand(now + thread_number);
    for (int i = 0; i < 256; i++) {
        if (i % 8 != thread_number)
            continue;
        snprintf(pathbuf, PATH_MAX, "%s/internal_state/%02x", Modes.globe_history_dir, i);

        DIR *dp;
        struct dirent *ep;

        dp = opendir (pathbuf);
        if (dp == NULL)
            continue;

        while ((ep = readdir (dp))) {
            if (strlen(ep->d_name) != 6)
                continue;
            snprintf(pathbuf, PATH_MAX, "%s/internal_state/%02x/%s", Modes.globe_history_dir, i, ep->d_name);

            int fd = open(pathbuf, O_RDONLY);

            fstat(fd, &fileinfo);
            off_t len = fileinfo.st_size;
            int trace_size = len - sizeof(struct aircraft);
            if (trace_size % sizeof(struct state) != 0) {
                fprintf(stderr, "filesize mismatch\n");
                close(fd);
                unlink(pathbuf);
                continue;
            }
            struct aircraft *a = (struct aircraft *) aligned_alloc(64, sizeof(struct aircraft));

            if (read(fd, a, sizeof(struct aircraft)) != sizeof(struct aircraft) ||
                    a->size_struct_aircraft != sizeof(struct aircraft)
               ) {
                if (a->size_struct_aircraft != sizeof(struct aircraft)) {
                    fprintf(stderr, "sizeof(struct aircraft) has changed, unable to read state!\n");
                } else {
                    fprintf(stderr, "read fail\n");
                }
                free(a);
                close(fd);
                unlink(pathbuf);
                continue;
            }

            a->first_message = NULL;

            if (a->trace_alloc > 0) {
                if ((uint32_t) a->trace_len != trace_size / sizeof(struct state)) {
                    fprintf(stderr, "trace_len mismatch\n");
                    free(a);
                    close(fd);
                    unlink(pathbuf);
                    continue;
                }
                a->trace = malloc(a->trace_alloc * sizeof(struct state));
                if (read(fd, a->trace, trace_size) != trace_size) {
                    fprintf(stderr, "read trace fail\n");
                    free(a->trace);
                    free(a);
                    close(fd);
                    unlink(pathbuf);
                    continue;
                }
                a->trace_next_fw = now + 1000 * (rand() % 120); // spread over 2 mins
                a->trace_full_write = 0xc0ffee; // rewrite full history file
                //a->trace_write = 1;
                //write_trace(a, now, 0);
            }

            if (pthread_mutex_init(&a->trace_mutex, NULL)) {
                fprintf(stderr, "Unable to initialize trace mutex!\n");
                exit(1);
            }

            Modes.stats_current.unique_aircraft++;

            close(fd);

            a->next = Modes.aircrafts[a->addr % AIRCRAFTS_BUCKETS]; // .. and put it at the head of the list
            Modes.aircrafts[a->addr % AIRCRAFTS_BUCKETS] = a;
        }

        closedir (dp);
    }
    return NULL;
}

void *jsonTraceThreadEntryPoint(void *arg) {

    int thread = * (int *) arg;

    srand(thread);

    int part = 0;
    int n_parts = 64; // power of 2

    int thread_section_len = (AIRCRAFTS_BUCKETS / TRACE_THREADS);
    int thread_start = thread * thread_section_len;
    //int thread_end = thread_start + thread_section_len;
    //fprintf(stderr, "%d %d\n", thread_start, thread_end);
    int section_len = thread_section_len / n_parts;

    struct timespec slp = {0, 0};
    // write each part every 25 seconds
    uint64_t sleep = 25 * 1000 / n_parts;

    slp.tv_sec =  (sleep / 1000);
    slp.tv_nsec = (sleep % 1000) * 1000 * 1000;

    pthread_mutex_lock(&Modes.jsonTraceThreadMutex[thread]);

    while (!Modes.exit) {
        struct aircraft *a;

        pthread_mutex_unlock(&Modes.jsonTraceThreadMutex[thread]);

        nanosleep(&slp, NULL);

        pthread_mutex_lock(&Modes.jsonTraceThreadMutex[thread]);

        int start = thread_start + part * section_len;
        int end = start + section_len;

        //fprintf(stderr, "%d %d %d\n", part, start, end);

        uint64_t now = mstime();

        for (int j = start; j < end; j++) {
            for (a = Modes.aircrafts[j]; a; a = a->next) {
                if (a->trace_write)
                    write_trace(a, now, 1);
            }
        }

        part++;
        part %= n_parts;
    }

    pthread_mutex_unlock(&Modes.jsonTraceThreadMutex[thread]);

#ifndef _WIN32
    pthread_exit(NULL);
#else
    return NULL;
#endif
}

static void mark_legs(struct aircraft *a) {
    if (a->trace_len < 20)
        return;

    int high = 0;
    int low = 100000;

    uint32_t focus = 0xfffffff;
    //focus = 0x3c5465;

    double sum = 0;

    for (int i = 0; i < a->trace_len; i++) {
        int32_t altitude = a->trace[i].altitude;
        int on_ground = altitude & (1<<22);
        int alt_unknown = altitude & (1<<23);

        if (a->trace[i].altitude & (1<<26))
            a->trace[i].altitude ^= (1<<26);

        if (alt_unknown)
            continue;

        altitude = altitude & ((1<<21) - 1);
        altitude -= 100000; // restore actual altitude

        if (on_ground)
            altitude = 0;

        sum += altitude;
    }

    int threshold = (int) sum / a->trace_len / 3;

    if (threshold > 10000)
        threshold = 10000;

    high = 0;
    low = 100000;

    uint64_t major_climb = 0;
    uint64_t major_descent = 0;
    int major_climb_index = 0;
    int major_descent_index = 0;
    uint64_t last_high = 0;
    uint64_t last_low = 0;

    int last_low_index = 0;

    uint64_t last_airborne = 0;
    int was_ground = 0;

    for (int i = 1; i < a->trace_len; i++) {
        struct state state = a->trace[i];

        int32_t altitude = state.altitude;
        //int stale = altitude & (1<<21);
        int on_ground = altitude & (1<<22);
        int alt_unknown = altitude & (1<<23);
        //int track_unknown = altitude & (1<<24);
        //int gs_unknown = altitude & (1<<25);
        //
        altitude = altitude & ((1<<21) - 1);
        altitude -= 100000; // restore actual altitude

        if (on_ground || alt_unknown)
            altitude = 0;

        if(!on_ground)
            last_airborne = state.timestamp;

        if (altitude >= high) {
            high = altitude;
        }
        if (altitude <= low) {
            low = altitude;
        }

        /*
        if (state.timestamp > a->trace[i-1].timestamp + 45 * 60 * 1000) {
            high = low = altitude;
        }
        */

        if (abs(low - altitude) < 800) {
            last_low = state.timestamp;
            last_low_index = i;
        }
        if (abs(high - altitude) < 800)
            last_high = state.timestamp;

        if (high - low > threshold) {
            if (last_high > last_low) {
                int bla = max(0, last_low_index + 3);
                major_climb = a->trace[bla].timestamp;
                major_climb_index = bla;
                if (a->addr == focus) {
                    time_t nowish = major_climb/1000;
                    struct tm *utc = gmtime(&nowish);
                    char tstring[100];
                    strftime (tstring, 100, "%H:%M:%S", utc);
                    fprintf(stderr, "climb: %d %s\n", altitude, tstring);
                }
                low = high - threshold * 9/10;
            }
            if (last_high < last_low) {
                int bla = max(0, i - 3);
                major_descent = a->trace[bla].timestamp;
                major_descent_index = bla;
                if (a->addr == focus) {
                    time_t nowish = major_descent/1000;
                    struct tm *utc = gmtime(&nowish);
                    char tstring[100];
                    strftime (tstring, 100, "%H:%M:%S", utc);
                    fprintf(stderr, "desc: %d %s\n", altitude, tstring);
                }
                high = low + threshold * 9/10;
            }
        }
        int leg_ground = 0;
        if ( (major_descent && (on_ground || was_ground) && state.timestamp > a->trace[i-1].timestamp + 25 * 60 * 1000) ||
                (major_descent && (on_ground || was_ground) && state.timestamp > last_airborne + 45 * 60 * 1000)
           )
        {
            leg_ground = 1;
        }

        if ( (major_climb && major_descent && major_climb >= major_descent + 10 * 60 * 1000) ||
                leg_ground
           )
        {
            uint64_t leg_ts = 0;
            if (leg_ground) {
                a->trace[i].altitude |= (1<<26);
                leg_ts = a->trace[i].timestamp;
                // set leg marker
            } else if (major_descent_index + 1 == major_climb_index) {
                a->trace[major_climb_index].altitude |= (1<<26);
                leg_ts = a->trace[major_climb_index].timestamp;
                // set leg marker
            } else {
                int found = 0;
                for (int i = major_climb_index; i >= major_descent_index; i--) {
                    if (found)
                        break;

                    struct state *state = &a->trace[i];
                    struct state *last = &a->trace[i - 1];

                    if (state->timestamp > last->timestamp + 5 * 60 * 1000) {
                        state->altitude |= (1<<26);
                        // set leg marker
                        leg_ts = state->timestamp;
                        found = 1;
                    }
                }
                uint64_t half = major_descent + (major_climb - major_descent) / 2;
                for (int i = major_descent_index + 1; i < major_climb_index; i++) {
                    if (found)
                        break;

                    struct state *state = &a->trace[i];

                    if (state->timestamp > half) {
                        state->altitude |= (1<<26);
                        // set leg marker
                        leg_ts = state->timestamp;
                        found = 1;
                    }
                }
            }

            major_climb = 0;
            major_climb_index = 0;
            major_descent = 0;
            major_descent_index = 0;
            if (a->addr == focus) {
                time_t nowish = leg_ts/1000;
                struct tm *utc = gmtime(&nowish);
                char tstring[100];
                strftime (tstring, 100, "%H:%M:%S", utc);
                fprintf(stderr, "leg: %s\n", tstring);
            }
        }

        was_ground = on_ground;
    }
}
