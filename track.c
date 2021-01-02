// Part of readsb, a Mode-S/ADSB/TIS message decoder.
//
// track.c: aircraft state tracking
//
// Copyright (c) 2019 Michael Wolf <michael@mictronics.de>
//
// This code is based on a detached fork of dump1090-fa.
//
// Copyright (c) 2014-2016 Oliver Jowett <oliver@mutability.co.uk>
//
// This file is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// any later version.
//
// This file is distributed in the hope that it will be useful, but
// WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
// General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <http://www.gnu.org/licenses/>.
//
// This file incorporates work covered by the following copyright and
// license:
//
// Copyright (C) 2012 by Salvatore Sanfilippo <antirez@gmail.com>
//
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are
// met:
//
//  *  Redistributions of source code must retain the above copyright
//     notice, this list of conditions and the following disclaimer.
//
//  *  Redistributions in binary form must reproduce the above copyright
//     notice, this list of conditions and the following disclaimer in the
//     documentation and/or other materials provided with the distribution.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
// "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
// LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
// A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
// HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
// SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
// LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
// DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
// THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
// (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

#include "readsb.h"

uint32_t modeAC_count[4096];
uint32_t modeAC_lastcount[4096];
uint32_t modeAC_match[4096];
uint32_t modeAC_age[4096];

static void showPositionDebug(struct aircraft *a, struct modesMessage *mm, uint64_t now);
static void position_bad(struct modesMessage *mm, struct aircraft *a);
static void calc_wind(struct aircraft *a, uint64_t now);
static void calc_temp(struct aircraft *a, uint64_t now);
static inline int declination (struct aircraft *a, double *dec);
static const char *source_string(datasource_t source);
static void incrementReliable(struct aircraft *a, struct modesMessage *mm, uint64_t now, int odd);

// Should we accept some new data from the given source?
// If so, update the validity and return 1

static int accept_data(data_validity *d, datasource_t source, struct modesMessage *mm, int reduce_often) {
    uint64_t receiveTime = mm->sysTimestampMsg;

    if (source == SOURCE_INVALID)
        return 0;

    if (receiveTime < d->updated)
        return 0;

    if (source < d->source && receiveTime < d->updated + TRACK_STALE)
        return 0;

    // prevent JAERO and other SBS from disrupting
    // other data sources too quickly
    if (source < d->last_source) {
        if (source <= SOURCE_MLAT && receiveTime < d->updated + 30 * 1000)
            return 0;
        if (source == SOURCE_JAERO && receiveTime < d->updated + 600 * 1000)
            return 0;
    }

    if (source == SOURCE_PRIO)
        d->source = SOURCE_ADSB;
    else
        d->source = source;

    d->last_source = d->source;

    d->updated = receiveTime;
    d->stale = 0;

    if (receiveTime > d->next_reduce_forward && !mm->sbs_in) {
        d->next_reduce_forward = receiveTime + Modes.net_output_beast_reduce_interval * 4;
        if (reduce_often == 1)
            d->next_reduce_forward = receiveTime + Modes.net_output_beast_reduce_interval;
        if (reduce_often == 2)
            d->next_reduce_forward = receiveTime + Modes.net_output_beast_reduce_interval / 2;
        // make sure global CPR stays possible even at high interval:
        if (Modes.net_output_beast_reduce_interval > 7000 && mm->cpr_valid) {
            d->next_reduce_forward = receiveTime + 7000;
        }
        mm->reduce_forward = 1;
    }
    return 1;
}

// Given two datasources, produce a third datasource for data combined from them.

static void combine_validity(data_validity *to, const data_validity *from1, const data_validity *from2, uint64_t now) {
    if (from1->source == SOURCE_INVALID) {
        *to = *from2;
        return;
    }

    if (from2->source == SOURCE_INVALID) {
        *to = *from1;
        return;
    }

    to->source = (from1->source < from2->source) ? from1->source : from2->source; // the worse of the two input sources
    to->last_source = to->source;
    to->updated = (from1->updated > from2->updated) ? from1->updated : from2->updated; // the *later* of the two update times
    to->stale = (now > to->updated + TRACK_STALE);
}

static int compare_validity(const data_validity *lhs, const data_validity *rhs) {
    if (!lhs->stale && lhs->source > rhs->source)
        return 1;
    else if (!rhs->stale && lhs->source < rhs->source)
        return -1;
    else if (lhs->updated >= rhs->updated)
        return 1;
    else if (lhs->updated < rhs->updated)
        return -1;
    else
        return 0;
}

//
// CPR position updating
//

// Distance between points on a spherical earth.
// This has up to 0.5% error because the earth isn't actually spherical
// (but we don't use it in situations where that matters)

double greatcircle(double lat0, double lon0, double lat1, double lon1) {
    double dlat, dlon;

    lat0 = lat0 * M_PI / 180.0;
    lon0 = lon0 * M_PI / 180.0;
    lat1 = lat1 * M_PI / 180.0;
    lon1 = lon1 * M_PI / 180.0;

    dlat = fabs(lat1 - lat0);
    dlon = fabs(lon1 - lon0);

    // use haversine for small distances for better numerical stability
    if (dlat < 0.001 && dlon < 0.001) {
        double a = sin(dlat / 2) * sin(dlat / 2) + cos(lat0) * cos(lat1) * sin(dlon / 2) * sin(dlon / 2);
        return 6371e3 * 2 * atan2(sqrt(a), sqrt(1.0 - a));
    }

    // spherical law of cosines
    return 6371e3 * acos(sin(lat0) * sin(lat1) + cos(lat0) * cos(lat1) * cos(dlon));
}

static float bearing(double lat0, double lon0, double lat1, double lon1) {
    lat0 = lat0 * M_PI / 180.0;
    lon0 = lon0 * M_PI / 180.0;
    lat1 = lat1 * M_PI / 180.0;
    lon1 = lon1 * M_PI / 180.0;

    double y = sin(lon1-lon0)*cos(lat1);
    double x = cos(lat0)*sin(lat1) - sin(lat0)*cos(lat1)*cos(lon1-lon0);
    double res = (atan2(y, x) * 180 / M_PI + 360);
    while (res > 360)
        res -= 360;
    return (float) res;
}

static void update_range_histogram(double lat, double lon) {
    double range = 0;
    int valid_latlon = Modes.bUserFlags & MODES_USER_LATLON_VALID;

    if (!valid_latlon)
        return;

    range = greatcircle(Modes.fUserLat, Modes.fUserLon, lat, lon);

    if ((range <= Modes.maxRange || Modes.maxRange == 0)) {
        if (range > Modes.stats_current.distance_max)
            Modes.stats_current.distance_max = range;
        if (range < Modes.stats_current.distance_min)
            Modes.stats_current.distance_min = range;
    }

    if (Modes.stats_range_histo) {
        int bucket = round(range / Modes.maxRange * RANGE_BUCKET_COUNT);

        if (bucket < 0)
            bucket = 0;
        else if (bucket >= RANGE_BUCKET_COUNT)
            bucket = RANGE_BUCKET_COUNT - 1;

        ++Modes.stats_current.range_histogram[bucket];
    }
}

// return true if it's OK for the aircraft to have travelled from its last known position
// to a new position at (lat,lon,surface) at a time of now.

static int speed_check(struct aircraft *a, datasource_t source, double lat, double lon, struct modesMessage *mm, cpr_local_t cpr_local) {
    uint64_t elapsed;
    double distance;
    double range;
    double speed;
    double calc_track = 0;
    double track_diff = -1;
    double track_bonus = 0;
    int inrange;
    uint64_t now = a->seen;
    double oldLat = a->lat;
    double oldLon = a->lon;

    // json_reliable == -1 disables the speed check
    if (Modes.json_reliable == -1)
        return 1;

    MODES_NOTUSED(mm);
    if (bogus_lat_lon(lat, lon) ||
            (mm->cpr_valid && mm->cpr_lat == 0 && mm->cpr_lon == 0)
       ) {
        mm->pos_ignore = 1; // don't decrement pos_reliable
        return 0;
    }

    int surface = trackDataValid(&a->airground_valid)
        && a->airground == AG_GROUND
        && a->pos_surface
        && (!mm->cpr_valid || mm->cpr_type == CPR_SURFACE);

    if (a->pos_reliable_odd < 1 && a->pos_reliable_even < 1)
        return 1;
    if (now > a->position_valid.updated + (120 * 1000))
        return 1; // no reference or older than 120 seconds, assume OK
    if (source > a->position_valid.last_source)
        return 1; // data is better quality, OVERRIDE

    elapsed = trackDataAge(now, &a->position_valid);

    speed = surface ? 150 : 900; // guess

    if (trackDataValid(&a->gs_valid)) {
        // use the larger of the current and earlier speed
        speed = (a->gs_last_pos > a->gs) ? a->gs_last_pos : a->gs;
        // add 2 knots for every second we haven't known the speed
        speed = speed + (3*trackDataAge(now, &a->gs_valid)/1000.0);
    } else if (trackDataValid(&a->tas_valid)) {
        speed = a->tas * 4 / 3;
    } else if (trackDataValid(&a->ias_valid)) {
        speed = a->ias * 2;
    }

    if (source <= SOURCE_MLAT) {
        if (elapsed > 25 * SECONDS)
            return 1;
        speed = speed * 2;
        speed = min(speed, 2400);
    }

    // Work out a reasonable speed to use:
    //  current speed + 1/3
    //  surface speed min 20kt, max 150kt
    //  airborne speed min 200kt, no max
    speed = speed * 1.3;
    if (surface) {
        if (speed < 20)
            speed = 20;
        if (speed > 150)
            speed = 150;
    } else {
        if (speed < 200)
            speed = 200;
    }

    // find actual distance
    distance = greatcircle(oldLat, oldLon, lat, lon);

    if (!surface && distance > 1 && source > SOURCE_MLAT
            && trackDataAge(now, &a->track_valid) < 7 * 1000
            && trackDataAge(now, &a->position_valid) < 7 * 1000
            && (oldLat != lat || oldLon != lon)
            && (a->pos_reliable_odd >= Modes.json_reliable && a->pos_reliable_even >= Modes.json_reliable)
       ) {
        calc_track = bearing(a->lat, a->lon, lat, lon);
        track_diff = fabs(norm_diff(a->track - calc_track, 180));
        track_bonus = speed * (90.0 - track_diff) / 90.0;
        speed += track_bonus * (1.1 - trackDataAge(now, &a->track_valid) / 5000);
        if (track_diff > 160) {
            mm->pos_ignore = 1; // don't decrement pos_reliable
        }
    }

    // 100m (surface) base distance to allow for minor errors, no airborne base distance due to ground track cross check
    // plus distance covered at the given speed for the elapsed time + 1 seconds.
    range = (surface ? 0.1e3 : 0.0e3) + ((elapsed + 1000.0) / 1000.0) * (speed * 1852.0 / 3600.0);

    inrange = (distance <= range);

    if ((source > SOURCE_MLAT && track_diff < 190 && !inrange && (Modes.debug_cpr || Modes.debug_speed_check))
            || (a->addr == Modes.cpr_focus && distance > 0.1)) {

        //fprintf(stderr, "%3.1f -> %3.1f\n", calc_track, a->track);
        fprintf(stderr, "%06x: %s %s %s %s %s R: %2d tD: %3.0f: %7.3fkm/%7.2fkm in%4.1f s, %4.0fkt/%4.0fkt, %10.6f,%11.6f->%10.6f,%11.6f\n",
                a->addr,
                source == a->position_valid.last_source ? "SQ" : "LQ",
                cpr_local == CPR_LOCAL ? "L" : (cpr_local == CPR_GLOBAL ? "G" : "O"),
                mm->cpr_odd ? "O" : "E",
                (inrange ? "  ok" : "FAIL"),
                (surface ? "S" : "A"),
                min(a->pos_reliable_odd, a->pos_reliable_even),
                track_diff,
                distance / 1000.0,
                range / 1000.0,
                elapsed / 1000.0,
                (distance / elapsed * 1000.0 / 1852.0 * 3600.0),
                speed,
                a->lat, a->lon, lat, lon);
    }

    if (!inrange && mm->source == SOURCE_ADSB
            && distance - range > 800 && track_diff > 45
            && a->pos_reliable_odd >= Modes.filter_persistence * 3 / 4
            && a->pos_reliable_even >= Modes.filter_persistence * 3 / 4
       ) {
        struct receiver *r = receiverBad(mm->receiverId, a->addr, now);
        if (r && Modes.debug_garbage && r->badCounter > 6) {
            fprintf(stderr, "hex: %06x id: %016"PRIx64" #good: %6d #bad: %3.0f trackDiff: %3.0f: %7.2fkm/%7.2fkm in %4.1f s, max %4.0f kt\n",
                    a->addr, r->id, r->goodCounter, r->badCounter,
                    track_diff,
                    distance / 1000.0,
                    range / 1000.0,
                    elapsed / 1000.0,
                    speed
                   );

        }
    }
    if (inrange && mm->source == SOURCE_ADSB && mm->cpr_type != CPR_SURFACE
            && a->pos_reliable_odd >= Modes.filter_persistence * 3 / 4
            && a->pos_reliable_even >= Modes.filter_persistence * 3 / 4
       ) {
        receiverPositionReceived(a, mm->receiverId, lat, lon, now);
    }

    return inrange;
}

static int doGlobalCPR(struct aircraft *a, struct modesMessage *mm, double *lat, double *lon, unsigned *nic, unsigned *rc) {
    int result;
    int fflag = mm->cpr_odd;
    int surface = (mm->cpr_type == CPR_SURFACE);
    struct receiver *receiver;
    double reflat, reflon;

    // derive NIC, Rc from the worse of the two position
    // smaller NIC is worse; larger Rc is worse
    *nic = (a->cpr_even_nic < a->cpr_odd_nic ? a->cpr_even_nic : a->cpr_odd_nic);
    *rc = (a->cpr_even_rc > a->cpr_odd_rc ? a->cpr_even_rc : a->cpr_odd_rc);

    if (surface) {
        // surface global CPR
        // find reference location

        if ((receiver = receiverGetReference(mm->receiverId, &reflat, &reflon, a))) {
            //function sets reflat and reflon on success, nothing to do here.
        } else if (trackDataValid(&a->position_valid)) { // Ok to try aircraft relative first
            reflat = a->lat;
            reflon = a->lon;
        } else if (Modes.bUserFlags & MODES_USER_LATLON_VALID) {
            reflat = Modes.fUserLat;
            reflon = Modes.fUserLon;
        } else if (a->seen_pos) {
            reflat = a->lat;
            reflon = a->lon;
        } else {
            // No local reference, give up
            return (-1);
        }

        result = decodeCPRsurface(reflat, reflon,
                a->cpr_even_lat, a->cpr_even_lon,
                a->cpr_odd_lat, a->cpr_odd_lon,
                fflag,
                lat, lon);

        if (Modes.debug_receiver && Modes.debug_speed_check && receiver && a->seen_pos
                && *lat < 89
                && *lat > -89
                && (fabs(a->lat - *lat) > 35 || fabs(a->lon - *lon) > 35 || fabs(reflat - *lat) > 35 || fabs(reflon - *lon) > 35)
                && !bogus_lat_lon(*lat, *lon)
           ) {
            //struct receiver *r = receiver;
            //fprintf(stderr, "id: %016"PRIx64" #pos: %9"PRIu64" lat min:%4.0f max:%4.0f lon min:%4.0f max:%4.0f\n",
            //        r->id, r->positionCounter,
            //        r->latMin, r->latMax,
            //        r->lonMin, r->lonMax);
            int sc = speed_check(a, mm->source, *lat, *lon, mm, CPR_GLOBAL);
            fprintf(stderr, "%s%06x surface CPR rec. ref.: %4.0f %4.0f sc: %d result: %7.2f %7.2f --> %7.2f %7.2f\n",
                    (a->addr & MODES_NON_ICAO_ADDRESS) ? "~" : " ",
                    a->addr, reflat, reflon, sc, a->lat, a->lon, *lat, *lon);
        }

        if (0 && Modes.debug_receiver && !(a->addr & MODES_NON_ICAO_ADDRESS)) {
            if (receiver && !trackDataValid(&a->position_valid))
                fprintf(stderr, "%06x using receiver reference: %4.0f %4.0f result: %7.2f %7.2f\n", a->addr, reflat, reflon, *lat, *lon);
        }
        if (Modes.debug_receiver && receiver && a->addr == Modes.cpr_focus)
            fprintf(stderr, "%06x using reference: %4.0f %4.0f result: %7.2f %7.2f\n", a->addr, reflat, reflon, *lat, *lon);
    } else {
        // airborne global CPR
        result = decodeCPRairborne(a->cpr_even_lat, a->cpr_even_lon,
                a->cpr_odd_lat, a->cpr_odd_lon,
                fflag,
                lat, lon);
    }

    if (result < 0) {
        if (a->addr == Modes.cpr_focus || Modes.debug_cpr) {
            fprintf(stderr, "CPR: decode failure for %06x (%d).\n", a->addr, result);
            fprintf(stderr, "  even: %d %d   odd: %d %d  fflag: %s\n",
                    a->cpr_even_lat, a->cpr_even_lon,
                    a->cpr_odd_lat, a->cpr_odd_lon,
                    fflag ? "odd" : "even");
        }
        return result;
    }

    // check max range
    if (Modes.maxRange > 0 && (Modes.bUserFlags & MODES_USER_LATLON_VALID)) {
        double range = greatcircle(Modes.fUserLat, Modes.fUserLon, *lat, *lon);
        if (range > Modes.maxRange) {
            if (a->addr == Modes.cpr_focus) {
                fprintf(stderr, "Global range check failed: %06x: %.3f,%.3f, max range %.1fkm, actual %.1fkm\n",
                        a->addr, *lat, *lon, Modes.maxRange / 1000.0, range / 1000.0);
            }
            Modes.stats_current.cpr_global_range_checks++;
            return (-2); // we consider an out-of-range value to be bad data
        }
    }

    // check speed limit
    if (!speed_check(a, mm->source, *lat, *lon, mm, CPR_GLOBAL)) {
        Modes.stats_current.cpr_global_speed_checks++;
        return -2;
    }

    return result;
}

static int doLocalCPR(struct aircraft *a, struct modesMessage *mm, double *lat, double *lon, unsigned *nic, unsigned *rc) {
    // relative CPR
    // find reference location
    double reflat, reflon;
    double range_limit = 0;
    int result;
    int fflag = mm->cpr_odd;
    int surface = (mm->cpr_type == CPR_SURFACE);
    int relative_to = 0; // aircraft(1) or receiver(2) relative

    if (fflag) {
        *nic = a->cpr_odd_nic;
        *rc = a->cpr_odd_rc;
    } else {
        *nic = a->cpr_even_nic;
        *rc = a->cpr_even_rc;
    }

    uint64_t now = mm->sysTimestampMsg;
    if (now < a->seenPosGlobal + 10 * MINUTES && trackDataValid(&a->position_valid)
            && now < a->position_valid.updated + (10*60*1000)) {
        reflat = a->lat;
        reflon = a->lon;

        if (a->pos_nic < *nic)
            *nic = a->pos_nic;
        if (a->pos_rc < *rc)
            *rc = a->pos_rc;

        range_limit = 1852*100; // 100NM
        // 100 NM in the 10 minutes of position validity means 600 knots which
        // is fast but happens even for commercial airliners.
        // It's not a problem if this limitation fails every now and then.
        // A wrong relative position decode would require the aircraft to
        // travel 360-100=260 NM in the 10 minutes of position validity.
        // This is impossible for planes slower than 1560 knots/Mach 2.3 over the ground.
        // Thus this range limit combined with the 10 minutes of position
        // validity should not provide bad positions (1 cell away).

        relative_to = 1;
    } else if (!surface && (Modes.bUserFlags & MODES_USER_LATLON_VALID)) {
        reflat = Modes.fUserLat;
        reflon = Modes.fUserLon;

        // The cell size is at least 360NM, giving a nominal
        // max range of 180NM (half a cell).
        //
        // If the receiver range is more than half a cell
        // then we must limit this range further to avoid
        // ambiguity. (e.g. if we receive a position report
        // at 200NM distance, this may resolve to a position
        // at (200-360) = 160NM in the wrong direction)

        if (Modes.maxRange == 0) {
            return (-1); // Can't do receiver-centered checks at all
        } else if (Modes.maxRange <= 1852 * 180) {
            range_limit = Modes.maxRange;
        } else if (Modes.maxRange < 1852 * 360) {
            range_limit = (1852 * 360) - Modes.maxRange;
        } else {
            return (-1); // Can't do receiver-centered checks at all
        }
        relative_to = 2;
    } else {
        // No local reference, give up
        return (-1);
    }

    result = decodeCPRrelative(reflat, reflon,
            mm->cpr_lat,
            mm->cpr_lon,
            fflag, surface,
            lat, lon);
    if (result < 0) {
        return result;
    }

    // check range limit
    if (range_limit > 0) {
        double range = greatcircle(reflat, reflon, *lat, *lon);
        if (range > range_limit) {
            Modes.stats_current.cpr_local_range_checks++;
            return (-1);
        }
    }

    // check speed limit
    if (!speed_check(a, mm->source, *lat, *lon, mm, CPR_LOCAL)) {
        Modes.stats_current.cpr_local_speed_checks++;
        return -2;
    }

    return relative_to;
}

static uint64_t time_between(uint64_t t1, uint64_t t2) {
    if (t1 >= t2)
        return t1 - t2;
    else
        return t2 - t1;
}


static void setPosition(struct aircraft *a, struct modesMessage *mm, uint64_t now) {
    if (0 && a->addr == Modes.cpr_focus) {
        showPositionDebug(a, mm, now);
    }

    if (now < a->seen_pos + 3 * SECONDS && a->lat == mm->decoded_lat && a->lon == mm->decoded_lon) {
        // don't use duplicate positions for beastReduce
        mm->reduce_forward = 0;
        mm->duplicate = 1;
        mm->pos_ignore = 1;
    }

    Modes.stats_current.pos_by_type[mm->addrtype]++;
    Modes.stats_current.pos_all++;

    // mm->pos_bad should never arrive here, handle it just in case
    if (mm->cpr_valid && (mm->garbage || mm->pos_bad)) {
        Modes.stats_current.pos_garbage++;
        return;
    }

    if (mm->source == SOURCE_MLAT) {
        a->receiverCountMlat = mm->receiverCountMlat;
    } else {
        uint16_t simpleHash = (uint16_t) mm->receiverId;
        simpleHash = simpleHash ? simpleHash : 1;
        a->receiverIds[a->receiverIdsNext++ % RECEIVERIDBUFFER] = simpleHash;
    }

    if (mm->duplicate) {
        Modes.stats_current.pos_duplicate++;
        return;
    }

    if (mm->client) {
        mm->client->positionCounter++;
    }

    if (trackDataAge(now, &a->track_valid) >= 10 * SECONDS && a->seen_pos) {
        double distance = greatcircle(a->lat, a->lon, mm->decoded_lat, mm->decoded_lon);
        if (distance > 100)
            a->calc_track = bearing(a->lat, a->lon, mm->decoded_lat, mm->decoded_lon);
        if (mm->source == SOURCE_JAERO
                && (a->position_valid.last_source == SOURCE_JAERO || trackDataAge(now, &a->position_valid) >= 30 * MINUTES)
                && trackDataAge(now, &a->track_valid) > TRACK_EXPIRE
                && distance > 10e3 // more than 10 km
           ) {
            accept_data(&a->track_valid, SOURCE_JAERO, mm, 2);
            a->track = a->calc_track;
        }
    }

    // Update aircraft state
    a->lat = mm->decoded_lat;
    a->lon = mm->decoded_lon;
    a->pos_nic = mm->decoded_nic;
    a->pos_rc = mm->decoded_rc;

    a->lastPosReceiverId = mm->receiverId;

    if (posReliable(a)) {
        set_globe_index(a, globe_index(a->lat, a->lon));

        if (traceAdd(a, now)) {
            mm->jsonPos = 1;
            //fprintf(stderr, "Added to trace for %06x (%d).\n", a->addr, a->trace_len);
        }


        a->seenPosReliable = now; // must be after traceAdd for trace stale detection
        a->latReliable = mm->decoded_lat;
        a->lonReliable = mm->decoded_lon;
    }

    a->pos_surface = trackDataValid(&a->airground_valid) && a->airground == AG_GROUND;

    if (mm->jsonPos)
        jsonPositionOutput(mm, a);

    if (a->pos_reliable_odd >= 2 && a->pos_reliable_even >= 2 && mm->source == SOURCE_ADSB) {
        update_range_histogram(mm->decoded_lat, mm->decoded_lon);
    }

    a->seen_pos = now;

    // update addrtype, we use the type from the accepted position.
    a->addrtype = mm->addrtype;
    a->addrtype_updated = now;
}

static void updatePosition(struct aircraft *a, struct modesMessage *mm, uint64_t now) {
    int location_result = -1;
    int globalCPR = 0;
    uint64_t max_elapsed;
    double new_lat = 0, new_lon = 0;
    unsigned new_nic = 0;
    unsigned new_rc = 0;
    int surface;

    surface = (mm->cpr_type == CPR_SURFACE);
    a->pos_surface = trackDataValid(&a->airground_valid) && a->airground == AG_GROUND;

    if (surface) {
        ++Modes.stats_current.cpr_surface;

        // Surface: 25 seconds if >25kt or speed unknown, 50 seconds otherwise
        if (mm->gs_valid && mm->gs.selected <= 25)
            max_elapsed = 50000;
        else
            max_elapsed = 25000;
    } else {
        ++Modes.stats_current.cpr_airborne;

        // Airborne: 10 seconds
        max_elapsed = 10000;
    }

    // If we have enough recent data, try global CPR
    if (trackDataValid(&a->cpr_odd_valid) && trackDataValid(&a->cpr_even_valid) &&
            a->cpr_odd_valid.source == a->cpr_even_valid.source &&
            a->cpr_odd_type == a->cpr_even_type &&
            time_between(a->cpr_odd_valid.updated, a->cpr_even_valid.updated) <= max_elapsed) {

        location_result = doGlobalCPR(a, mm, &new_lat, &new_lon, &new_nic, &new_rc);

        //if (a->addr == Modes.cpr_focus)
        //    fprintf(stderr, "%06x globalCPR result: %d\n", a->addr, location_result);

        if (location_result == -2) {
            // Global CPR failed because the position produced implausible results.
            // This is bad data.

            mm->pos_bad = 1;
            return;
        } else if (location_result == -1) {
            if (a->addr == Modes.cpr_focus || Modes.debug_cpr) {
                if (mm->source == SOURCE_MLAT) {
                    fprintf(stderr, "CPR skipped from MLAT (%06x).\n", a->addr);
                }
            }
            // No local reference for surface position available, or the two messages crossed a zone.
            // Nonfatal, try again later.
            Modes.stats_current.cpr_global_skipped++;
        } else {
            if (accept_data(&a->position_valid, mm->source, mm, 2)) {
                Modes.stats_current.cpr_global_ok++;

                globalCPR = 1;
            } else {
                Modes.stats_current.cpr_global_skipped++;
                location_result = -2;
            }
        }
    }

    // Otherwise try relative CPR.
    if (location_result == -1) {
        location_result = doLocalCPR(a, mm, &new_lat, &new_lon, &new_nic, &new_rc);

        if (location_result == -2) {
            // Local CPR failed because the position produced implausible results.
            // This is bad data.

            mm->pos_bad = 1;
            return;
        }

        //if (a->addr == Modes.cpr_focus)
        //    fprintf(stderr, "%06x: localCPR: %d\n", a->addr, location_result);

        if (location_result >= 0 && accept_data(&a->position_valid, mm->source, mm, 2)) {
            Modes.stats_current.cpr_local_ok++;
            mm->cpr_relative = 1;

            if (location_result == 1) {
                Modes.stats_current.cpr_local_aircraft_relative++;
            }
            if (location_result == 2) {
                Modes.stats_current.cpr_local_receiver_relative++;
            }
        } else {
            Modes.stats_current.cpr_local_skipped++;
            location_result = -1;
        }
    }

    if (location_result == -1 && a->addr == Modes.cpr_focus) {
        fprintf(stderr, "-1: mm->cpr: %s %s, other CPR age %0.1f sources %s %s %s %s odd_t: %s even_t: %s\n",
                mm->cpr_odd ? " odd" : "even", cpr_type_string(mm->cpr_type),
                mm->cpr_odd ? fmin(999, ((double) now - a->cpr_even_valid.updated) / 1000.0) : fmin(999, ((double) now - a->cpr_odd_valid.updated) / 1000.0),
                source_enum_string(a->cpr_odd_valid.source), source_enum_string(a->cpr_even_valid.source),
                source_enum_string(a->cpr_odd_valid.last_source), source_enum_string(a->cpr_even_valid.last_source),
                cpr_type_string(a->cpr_odd_type), cpr_type_string(a->cpr_even_type));
    }

    if (location_result >= 0) {
        // If we sucessfully decoded, back copy the results to mm
        mm->cpr_decoded = 1;
        mm->decoded_lat = new_lat;
        mm->decoded_lon = new_lon;
        mm->decoded_nic = new_nic;
        mm->decoded_rc = new_rc;

        if (trackDataValid(&a->gs_valid))
            a->gs_last_pos = a->gs;

        if (globalCPR)
            incrementReliable(a, mm, now, mm->cpr_odd);

        setPosition(a, mm, now);
    }

}

static unsigned compute_nic(unsigned metype, unsigned version, unsigned nic_a, unsigned nic_b, unsigned nic_c) {
    switch (metype) {
        case 5: // surface
        case 9: // airborne
        case 20: // airborne, GNSS altitude
            return 11;

        case 6: // surface
        case 10: // airborne
        case 21: // airborne, GNSS altitude
            return 10;

        case 7: // surface
            if (version == 2) {
                if (nic_a && !nic_c) {
                    return 9;
                } else {
                    return 8;
                }
            } else if (version == 1) {
                if (nic_a) {
                    return 9;
                } else {
                    return 8;
                }
            } else {
                return 8;
            }

        case 8: // surface
            if (version == 2) {
                if (nic_a && nic_c) {
                    return 7;
                } else if (nic_a && !nic_c) {
                    return 6;
                } else if (!nic_a && nic_c) {
                    return 6;
                } else {
                    return 0;
                }
            } else {
                return 0;
            }

        case 11: // airborne
            if (version == 2) {
                if (nic_a && nic_b) {
                    return 9;
                } else {
                    return 8;
                }
            } else if (version == 1) {
                if (nic_a) {
                    return 9;
                } else {
                    return 8;
                }
            } else {
                return 8;
            }

        case 12: // airborne
            return 7;

        case 13: // airborne
            return 6;

        case 14: // airborne
            return 5;

        case 15: // airborne
            return 4;

        case 16: // airborne
            if (nic_a && nic_b) {
                return 3;
            } else {
                return 2;
            }

        case 17: // airborne
            return 1;

        default:
            return 0;
    }
}

static unsigned compute_rc(unsigned metype, unsigned version, unsigned nic_a, unsigned nic_b, unsigned nic_c) {
    switch (metype) {
        case 5: // surface
        case 9: // airborne
        case 20: // airborne, GNSS altitude
            return 8; // 7.5m

        case 6: // surface
        case 10: // airborne
        case 21: // airborne, GNSS altitude
            return 25;

        case 7: // surface
            if (version == 2) {
                if (nic_a && !nic_c) {
                    return 75;
                } else {
                    return 186; // 185.2m, 0.1NM
                }
            } else if (version == 1) {
                if (nic_a) {
                    return 75;
                } else {
                    return 186; // 185.2m, 0.1NM
                }
            } else {
                return 186; // 185.2m, 0.1NM
            }

        case 8: // surface
            if (version == 2) {
                if (nic_a && nic_c) {
                    return 371; // 370.4m, 0.2NM
                } else if (nic_a && !nic_c) {
                    return 556; // 555.6m, 0.3NM
                } else if (!nic_a && nic_c) {
                    return 926; // 926m, 0.5NM
                } else {
                    return RC_UNKNOWN;
                }
            } else {
                return RC_UNKNOWN;
            }

        case 11: // airborne
            if (version == 2) {
                if (nic_a && nic_b) {
                    return 75;
                } else {
                    return 186; // 370.4m, 0.2NM
                }
            } else if (version == 1) {
                if (nic_a) {
                    return 75;
                } else {
                    return 186; // 370.4m, 0.2NM
                }
            } else {
                return 186; // 370.4m, 0.2NM
            }

        case 12: // airborne
            return 371; // 370.4m, 0.2NM

        case 13: // airborne
            if (version == 2) {
                if (!nic_a && nic_b) {
                    return 556; // 555.6m, 0.3NM
                } else if (!nic_a && !nic_b) {
                    return 926; // 926m, 0.5NM
                } else if (nic_a && nic_b) {
                    return 1112; // 1111.2m, 0.6NM
                } else {
                    return RC_UNKNOWN; // bad combination, assume worst Rc
                }
            } else if (version == 1) {
                if (nic_a) {
                    return 1112; // 1111.2m, 0.6NM
                } else {
                    return 926; // 926m, 0.5NM
                }
            } else {
                return 926; // 926m, 0.5NM
            }

        case 14: // airborne
            return 1852; // 1.0NM

        case 15: // airborne
            return 3704; // 2NM

        case 16: // airborne
            if (version == 2) {
                if (nic_a && nic_b) {
                    return 7408; // 4NM
                } else {
                    return 14816; // 8NM
                }
            } else if (version == 1) {
                if (nic_a) {
                    return 7408; // 4NM
                } else {
                    return 14816; // 8NM
                }
            } else {
                return 18520; // 10NM
            }

        case 17: // airborne
            return 37040; // 20NM

        default:
            return RC_UNKNOWN;
    }
}

// Map ADS-B v0 position message type to NACp value
// returned computed NACp, or -1 if not a suitable message type
static int compute_v0_nacp(struct modesMessage *mm)
{
    if (mm->msgtype != 17 && mm->msgtype != 18) {
        return -1;
    }

    // ED-102A Table N-7
    switch (mm->metype) {
    case 0: return 0;
    case 5: return 11;
    case 6: return 10;
    case 7: return 8;
    case 8: return 0;
    case 9: return 11;
    case 10: return 10;
    case 11: return 8;
    case 12: return 7;
    case 13: return 6;
    case 14: return 5;
    case 15: return 4;
    case 16: return 1;
    case 17: return 1;
    case 18: return 0;
    case 20: return 11;
    case 21: return 10;
    case 22: return 0;
    default: return -1;
    }
}

// Map ADS-B v0 position message type to SIL value
// returned computed SIL, or -1 if not a suitable message type
static int compute_v0_sil(struct modesMessage *mm)
{
    if (mm->msgtype != 17 && mm->msgtype != 18) {
        return -1;
    }

    // ED-102A Table N-8
    switch (mm->metype) {
    case 0:
        return 0;

    case 5:
    case 6:
    case 7:
    case 8:
    case 9:
    case 10:
    case 11:
    case 12:
    case 13:
    case 14:
    case 15:
    case 16:
    case 17:
        return 2;

    case 18:
        return 0;

    case 20:
    case 21:
        return 2;

    case 22:
        return 0;

    default:
        return -1;
    }
}

static void compute_nic_rc_from_message(struct modesMessage *mm, struct aircraft *a, unsigned *nic, unsigned *rc) {
    int nic_a = (trackDataValid(&a->nic_a_valid) && a->nic_a);
    int nic_b = (mm->accuracy.nic_b_valid && mm->accuracy.nic_b);
    int nic_c = (trackDataValid(&a->nic_c_valid) && a->nic_c);

    *nic = compute_nic(mm->metype, a->adsb_version, nic_a, nic_b, nic_c);
    *rc = compute_rc(mm->metype, a->adsb_version, nic_a, nic_b, nic_c);
}

static int altitude_to_feet(int raw, altitude_unit_t unit) {
    switch (unit) {
        case UNIT_METERS:
            return raw / 0.3048;
        case UNIT_FEET:
            return raw;
        default:
            return 0;
    }
}

// check if we trust that this message is actually from the aircraft with this address
// similar reasoning to icaoFilterAdd in mode_s.c
static int addressReliable(struct modesMessage *mm) {
    if (mm->msgtype == 17 || mm->msgtype == 18 || (mm->msgtype == 11 && mm->IID == 0) || mm->sbs_in) {
        return 1;
    }
    return 0;
}

static inline void focusGroundstateChange(struct aircraft *a, struct modesMessage *mm, int arg, uint64_t now) {
    if (a->addr == Modes.cpr_focus && a->airground != mm->airground) {
        fprintf(stderr, "%4.1fs Ground state change %d: Source: %s, %s -> %s\n",
                (now % (600 * SECONDS)) / 1000.0,
                arg,
                source_enum_string(mm->source),
                airground_to_string(a->airground),
                airground_to_string(mm->airground));
    }
}

//
//=========================================================================
//
// Receive new messages and update tracked aircraft state
//

struct aircraft *trackUpdateFromMessage(struct modesMessage *mm) {
    struct aircraft *a;
    unsigned int cpr_new = 0;

    if (mm->msgtype == 32) {
        // Mode A/C, just count it (we ignore SPI)
        modeAC_count[modeAToIndex(mm->squawk)]++;
        return NULL;
    }

    uint64_t now = mm->sysTimestampMsg;

    // Lookup our aircraft or create a new one
    a = aircraftGet(mm->addr);
    if (!a) { // If it's a currently unknown aircraft....
        if (addressReliable(mm)) {
            a = aircraftCreate(mm); // ., create a new record for it,
        } else {
            //fprintf(stderr, "%06x: !a && !addressReliable(mm)\n", mm->addr);
            return NULL;
        }
    }

    bool haveScratch = false;
    if (mm->cpr_valid || mm->sbs_pos_valid) {
        memcpy(Modes.scratch, a, sizeof(struct aircraft));
        haveScratch = true;
        // messages from receivers classified garbage with position get processed to see if they still send garbage
    } else if (mm->garbage) {
        return NULL;
    }

    // only count the aircraft as "seen" for reliable messages with CRC
    if (addressReliable(mm)) {
        //if (now > a->seen + Modes.trackExpireMax)
            //ca_add(&Modes.activeAircraft, a);
        a->seen = now;
    }

    // don't use messages with unreliable CRC too long after receiving a reliable address from an aircraft
    if (now > a->seen + 45 * SECONDS) {
        return NULL;
    }

    if (mm->signalLevel > 0) {

        a->signalLevel[a->signalNext] = mm->signalLevel;
        a->signalNext = (a->signalNext + 1) & 7;

        if (a->no_signal_count >= 10) {
            for (int i = 0; i < 8; ++i) {
                a->signalLevel[i] = fmax(0, mm->signalLevel);
            }
        }
        if (a->no_signal_count > 0)
            a->no_signal_count = 0;
    } else {
        // if we haven't received a message with signal level for a bit, set it to zero
        if (a->no_signal_count < 10 && ++a->no_signal_count >= 10) {
            for (int i = 0; i < 8; ++i) {
                a->signalLevel[i] = 0;
            }
        }
    }

    // reset to 100000 on overflow ... avoid any low message count checks
    if (a->messages == UINT32_MAX)
        a->messages = 100000;

    a->messages++;

    if (mm->client && !mm->garbage) {
        mm->client->messageCounter++;
    }

    // update addrtype
    if (a->addrtype_updated > now)
        a->addrtype_updated = now;

    if (
            (mm->addrtype <= a->addrtype && now > 30 * 1000 + a->addrtype_updated)
            || (mm->addrtype > a->addrtype && now > 90 * 1000 + a->addrtype_updated)
       ) {

        a->addrtype = mm->addrtype;
        a->addrtype_updated = now;

        if (a->addrtype > ADDR_ADSB_ICAO_NT) {
            a->adsb_version = -1; // reset ADS-B version if a non ADS-B message type is received
        }
    }

    // decide on where to stash the version
    int dummy_version = -1; // used for non-adsb/adsr/tisb messages
    int *message_version;

    switch (mm->source) {
    case SOURCE_ADSB:
        message_version = &a->adsb_version;
        break;
    case SOURCE_TISB:
        message_version = &a->tisb_version;
        break;
    case SOURCE_ADSR:
        message_version = &a->adsr_version;
        break;
    default:
        message_version = &dummy_version;
        break;
    }

    // assume version 0 until we see something else
    if (*message_version < 0) {
        *message_version = 0;
    }

    if (mm->category_valid) {
        a->category = mm->category;
        a->category_updated = now;
    }

    // operational status message
    // done early to update version / HRD / TAH
    if (mm->opstatus.valid) {
        *message_version = mm->opstatus.version;
        
        if (mm->opstatus.hrd != HEADING_INVALID) {
            a->adsb_hrd = mm->opstatus.hrd;
        }
        if (mm->opstatus.tah != HEADING_INVALID) {
            a->adsb_tah = mm->opstatus.tah;
        }
    }

    // fill in ADS-B v0 NACp, SIL from position message type
    if (*message_version == 0 && !mm->accuracy.nac_p_valid) {
        int computed_nacp = compute_v0_nacp(mm);
        if (computed_nacp != -1) {
            mm->accuracy.nac_p_valid = 1;
            mm->accuracy.nac_p = computed_nacp;
        }
    }

    if (*message_version == 0 && mm->accuracy.sil_type == SIL_INVALID) {
        int computed_sil = compute_v0_sil(mm);
        if (computed_sil != -1) {
            mm->accuracy.sil_type = SIL_UNKNOWN;
            mm->accuracy.sil = computed_sil;
        }
    }

    if (mm->altitude_baro_valid &&
            (mm->source >= a->altitude_baro_valid.source ||
             (trackDataAge(now, &a->altitude_baro_valid) > 10 * 1000
              && a->altitude_baro_valid.source != SOURCE_JAERO
              && a->altitude_baro_valid.source != SOURCE_SBS)
            )
       ) {
        int alt = altitude_to_feet(mm->altitude_baro, mm->altitude_baro_unit);
        if (a->modeC_hit) {
            int new_modeC = (a->altitude_baro + 49) / 100;
            int old_modeC = (alt + 49) / 100;
            if (new_modeC != old_modeC) {
                a->modeC_hit = 0;
            }
        }

        int delta = alt - a->altitude_baro;
        int fpm = 0;

        int max_fpm = 12500;
        int min_fpm = -12500;

        if (abs(delta) >= 300) {
            fpm = delta*60*10/(abs((int)trackDataAge(now, &a->altitude_baro_valid)/100)+10);
            if (trackDataValid(&a->geom_rate_valid) && trackDataAge(now, &a->geom_rate_valid) < trackDataAge(now, &a->baro_rate_valid)) {
                min_fpm = a->geom_rate - 1500 - min(11000, ((int)trackDataAge(now, &a->geom_rate_valid)/2));
                max_fpm = a->geom_rate + 1500 + min(11000, ((int)trackDataAge(now, &a->geom_rate_valid)/2));
            } else if (trackDataValid(&a->baro_rate_valid)) {
                min_fpm = a->baro_rate - 1500 - min(11000, ((int)trackDataAge(now, &a->baro_rate_valid)/2));
                max_fpm = a->baro_rate + 1500 + min(11000, ((int)trackDataAge(now, &a->baro_rate_valid)/2));
            }
            if (trackDataValid(&a->altitude_baro_valid) && trackDataAge(now, &a->altitude_baro_valid) < 30 * SECONDS) {
                a->alt_reliable = min(
                        ALTITUDE_BARO_RELIABLE_MAX - (ALTITUDE_BARO_RELIABLE_MAX*trackDataAge(now, &a->altitude_baro_valid)/(30 * SECONDS)),
                        a->alt_reliable);
            } else {
                a->alt_reliable = 0;
            }
        }
        int good_crc = (mm->crc == 0 && mm->source >= SOURCE_JAERO) ? 4 : 0;

        if (mm->source == SOURCE_SBS || mm->source == SOURCE_MLAT)
            good_crc = ALTITUDE_BARO_RELIABLE_MAX/2 - 1;

        if (a->altitude_baro > 50175 && mm->alt_q_bit && a->alt_reliable > ALTITUDE_BARO_RELIABLE_MAX/4) {
            good_crc = 0;
            //fprintf(stderr, "q_bit == 1 && a->alt > 50175: %06x\n", a->addr);
            goto discard_alt;
        }

        if (a->alt_reliable <= 0  || abs(delta) < 300)
            goto accept_alt;
        if (fpm < max_fpm && fpm > min_fpm)
            goto accept_alt;
        if (good_crc > a->alt_reliable)
            goto accept_alt;
        if (mm->source > a->altitude_baro_valid.source)
            goto accept_alt;
        if (mm->source == SOURCE_JAERO && (a->altitude_baro_valid.source == SOURCE_JAERO || a->altitude_baro_valid.source == SOURCE_INVALID)) {
            good_crc = ALTITUDE_BARO_RELIABLE_MAX;
            goto accept_alt;
        }

        goto discard_alt;
accept_alt:
            if (accept_data(&a->altitude_baro_valid, mm->source, mm, 2)) {
                a->alt_reliable = min(ALTITUDE_BARO_RELIABLE_MAX , a->alt_reliable + (good_crc+1));
                if (0 && a->addr == 0x4b2917 && abs(delta) > -1 && delta != alt) {
                    fprintf(stderr, "Alt check S: %06x: %2d %6d ->%6d, %s->%s, min %.1f kfpm, max %.1f kfpm, actual %.1f kfpm\n",
                            a->addr, a->alt_reliable, a->altitude_baro, alt,
                            source_string(a->altitude_baro_valid.source),
                            source_string(mm->source),
                            min_fpm/1000.0, max_fpm/1000.0, fpm/1000.0);
                }
                a->altitude_baro = alt;
            }
            goto end_alt;
discard_alt:
            a->alt_reliable = a->alt_reliable - (good_crc+1);
            if (0 && a->addr == 0x4b2917)
                fprintf(stderr, "Alt check F: %06x: %2d %6d ->%6d, %s->%s, min %.1f kfpm, max %.1f kfpm, actual %.1f kfpm\n",
                        a->addr, a->alt_reliable, a->altitude_baro, alt,
                        source_string(a->altitude_baro_valid.source),
                        source_string(mm->source),
                        min_fpm/1000.0, max_fpm/1000.0, fpm/1000.0);
            if (a->alt_reliable <= 0) {
                //fprintf(stderr, "Altitude INVALIDATED: %06x\n", a->addr);
                a->alt_reliable = 0;
                if (a->position_valid.source > SOURCE_JAERO)
                    a->altitude_baro_valid.source = SOURCE_INVALID;
            }
            if (Modes.garbage_ports)
                mm->source = SOURCE_INVALID;
end_alt:
            ;
    }

    if (mm->squawk_valid && accept_data(&a->squawk_valid, mm->source, mm, 0)) {
        if (mm->squawk != a->squawk) {
            a->modeA_hit = 0;
        }
        a->squawk = mm->squawk;

#if 0   // Disabled for now as it obscures the origin of the data
        // Handle 7x00 without a corresponding emergency status
        if (!mm->emergency_valid) {
            emergency_t squawk_emergency;
            switch (mm->squawk) {
                case 0x7500:
                    squawk_emergency = EMERGENCY_UNLAWFUL;
                    break;
                case 0x7600:
                    squawk_emergency = EMERGENCY_NORDO;
                    break;
                case 0x7700:
                    squawk_emergency = EMERGENCY_GENERAL;
                    break;
                default:
                    squawk_emergency = EMERGENCY_NONE;
                    break;
            }

            if (squawk_emergency != EMERGENCY_NONE && accept_data(&a->emergency_valid, mm->source, mm, 0)) {
                a->emergency = squawk_emergency;
            }
        }
#endif
    }

    if (mm->emergency_valid && accept_data(&a->emergency_valid, mm->source, mm, 0)) {
        a->emergency = mm->emergency;
    }

    if (mm->altitude_geom_valid && accept_data(&a->altitude_geom_valid, mm->source, mm, 1)) {
        a->altitude_geom = altitude_to_feet(mm->altitude_geom, mm->altitude_geom_unit);
    }

    if (mm->geom_delta_valid && accept_data(&a->geom_delta_valid, mm->source, mm, 1)) {
        a->geom_delta = mm->geom_delta;
    }

    if (mm->heading_valid) {
        heading_type_t htype = mm->heading_type;
        if (htype == HEADING_MAGNETIC_OR_TRUE) {
            htype = a->adsb_hrd;
        } else if (htype == HEADING_TRACK_OR_HEADING) {
            htype = a->adsb_tah;
        }

        if (htype == HEADING_GROUND_TRACK && accept_data(&a->track_valid, mm->source, mm, 2)) {
            a->track = mm->heading;
        } else if (htype == HEADING_MAGNETIC) {
            double dec;
            int err = declination(a, &dec);
            if (accept_data(&a->mag_heading_valid, mm->source, mm, 1)) {
                a->mag_heading = mm->heading;

                // don't accept more than 45 degree crab when deriving the true heading
                if (
                        (!trackDataValid(&a->track_valid) || fabs(norm_diff(mm->heading + dec - a->track, 180)) < 45)
                        && !err && accept_data(&a->true_heading_valid, SOURCE_INDIRECT, mm, 1)
                   ) {
                    a->true_heading = norm_angle(mm->heading + dec, 180);
                    calc_wind(a, now);
                }
            }
        } else if (htype == HEADING_TRUE && accept_data(&a->true_heading_valid, mm->source, mm, 1)) {
            a->true_heading = mm->heading;
        }
    }

    if (mm->track_rate_valid && accept_data(&a->track_rate_valid, mm->source, mm, 1)) {
        a->track_rate = mm->track_rate;
    }

    if (mm->roll_valid && accept_data(&a->roll_valid, mm->source, mm, 1)) {
        a->roll = mm->roll;
    }

    if (mm->gs_valid) {
        mm->gs.selected = (*message_version == 2 ? mm->gs.v2 : mm->gs.v0);
        if (accept_data(&a->gs_valid, mm->source, mm, 2)) {
            a->gs = mm->gs.selected;
        }
    }

    if (mm->ias_valid && accept_data(&a->ias_valid, mm->source, mm, 1)) {
        a->ias = mm->ias;
    }

    if (mm->tas_valid
            && !(trackDataValid(&a->ias_valid) && mm->tas < a->ias)
            && accept_data(&a->tas_valid, mm->source, mm, 1)) {
        a->tas = mm->tas;
        calc_temp(a, now);
        calc_wind(a, now);
    }

    if (mm->mach_valid && accept_data(&a->mach_valid, mm->source, mm, 1)) {
        a->mach = mm->mach;
        calc_temp(a, now);
    }

    if (mm->baro_rate_valid && accept_data(&a->baro_rate_valid, mm->source, mm, 2)) {
        a->baro_rate = mm->baro_rate;
    }

    if (mm->geom_rate_valid && accept_data(&a->geom_rate_valid, mm->source, mm, 2)) {
        a->geom_rate = mm->geom_rate;
    }

    if (mm->airground != AG_INVALID && mm->source != SOURCE_MODE_S &&
            !(a->last_cpr_type == CPR_SURFACE && mm->airground == AG_AIRBORNE && now < a->airground_valid.updated + TRACK_EXPIRE_LONG)
       ) {
        // If our current state is UNCERTAIN, accept new data as normal
        // If our current state is certain but new data is not, only accept the uncertain state if the certain data has gone stale
        if (a->airground == AG_UNCERTAIN || mm->airground != AG_UNCERTAIN ||
                (mm->airground == AG_UNCERTAIN && now > a->airground_valid.updated + TRACK_EXPIRE_LONG)) {
            if (mm->airground != a->airground)
                mm->reduce_forward = 1;
            if (accept_data(&a->airground_valid, mm->source, mm, 0)) {
                focusGroundstateChange(a, mm, 1, now);
                a->airground = mm->airground;

                //if (a->airground == AG_GROUND && mm->source == SOURCE_MODE_S) {
            }
        }
    }

    if (mm->callsign_valid && accept_data(&a->callsign_valid, mm->source, mm, 0)) {
        memcpy(a->callsign, mm->callsign, sizeof (a->callsign));
    }

    if (mm->nav.mcp_altitude_valid && accept_data(&a->nav_altitude_mcp_valid, mm->source, mm, 0)) {
        a->nav_altitude_mcp = mm->nav.mcp_altitude;
    }

    if (mm->nav.fms_altitude_valid && accept_data(&a->nav_altitude_fms_valid, mm->source, mm, 0)) {
        a->nav_altitude_fms = mm->nav.fms_altitude;
    }

    if (mm->nav.altitude_source != NAV_ALT_INVALID && accept_data(&a->nav_altitude_src_valid, mm->source, mm, 0)) {
        a->nav_altitude_src = mm->nav.altitude_source;
    }

    if (mm->nav.heading_valid && accept_data(&a->nav_heading_valid, mm->source, mm, 0)) {
        a->nav_heading = mm->nav.heading;
    }

    if (mm->nav.modes_valid && accept_data(&a->nav_modes_valid, mm->source, mm, 0)) {
        a->nav_modes = mm->nav.modes;
    }

    if (mm->nav.qnh_valid && accept_data(&a->nav_qnh_valid, mm->source, mm, 0)) {
        a->nav_qnh = mm->nav.qnh;
    }

    if (mm->alert_valid && accept_data(&a->alert_valid, mm->source, mm, 0)) {
        a->alert = mm->alert;
    }

    if (mm->spi_valid && accept_data(&a->spi_valid, mm->source, mm, 0)) {
        a->spi = mm->spi;
    }

    // forward all CPRs to the apex for faster garbage detection
    if (Modes.netIngest && mm->cpr_valid) {
        mm->reduce_forward = 1;
    }

    // CPR, even
    if (mm->cpr_valid && !mm->cpr_odd && accept_data(&a->cpr_even_valid, mm->source, mm, 1)) {
        a->cpr_even_type = mm->cpr_type;
        a->cpr_even_lat = mm->cpr_lat;
        a->cpr_even_lon = mm->cpr_lon;
        compute_nic_rc_from_message(mm, a, &a->cpr_even_nic, &a->cpr_even_rc);
        cpr_new = 1;
    }

    // CPR, odd
    if (mm->cpr_valid && mm->cpr_odd && accept_data(&a->cpr_odd_valid, mm->source, mm, 1)) {
        a->cpr_odd_type = mm->cpr_type;
        a->cpr_odd_lat = mm->cpr_lat;
        a->cpr_odd_lon = mm->cpr_lon;
        compute_nic_rc_from_message(mm, a, &a->cpr_odd_nic, &a->cpr_odd_rc);
        cpr_new = 1;
    }

    if (mm->accuracy.sda_valid && accept_data(&a->sda_valid, mm->source, mm, 0)) {
        a->sda = mm->accuracy.sda;
    }

    if (mm->accuracy.nic_a_valid && accept_data(&a->nic_a_valid, mm->source, mm, 0)) {
        a->nic_a = mm->accuracy.nic_a;
    }

    if (mm->accuracy.nic_c_valid && accept_data(&a->nic_c_valid, mm->source, mm, 0)) {
        a->nic_c = mm->accuracy.nic_c;
    }

    if (mm->accuracy.nic_baro_valid && accept_data(&a->nic_baro_valid, mm->source, mm, 0)) {
        a->nic_baro = mm->accuracy.nic_baro;
    }

    if (mm->accuracy.nac_p_valid && accept_data(&a->nac_p_valid, mm->source, mm, 0)) {
        a->nac_p = mm->accuracy.nac_p;
    }

    if (mm->accuracy.nac_v_valid && accept_data(&a->nac_v_valid, mm->source, mm, 0)) {
        a->nac_v = mm->accuracy.nac_v;
    }

    if (mm->accuracy.sil_type != SIL_INVALID && accept_data(&a->sil_valid, mm->source, mm, 0)) {
        a->sil = mm->accuracy.sil;
        if (a->sil_type == SIL_INVALID || mm->accuracy.sil_type != SIL_UNKNOWN) {
            a->sil_type = mm->accuracy.sil_type;
        }
    }

    if (mm->accuracy.gva_valid && accept_data(&a->gva_valid, mm->source, mm, 0)) {
        a->gva = mm->accuracy.gva;
    }

    if (mm->accuracy.sda_valid && accept_data(&a->sda_valid, mm->source, mm, 0)) {
        a->sda = mm->accuracy.sda;
    }

    // Now handle derived data

    // derive geometric altitude if we have baro + delta
    if (a->alt_reliable >= Modes.json_reliable + 1 && compare_validity(&a->altitude_baro_valid, &a->altitude_geom_valid) > 0 &&
            compare_validity(&a->geom_delta_valid, &a->altitude_geom_valid) > 0) {
        // Baro and delta are both more recent than geometric, derive geometric from baro + delta
        a->altitude_geom = a->altitude_baro + a->geom_delta;
        combine_validity(&a->altitude_geom_valid, &a->altitude_baro_valid, &a->geom_delta_valid, now);
    }

    // to keep the barometric altitude consistent with geometric altitude, save a derived geom_delta if all data are current
    if (mm->altitude_geom_valid && !mm->geom_delta_valid && a->alt_reliable >= Modes.json_reliable + 1
            && trackDataAge(now, &a->altitude_baro_valid) < 1 * SECONDS
            && accept_data(&a->geom_delta_valid, mm->source, mm, 2)
       ) {
        a->geom_delta = a->altitude_geom - a->altitude_baro;
    }

    // If we've got a new cpr_odd or cpr_even
    if (cpr_new) {
        // this is in addition to the normal air / ground handling
        // making especially sure we catch surface -> airborne transitions
        if (a->last_cpr_type == CPR_SURFACE && mm->cpr_type == CPR_AIRBORNE
                && accept_data(&a->airground_valid, mm->source, mm, 0)) {
            focusGroundstateChange(a, mm, 2, now);
            a->airground = AG_AIRBORNE;
            mm->reduce_forward = 1;

        }
        if (a->last_cpr_type == CPR_AIRBORNE && mm->cpr_type == CPR_SURFACE
                && accept_data(&a->airground_valid, mm->source, mm, 0)) {
            focusGroundstateChange(a, mm, 2, now);
            a->airground = AG_GROUND;
            mm->reduce_forward = 1;
        }

        updatePosition(a, mm, now);
        if (0 && a->addr == Modes.cpr_focus) {
            fprintf(stderr, "%06x: age: odd %"PRIu64" even %"PRIu64"\n",
                    a->addr,
                    trackDataAge(mm->sysTimestampMsg, &a->cpr_odd_valid),
                    trackDataAge(mm->sysTimestampMsg, &a->cpr_even_valid));
        }
    }

    if (mm->sbs_in && mm->sbs_pos_valid) {
        int old_jaero = 0;
        if (mm->source == SOURCE_JAERO && a->trace_len > 0) {
            for (int i = max(0, a->trace_len - 10); i < a->trace_len; i++) {
                if ( (int32_t) (mm->decoded_lat * 1E6) == a->trace[i].lat
                        && (int32_t) (mm->decoded_lon * 1E6) == a->trace[i].lon )
                    old_jaero = 1;
            }
        }
        // avoid using already received positions
        if (old_jaero || greatcircle(a->lat, a->lon, mm->decoded_lat, mm->decoded_lon) < 1) {
        } else if (
                mm->source != SOURCE_PRIO
                && !speed_check(a, mm->source, mm->decoded_lat, mm->decoded_lon, mm, CPR_NONE)
           )
        {
            mm->pos_bad = 1;
            // speed check failed, do nothing
        } else if (accept_data(&a->position_valid, mm->source, mm, 2)) {

            incrementReliable(a, mm, now, 2);

            setPosition(a, mm, now);

            if (a->messages < 2)
                a->messages = 2;

            for (int i = 0; i < 8; ++i) {
                a->signalLevel[i] = 0;
            }
            a->no_signal_count = 10;
        }
    }

    if (mm->msgtype == 11 && mm->IID == 0 && mm->correctedbits == 0) {
        double reflat;
        double reflon;
        struct receiver *r = receiverGetReference(mm->receiverId, &reflat, &reflon, a);
        if (r) {
            a->rr_lat = reflat;
            a->rr_lon = reflon;
            a->rr_seen = now;
            if (Modes.debug_rough_receiver_location
                    && now > a->seenPosReliable + 5 * MINUTES
                    && accept_data(&a->position_valid, SOURCE_MODE_AC, mm, 2)) {
                a->addrtype_updated = now;
                a->addrtype = ADDR_MODE_S;
                mm->decoded_lat = reflat;
                mm->decoded_lon = reflon;
                incrementReliable(a, mm, now, 2);
                set_globe_index(a, globe_index(reflat, reflon));
                setPosition(a, mm, now);
            }
        }
    }

    if (mm->msgtype == 11 && mm->IID == 0 && mm->correctedbits == 0 && now > a->next_reduce_forward_DF11) {

        a->next_reduce_forward_DF11 = now + Modes.net_output_beast_reduce_interval * 4;
        mm->reduce_forward = 1;
    }

    if (mm->cpr_valid)
        a->last_cpr_type = mm->cpr_type;

    if (haveScratch && (mm->garbage || mm->pos_bad || mm->duplicate)) {
        memcpy(a, Modes.scratch, sizeof(struct aircraft));
        if (mm->pos_bad) {
            position_bad(mm, a);
        }
    }

    return (a);
}

//
// Periodic updates of tracking state
//

// Periodically match up mode A/C results with mode S results

static void trackMatchAC(uint64_t now) {
    // clear match flags
    for (unsigned i = 0; i < 4096; ++i) {
        modeAC_match[i] = 0;
    }

    // scan aircraft list, look for matches
    for (int j = 0; j < AIRCRAFT_BUCKETS; j++) {
        for (struct aircraft *a = Modes.aircraft[j]; a; a = a->next) {
            if ((now - a->seen) > 5000) {
                continue;
            }

            // match on Mode A
            if (trackDataValid(&a->squawk_valid)) {
                unsigned i = modeAToIndex(a->squawk);
                if ((modeAC_count[i] - modeAC_lastcount[i]) >= TRACK_MODEAC_MIN_MESSAGES) {
                    a->modeA_hit = 1;
                    modeAC_match[i] = (modeAC_match[i] ? 0xFFFFFFFF : a->addr);
                }
            }

            // match on Mode C (+/- 100ft)
            if (trackDataValid(&a->altitude_baro_valid)) {
                int modeC = (a->altitude_baro + 49) / 100;

                unsigned modeA = modeCToModeA(modeC);
                unsigned i = modeAToIndex(modeA);
                if (modeA && (modeAC_count[i] - modeAC_lastcount[i]) >= TRACK_MODEAC_MIN_MESSAGES) {
                    a->modeC_hit = 1;
                    modeAC_match[i] = (modeAC_match[i] ? 0xFFFFFFFF : a->addr);
                }

                modeA = modeCToModeA(modeC + 1);
                i = modeAToIndex(modeA);
                if (modeA && (modeAC_count[i] - modeAC_lastcount[i]) >= TRACK_MODEAC_MIN_MESSAGES) {
                    a->modeC_hit = 1;
                    modeAC_match[i] = (modeAC_match[i] ? 0xFFFFFFFF : a->addr);
                }

                modeA = modeCToModeA(modeC - 1);
                i = modeAToIndex(modeA);
                if (modeA && (modeAC_count[i] - modeAC_lastcount[i]) >= TRACK_MODEAC_MIN_MESSAGES) {
                    a->modeC_hit = 1;
                    modeAC_match[i] = (modeAC_match[i] ? 0xFFFFFFFF : a->addr);
                }
            }
        }
    }

    // reset counts for next time
    for (unsigned i = 0; i < 4096; ++i) {
        if (!modeAC_count[i])
            continue;

        if ((modeAC_count[i] - modeAC_lastcount[i]) < TRACK_MODEAC_MIN_MESSAGES) {
            if (++modeAC_age[i] > 15) {
                // not heard from for a while, clear it out
                modeAC_lastcount[i] = modeAC_count[i] = modeAC_age[i] = 0;
            }
        } else {
            // this one is live
            // set a high initial age for matches, so they age out rapidly
            // and don't show up on the interactive display when the matching
            // mode S data goes away or changes
            if (modeAC_match[i]) {
                modeAC_age[i] = 10;
            } else {
                modeAC_age[i] = 0;
            }
        }

        modeAC_lastcount[i] = modeAC_count[i];
    }
}

/*
static void updateAircraft() {
    for (int j = 0; j < AIRCRAFT_BUCKETS; j++) {
        for (struct aircraft *a = Modes.aircraft[j]; a; a = a->next) {
        }
    }
}
*/

static void trackRemoveStale(uint64_t now) {
    MODES_NOTUSED(now);
    //fprintf(stderr, "removeStale()\n");
    //fprintf(stderr, "removeStale start: running for %ld ms\n", mstime() - Modes.startup_time);

    for (int thread = 0; thread < STALE_THREADS; thread++) {
        pthread_mutex_lock(&Modes.staleMutex[thread]);
        Modes.staleRun[thread] = 1;
        pthread_cond_signal(&Modes.staleCond[thread]);
        pthread_mutex_unlock(&Modes.staleMutex[thread]);
    }
    for (int thread = 0; thread < STALE_THREADS; thread++) {
        while (Modes.staleRun[thread]) {
            int err = pthread_cond_wait(&Modes.staleDoneCond[thread], &Modes.staleDoneMutex[thread]);
            if (err)
                fprintf(stderr, "trackRemoveStale: pthread_cond unexpected error: %s\n", strerror(err));
        }
    }

    Modes.doFullTraceWrite = 0;

    //fprintf(stderr, "removeStale done: running for %ld ms\n", mstime() - Modes.startup_time);
}
//
//=========================================================================
//
// If we don't receive new nessages within TRACK_AIRCRAFT_TTL
// we remove the aircraft from the list.
//

void trackRemoveStaleThread(int thread, int start, int end, uint64_t now) {
    //fprintf(stderr, "%d %d %d %d\n", thread, start, end, AIRCRAFT_BUCKETS);
    MODES_NOTUSED(thread);

    // non-icao timeout
    uint64_t nonicaoTimeout = now - 1 * HOURS;

    // timeout for aircraft with position
    uint64_t posTimeout = now - 1 * HOURS;
    if (Modes.json_globe_index) {
        posTimeout = now - 26 * HOURS;
        nonicaoTimeout = now - 26 * HOURS;
    }
    if (Modes.state_dir) {
        posTimeout = now - 14 * 24 * HOURS;
    }

    // timeout for aircraft with position
    uint64_t noposTimeout = now - 5 * MINUTES;


    uint64_t doValiditiesCutoff = now - Modes.trackExpireMax;

    for (int j = start; j < end; j++) {
        struct aircraft **nextPointer = &(Modes.aircraft[j]);
        while (*nextPointer) {
            struct aircraft *a = *nextPointer;
            if (
                    (!a->seen_pos && a->seen < noposTimeout)
                    || (a->seen_pos
                        && ((a->seen_pos < posTimeout)
                            || ((a->addr & MODES_NON_ICAO_ADDRESS) && (a->seen_pos < nonicaoTimeout))
                           ))
               ) {
                // Count aircraft where we saw only one message before reaping them.
                // These are likely to be due to messages with bad addresses.
                if (a->messages == 1)
                    Modes.stats_current.single_message_aircraft++;

                if (a->addr == Modes.cpr_focus)
                    fprintf(stderr, "del: %06x seen: %.1f seen_pos: %.1f\n", a->addr, (now - a->seen) / 1000.0, (now - a->seen_pos) / 1000.0);

                // remove from the globeList
                set_globe_index(a, -5);

                // Remove the element from the linked list
                *nextPointer = a->next;

                freeAircraft(a);

            } else {
                if (doValiditiesCutoff < a->seen) {
                    updateValidities(a, now);
                }

                if (Modes.keep_traces && a->trace_alloc) {
                    traceMaintenance(a, now);
                }

                nextPointer = &(a->next);
            }
        }
    }
}

void *staleThreadEntryPoint(void *arg) {
    int thread = * (int *) arg;
    pthread_mutex_lock(&Modes.staleMutex[thread]);
    Modes.staleRun[thread] = 0;
    pthread_cond_signal(&Modes.staleDoneCond[thread]); // tell the main thread we've take the staleMutex lock

    srandom(get_seed());

    int thread_start = thread * STALE_BUCKETS;
    int thread_end = thread_start + STALE_BUCKETS;

    while (!Modes.staleStop) {
        int err = pthread_cond_wait(&Modes.staleCond[thread], &Modes.staleMutex[thread]);
        if (err)
            fprintf(stderr, "staleThread: pthread_cond_wait unexpected error: %s\n", strerror(err));

        if (Modes.staleRun[thread]) {
            uint64_t now = mstime();

            trackRemoveStaleThread(thread, thread_start, thread_end, now);

            if (now > Modes.lastRemoveStale[thread] + 60 * SECONDS && Modes.lastRemoveStale[thread] && !Modes.staleStop) {
                fprintf(stderr, "thread %d: removeStale interval too long: %.1f seconds\n", thread, (now - Modes.lastRemoveStale[thread]) / 1000.0);
            }
            Modes.lastRemoveStale[thread] = now;

            pthread_mutex_lock(&Modes.staleDoneMutex[thread]);
            Modes.staleRun[thread] = 0;
            pthread_cond_signal(&Modes.staleDoneCond[thread]);
            pthread_mutex_unlock(&Modes.staleDoneMutex[thread]);
        }
        //fprintf(stderr, "%d %d %d\n", thread, thread_start, thread_end);
    }
    pthread_mutex_unlock(&Modes.staleMutex[thread]);

    pthread_exit(NULL);
}


static void lockThreads() {
    pthread_mutex_lock(&Modes.jsonMutex);
    for (int i = 0; i < TRACE_THREADS; i++) {
        pthread_mutex_lock(&Modes.jsonTraceMutex[i]);
    }
    pthread_mutex_lock(&Modes.jsonGlobeMutex);
    pthread_mutex_lock(&Modes.miscMutex);
    pthread_mutex_lock(&Modes.decodeMutex);
}

static void unlockThreads() {
    pthread_mutex_unlock(&Modes.decodeMutex);
    pthread_mutex_unlock(&Modes.miscMutex);
    pthread_mutex_unlock(&Modes.jsonGlobeMutex);
    for (int i = 0; i < TRACE_THREADS; i++) {
        pthread_mutex_unlock(&Modes.jsonTraceMutex[i]);
    }
    pthread_mutex_unlock(&Modes.jsonMutex);
}

//
// Entry point for periodic updates
//

void trackPeriodicUpdate() {
    static uint32_t upcount;
    upcount++; // free running counter, first iteration is with 1

    // stop all threads so we can remove aircraft from the list.
    // also serves as memory barrier so json threads get new aircraft in the list
    // adding aircraft does not need to be done with locking:
    // the worst case is that the newly added aircraft is skipped as it's not yet
    // in the cache used by the json threads.
    lockThreads();

    uint64_t now = mstime();

    if (now > Modes.next_stats_update)
        Modes.updateStats = 1;

    struct timespec watch;
    startWatch(&watch);
    struct timespec start_time;
    start_monotonic_timing(&start_time);

    if (!Modes.miscThreadRunning && now > Modes.next_remove_stale) {
        trackRemoveStale(now);
        if (now > Modes.next_remove_stale + 10 * SECONDS && Modes.next_remove_stale) {
            fprintf(stderr, "removeStale delayed by %.1f seconds\n", (now - Modes.next_remove_stale) / 1000.0);
        }
        Modes.next_remove_stale = now + 1 * SECONDS;
    }

    if (Modes.mode_ac && upcount % (1 * SECONDS / PERIODIC_UPDATE) == 2)
        trackMatchAC(now);

    if (upcount % (1 * SECONDS / PERIODIC_UPDATE) == 3)
        checkDisplayStats(now);

    if (upcount % (1 * SECONDS / PERIODIC_UPDATE) == 4)
        netFreeClients();

    if (Modes.updateStats)
        statsUpdate(now); // needs to happen under lock

    int nParts = 5 * MINUTES / PERIODIC_UPDATE;
    receiverTimeout((upcount % nParts), nParts, now);

    end_monotonic_timing(&start_time, &Modes.stats_current.remove_stale_cpu);
    int64_t elapsed = stopWatch(&watch);

    unlockThreads();

    static uint64_t antiSpam;
    if (elapsed > 50 && now > antiSpam + 30 * SECONDS) {
        fprintf(stderr, "<3>High load: removeStale took %"PRIu64" ms! Suppressing for 30 seconds\n", elapsed);
        antiSpam = now;
    }

    //fprintf(stderr, "running for %ld ms\n", mstime() - Modes.startup_time);
    //fprintf(stderr, "removeStale took %"PRIu64" ms, running for %ld ms\n", elapsed, now - Modes.startup_time);

    if (Modes.updateStats) {
        statsResetCount();
        uint32_t aircraftCount = 0;
        for (int j = 0; j < AIRCRAFT_BUCKETS; j++) {
            for (struct aircraft *a = Modes.aircraft[j]; a; a = a->next) {
                aircraftCount++;
                if (Modes.updateStats && a->messages >= 2 && (now < a->seen + TRACK_EXPIRE || trackDataValid(&a->position_valid)))
                    statsCountAircraft(a);
            }
        }

        statsWrite();
        Modes.updateStats = 0;

        Modes.aircraftCount = aircraftCount;

        static uint64_t antiSpam2;
        if (Modes.aircraftCount > 2 * AIRCRAFT_BUCKETS && now > antiSpam2 + 12 * HOURS) {
            fprintf(stderr, "<3>increase AIRCRAFT_HASH_BITS, aircraft hash table fill: %0.1f\n", Modes.aircraftCount / (double) AIRCRAFT_BUCKETS);
            antiSpam2 = now;
        }
    }
}

void miscStuff() {
    uint64_t now = mstime();

    struct timespec watch;
    startWatch(&watch);

    struct timespec start_time;
    start_cpu_timing(&start_time);

    checkNewDay(now);

    // don't do everything at once ... this stuff isn't that time critical it'll get its turn
    int enough = 0;

    if (handleHeatmap(now)) {
        enough = 1;
    }

    static uint32_t blob; // current blob
    static uint64_t next_blob;
    if (Modes.state_dir && !enough && now > next_blob) {
        enough = 1;
        save_blob(blob++ % STATE_BLOBS);
        next_blob = now + 60 * MINUTES / STATE_BLOBS;
    }

    if (!enough && Modes.api && now > Modes.next_api_update) {
        // this will probably get its own thread at some point
        enough = 1;
        Modes.next_api_update = now + 1 * SECONDS;
        apiClear();
        for (int j = 0; j < AIRCRAFT_BUCKETS; j++) {
            for (struct aircraft *a = Modes.aircraft[j]; a; a = a->next) {
                apiAdd(a);
            }
        }
        apiSort();
    }

    static uint64_t next_clients_json;
    if (!enough && Modes.json_dir && now > next_clients_json) {
        enough = 1;
        next_clients_json = now + 10 * SECONDS;
        if (Modes.netIngest)
            writeJsonToFile(Modes.json_dir, "clients.json", generateClientsJson());
        if (Modes.netReceiverIdJson)
            writeJsonToFile(Modes.json_dir, "receivers.json", generateReceiversJson());
    }

    if (!enough) {
        // one iteration later, finish db update if db was updated
        if (dbFinishUpdate())
            enough = 1;
    }
    static uint64_t next_db_check;
    if (!enough && now > next_db_check) {
        enough = 1;
        dbUpdate();
        // db update check every 5 min
        next_db_check = now + 5 * MINUTES;
    }

    end_cpu_timing(&start_time, &Modes.stats_current.heatmap_and_state_cpu);

    uint64_t elapsed = stopWatch(&watch);
    static uint64_t antiSpam2;
    if (elapsed > 2 * SECONDS && now > antiSpam2 + 30 * SECONDS) {
        fprintf(stderr, "<3>High load: heatmap_and_stuff took %"PRIu64" ms! Suppressing for 30 seconds\n", elapsed);
        antiSpam2 = now;
    }
}


void *miscThreadEntryPoint(void *arg) {
    MODES_NOTUSED(arg);

    pthread_mutex_lock(&Modes.miscMutex);

    srandom(get_seed());
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);

    while (!Modes.exit) {

        if (mstime() < Modes.next_remove_stale) {

            Modes.miscThreadRunning = 1;

            pthread_mutex_unlock(&Modes.miscMutex);
            miscStuff();
            pthread_mutex_lock(&Modes.miscMutex);

            Modes.miscThreadRunning = 0;
        }


        incTimedwait(&ts, 250); // check every quarter second if there is something to do

        int err = pthread_cond_timedwait(&Modes.miscCond, &Modes.miscMutex, &ts);
        if (err && err != ETIMEDOUT)
            fprintf(stderr, "main thread: pthread_cond_timedwait unexpected error: %s\n", strerror(err));
    }

    pthread_mutex_unlock(&Modes.miscMutex);

    pthread_exit(NULL);
}

/*
static void adjustExpire(struct aircraft *a, uint64_t timeout) {
#define F(f,s,e) do { a->f##_valid.stale_interval = (s) * 1000; a->f##_valid.expire_interval = (e) * 1000; } while (0)
    F(callsign, 60,  timeout); // ADS-B or Comm-B
    F(altitude_baro, 15,  timeout); // ADS-B or Mode S
    F(altitude_geom, 30, timeout); // ADS-B only
    F(geom_delta, 30, timeout); // ADS-B only
    F(gs, 30,  timeout); // ADS-B or Comm-B
    F(ias, 30, timeout); // ADS-B (rare) or Comm-B
    F(tas, 30, timeout); // ADS-B (rare) or Comm-B
    F(mach, 30, timeout); // Comm-B only
    F(track, 30,  timeout); // ADS-B or Comm-B
    F(track_rate, 30, timeout); // Comm-B only
    F(roll, 30, timeout); // Comm-B only
    F(mag_heading, 30, timeout); // ADS-B (rare) or Comm-B
    F(true_heading, 30, timeout); // ADS-B only (rare)
    F(baro_rate, 30, timeout); // ADS-B or Comm-B
    F(geom_rate, 30, timeout); // ADS-B or Comm-B
    F(squawk, 15, timeout); // ADS-B or Mode S
    F(airground, 15, timeout); // ADS-B or Mode S
    F(nav_qnh, 30, timeout); // Comm-B only
    F(nav_altitude_mcp, 30, timeout);  // ADS-B or Comm-B
    F(nav_altitude_fms, 30, timeout);  // ADS-B or Comm-B
    F(nav_altitude_src, 30, timeout); // ADS-B or Comm-B
    F(nav_heading, 30, timeout); // ADS-B or Comm-B
    F(nav_modes, 30, timeout); // ADS-B or Comm-B
    F(cpr_odd, 10, timeout); // ADS-B only
    F(cpr_even, 10, timeout); // ADS-B only
    F(position, 10,  timeout); // ADS-B only
    F(nic_a, 30, timeout); // ADS-B only
    F(nic_c, 30, timeout); // ADS-B only
    F(nic_baro, 30, timeout); // ADS-B only
    F(nac_p, 30, timeout); // ADS-B only
    F(nac_v, 30, timeout); // ADS-B only
    F(sil, 30, timeout); // ADS-B only
    F(gva, 30, timeout); // ADS-B only
    F(sda, 30, timeout); // ADS-B only
#undef F
}
*/

static void position_bad(struct modesMessage *mm, struct aircraft *a) {
    if (mm->garbage)
        return;
    if (mm->pos_ignore)
        return;
    if (mm->source < a->position_valid.source)
        return;


    Modes.stats_current.cpr_global_bad++;


    if (a->addr == Modes.cpr_focus)
        fprintf(stderr, "%06x: position_bad\n", a->addr);

    a->pos_reliable_odd--;
    a->pos_reliable_even--;

    if (a->pos_reliable_odd <= 0 || a->pos_reliable_even <=0) {
        a->position_valid.source = SOURCE_INVALID;
        a->pos_reliable_odd = 0;
        a->pos_reliable_even = 0;
        a->cpr_odd_valid.source = SOURCE_INVALID;
        a->cpr_even_valid.source = SOURCE_INVALID;
    }
}

void to_state_all(struct aircraft *a, struct state_all *new, uint64_t now) {
            for (int i = 0; i < 8; i++)
                new->callsign[i] = a->callsign[i];

            new->pos_nic = a->pos_nic;
            new->pos_rc = a->pos_rc;

            new->altitude_geom = (int16_t) nearbyint(a->altitude_geom / 25.0);
            new->baro_rate = (int16_t) nearbyint(a->baro_rate / 8.0);
            new->geom_rate = (int16_t) nearbyint(a->geom_rate / 8.0);
            new->ias = a->ias;
            new->tas = a->tas;

            new->squawk = a->squawk;
            new->category = a->category; // Aircraft category A0 - D7 encoded as a single hex byte. 00 = unset
            new->nav_altitude_mcp = (uint16_t) nearbyint(a->nav_altitude_mcp / 4.0);
            new->nav_altitude_fms = (uint16_t) nearbyint(a->nav_altitude_fms / 4.0);

            new->nav_qnh = (int16_t) nearbyint(a->nav_qnh * 10.0);
            new->gs = (int16_t) nearbyint(a->gs * 10.0);
            new->mach = (int16_t) nearbyint(a->mach * 1000.0);

            new->track_rate = (int16_t) nearbyint(a->track_rate * 100.0);
            new->roll = (int16_t) nearbyint(a->roll * 100.0);

            new->track = (int16_t) nearbyint(a->track * 90.0);
            new->mag_heading = (int16_t) nearbyint(a->mag_heading * 90.0);
            new->true_heading = (int16_t) nearbyint(a->true_heading * 90.0);
            new->nav_heading = (int16_t) nearbyint(a->nav_heading * 90.0);

            new->emergency = a->emergency;
            new->airground = a->airground;
            new->addrtype = a->addrtype;
            new->nav_modes = a->nav_modes;
            new->nav_altitude_src = a->nav_altitude_src;
            new->sil_type = a->sil_type;

            if (now < a->wind_updated + TRACK_EXPIRE && abs(a->wind_altitude - a->altitude_baro) < 500) {
                new->wind_direction = (int) nearbyint(a->wind_direction);
                new->wind_speed = (int) nearbyint(a->wind_speed);
                new->wind_valid = 1;
            }
            if (now < a->oat_updated + TRACK_EXPIRE) {
                new->oat = (int) nearbyint(a->oat);
                new->tat = (int) nearbyint(a->tat);
                new->temp_valid = 1;
            }

            if (a->adsb_version < 0)
                new->adsb_version = 15;
            else
                new->adsb_version = a->adsb_version;

            if (a->adsr_version < 0)
                new->adsr_version = 15;
            else
                new->adsr_version = a->adsr_version;

            if (a->tisb_version < 0)
                new->tisb_version = 15;
            else
                new->tisb_version = a->tisb_version;

            new->nic_a = a->nic_a;
            new->nic_c = a->nic_c;
            new->nic_baro = a->nic_baro;
            new->nac_p = a->nac_p;
            new->nac_v = a->nac_v;
            new->sil = a->sil;
            new->gva = a->gva;
            new->sda = a->sda;
            new->alert = a->alert;
            new->spi = a->spi;

#define F(f) do { new->f = trackVState(now, &a->f, &a->position_valid); } while (0)
           F(callsign_valid);
           F(altitude_baro_valid);
           F(altitude_geom_valid);
           F(geom_delta_valid);
           F(gs_valid);
           F(ias_valid);
           F(tas_valid);
           F(mach_valid);
           F(track_valid);
           F(track_rate_valid);
           F(roll_valid);
           F(mag_heading_valid);
           F(true_heading_valid);
           F(baro_rate_valid);
           F(geom_rate_valid);
           F(nic_a_valid);
           F(nic_c_valid);
           F(nic_baro_valid);
           F(nac_p_valid);
           F(nac_v_valid);
           F(sil_valid);
           F(gva_valid);
           F(sda_valid);
           F(squawk_valid);
           F(emergency_valid);
           F(airground_valid);
           F(nav_qnh_valid);
           F(nav_altitude_mcp_valid);
           F(nav_altitude_fms_valid);
           F(nav_altitude_src_valid);
           F(nav_heading_valid);
           F(nav_modes_valid);
           F(position_valid);
           F(alert_valid);
           F(spi_valid);
#undef F
}
static void calc_wind(struct aircraft *a, uint64_t now) {
    uint32_t focus = 0xc0ffeeba;

    if (a->addr == focus)
        fprintf(stderr, "%"PRIu64" %"PRIu64" %"PRIu64" %"PRIu64"\n", trackDataAge(now, &a->tas_valid), trackDataAge(now, &a->true_heading_valid),
                trackDataAge(now, &a->gs_valid), trackDataAge(now, &a->track_valid));

    if (!trackDataValid(&a->position_valid) || a->airground == AG_GROUND)
        return;

    if (trackDataAge(now, &a->tas_valid) > TRACK_WT_TIMEOUT
            || trackDataAge(now, &a->gs_valid) > TRACK_WT_TIMEOUT
            || trackDataAge(now, &a->track_valid) > TRACK_WT_TIMEOUT / 2
            || trackDataAge(now, &a->true_heading_valid) > TRACK_WT_TIMEOUT / 2
       ) {
        return;
    }

    // don't use this code for now
    /*
    if (a->trace && a->trace_len >= 2) {
        struct state *last = &(a->trace[a->trace_len-1]);
        if (now + 1500 < last->timestamp)
            last = &(a->trace[a->trace_len-2]);
        float track_diff = fabs(a->track - last->track / 10.0);
        if (last->flags.track_valid && track_diff > 0.5)
            return;
    }
    */

    double trk = (M_PI / 180) * a->track;
    double hdg = (M_PI / 180) * a->true_heading;
    double tas = a->tas;
    double gs = a->gs;
    double crab = norm_diff(hdg - trk, M_PI);

    double hw = tas - cos(crab) * gs;
    double cw = sin(crab) * gs;
    double ws = sqrt(hw * hw + cw * cw);
    double wd = hdg + atan2(cw, hw);

    wd = norm_angle(wd, M_PI);

    wd *= (180 / M_PI);
    crab *= (180 / M_PI);

    //if (a->addr == focus)
    //fprintf(stderr, "%06x: %.1f %.1f %.1f %.1f %.1f\n", a->addr, ws, wd, gs, tas, crab);
    if (ws > 250) {
        // Filter out wildly unrealistic wind speeds
        return;
    }
    a->wind_speed = ws;
    a->wind_direction = wd;
    a->wind_updated = now;
    a->wind_altitude = a->altitude_baro;
}
static void calc_temp(struct aircraft *a, uint64_t now) {
    if (a->airground == AG_GROUND)
        return;
    if (trackDataAge(now, &a->tas_valid) > TRACK_WT_TIMEOUT || trackDataAge(now, &a->mach_valid) > TRACK_WT_TIMEOUT)
        return;

    if (a->mach < 0.395)
        return;

    double fraction = a->tas / 661.47 / a->mach;
    double oat = (fraction * fraction * 288.15) - 273.15;
    double tat = -273.15 + ((oat + 273.15) * (1 + 0.2 * a->mach * a->mach));

    a->oat = oat;
    a->tat = tat;
    a->oat_updated = now;
}

static inline int declination (struct aircraft *a, double *dec) {
    double year;
    time_t now_t = a->seen/1000;

    struct tm utc;
    gmtime_r(&now_t, &utc);

    year = 1900.0 + utc.tm_year + utc.tm_yday / 365.0;

    double dip;
    double ti;
    double gv;

    int res = geomag_calc(a->altitude_baro * 0.0003048, a->lat, a->lon, year, dec, &dip, &ti, &gv);
    if (res)
        *dec = 0.0;
    return res;
}

void from_state_all(struct state_all *in, struct aircraft *a , uint64_t ts) {
            for (int i = 0; i < 8; i++)
                a->callsign[i] = in->callsign[i];
            a->callsign[8] = '\0';

            a->pos_nic = in->pos_nic;
            a->pos_rc = in->pos_rc;

            a->altitude_geom = in->altitude_geom * 25;
            a->baro_rate = in->baro_rate * 8;
            a->geom_rate = in->geom_rate * 8;
            a->ias = in->ias;
            a->tas = in->tas;

            a->squawk = in->squawk;
            a->category =  in->category; // Aircraft category A0 - D7 encoded as a single hex byte. 00 = unset
            a->nav_altitude_mcp = in->nav_altitude_mcp * 4;
            a->nav_altitude_fms = in->nav_altitude_fms * 4;

            a->nav_qnh = in->nav_qnh / 10.0;
            a->gs = in->gs / 10.0;
            a->mach = in->mach / 1000.0;

            a->track_rate = in->track_rate / 100.0;
            a->roll = in->roll / 100.0;

            a->track = in->track / 90.0;
            a->mag_heading = in->mag_heading / 90.0;
            a->true_heading = in->true_heading / 90.0;
            a->nav_heading = in->nav_heading / 90.0;

            a->emergency = in->emergency;
            a->airground = in->airground;
            a->addrtype = in->addrtype;
            a->nav_modes = in->nav_modes;
            a->nav_altitude_src = in->nav_altitude_src;
            a->sil_type = in->sil_type;

            if (in->wind_valid) {
                a->wind_direction = in->wind_direction;
                a->wind_speed = in->wind_speed;
                a->wind_updated = ts - 5000;
                a->wind_altitude = a->altitude_baro;
            }
            if (in->temp_valid) {
                a->oat = in->oat;
                a->tat = in->tat;
                a->oat_updated = ts - 5000;
            }

            if (in->adsb_version == 15)
                a->adsb_version = -1;
            else
                a->adsb_version = in->adsb_version;

            if (in->adsr_version == 15)
                a->adsr_version = -1;
            else
                a->adsr_version = in->adsr_version;

            if (in->tisb_version == 15)
                a->tisb_version = -1;
            else
                a->tisb_version = in->tisb_version;

            a->nic_a = in->nic_a;
            a->nic_c = in->nic_c;
            a->nic_baro = in->nic_baro;
            a->nac_p = in->nac_p;
            a->nac_v = in->nac_v;
            a->sil = in->sil;
            a->gva = in->gva;
            a->sda = in->sda;
            a->alert = in->alert;
            a->spi = in->spi;


            // giving this a timestamp is kinda hacky, do it anyway
            // we want to be able to reuse the sprintAircraft routine for printing aircraft details
#define F(f) do { a->f.source = (in->f ? SOURCE_INDIRECT : SOURCE_INVALID); a->f.updated = ts - 5000; } while (0)
           F(callsign_valid);
           F(altitude_baro_valid);
           F(altitude_geom_valid);
           F(geom_delta_valid);
           F(gs_valid);
           F(ias_valid);
           F(tas_valid);
           F(mach_valid);
           F(track_valid);
           F(track_rate_valid);
           F(roll_valid);
           F(mag_heading_valid);
           F(true_heading_valid);
           F(baro_rate_valid);
           F(geom_rate_valid);
           F(nic_a_valid);
           F(nic_c_valid);
           F(nic_baro_valid);
           F(nac_p_valid);
           F(nac_v_valid);
           F(sil_valid);
           F(gva_valid);
           F(sda_valid);
           F(squawk_valid);
           F(emergency_valid);
           F(airground_valid);
           F(nav_qnh_valid);
           F(nav_altitude_mcp_valid);
           F(nav_altitude_fms_valid);
           F(nav_altitude_src_valid);
           F(nav_heading_valid);
           F(nav_modes_valid);
           F(position_valid);
           F(alert_valid);
           F(spi_valid);
#undef F
}

static const char *source_string(datasource_t source) {
    switch (source) {
        case SOURCE_INVALID:
            return "INVALID";
        case SOURCE_INDIRECT:
            return "INDIRECT";
        case SOURCE_MODE_AC:
            return "MODE_AC";
        case SOURCE_SBS:
            return "SBS";
        case SOURCE_MLAT:
            return "MLAT";
        case SOURCE_MODE_S:
            return "MODE_S";
        case SOURCE_JAERO:
            return "JAERO";
        case SOURCE_MODE_S_CHECKED:
            return "MODE_CH";
        case SOURCE_TISB:
            return "TISB";
        case SOURCE_ADSR:
            return "ADSR";
        case SOURCE_ADSB:
            return "ADSB";
        case SOURCE_PRIO:
            return "PRIO";
        default:
            return "UNKN";
    }
}
void updateValidities(struct aircraft *a, uint64_t now) {
    a->receiverIds[a->receiverIdsNext++ % RECEIVERIDBUFFER] = 0;

    if (a->globe_index >= 0 && now > a->seen_pos + Modes.trackExpireJaero + 1 * MINUTES) {
        set_globe_index(a, -5);
    }

    if (now > a->category_updated + 2 * HOURS)
        a->category = 0;

    updateValidity(&a->callsign_valid, now, TRACK_EXPIRE_LONG);
    updateValidity(&a->squawk_valid, now, TRACK_EXPIRE_LONG);
    updateValidity(&a->airground_valid, now, TRACK_EXPIRE_LONG);
    updateValidity(&a->altitude_baro_valid, now, TRACK_EXPIRE);
    updateValidity(&a->altitude_geom_valid, now, TRACK_EXPIRE);
    updateValidity(&a->geom_delta_valid, now, TRACK_EXPIRE);
    updateValidity(&a->gs_valid, now, TRACK_EXPIRE);
    updateValidity(&a->ias_valid, now, TRACK_EXPIRE);
    updateValidity(&a->tas_valid, now, TRACK_EXPIRE);
    updateValidity(&a->mach_valid, now, TRACK_EXPIRE);
    updateValidity(&a->track_valid, now, TRACK_EXPIRE);
    updateValidity(&a->track_rate_valid, now, TRACK_EXPIRE);
    updateValidity(&a->roll_valid, now, TRACK_EXPIRE);
    updateValidity(&a->mag_heading_valid, now, TRACK_EXPIRE);
    updateValidity(&a->true_heading_valid, now, TRACK_EXPIRE);
    updateValidity(&a->baro_rate_valid, now, TRACK_EXPIRE);
    updateValidity(&a->geom_rate_valid, now, TRACK_EXPIRE);
    updateValidity(&a->nav_qnh_valid, now, TRACK_EXPIRE);
    updateValidity(&a->nav_altitude_mcp_valid, now, TRACK_EXPIRE);
    updateValidity(&a->nav_altitude_fms_valid, now, TRACK_EXPIRE);
    updateValidity(&a->nav_altitude_src_valid, now, TRACK_EXPIRE);
    updateValidity(&a->nav_heading_valid, now, TRACK_EXPIRE);
    updateValidity(&a->nav_modes_valid, now, TRACK_EXPIRE);

    updateValidity(&a->cpr_odd_valid, now, TRACK_EXPIRE);
    updateValidity(&a->cpr_even_valid, now, TRACK_EXPIRE);
    updateValidity(&a->position_valid, now, TRACK_EXPIRE);
    updateValidity(&a->nic_a_valid, now, TRACK_EXPIRE);
    updateValidity(&a->nic_c_valid, now, TRACK_EXPIRE);
    updateValidity(&a->nic_baro_valid, now, TRACK_EXPIRE);
    updateValidity(&a->nac_p_valid, now, TRACK_EXPIRE);
    updateValidity(&a->sil_valid, now, TRACK_EXPIRE);
    updateValidity(&a->gva_valid, now, TRACK_EXPIRE);
    updateValidity(&a->sda_valid, now, TRACK_EXPIRE);

    // reset position reliability when no position was received for 2 minutes
    if (trackDataAge(now, &a->position_valid) > 2 * MINUTES || now > a->seenPosGlobal + 10 * MINUTES) {
        a->pos_reliable_odd = 0;
        a->pos_reliable_even = 0;
    }
    if (now > a->seenPosReliable + TRACE_STALE) {
        traceUsePosBuffered(a);
    }

    if (a->altitude_baro_valid.source == SOURCE_INVALID)
        a->alt_reliable = 0;
}

static void showPositionDebug(struct aircraft *a, struct modesMessage *mm, uint64_t now) {

    fprintf(stderr, "%06x: ", a->addr);
    fprintf(stderr, "elapsed: %0.1f ", (now - a->seen_pos) / 1000.0);

    if (mm->sbs_in) {
        fprintf(stderr, "SBS, ");
        if (mm->source == SOURCE_JAERO)
            fprintf(stderr, "JAERO, ");
        if (mm->source == SOURCE_MLAT)
            fprintf(stderr, "MLAT, ");
    } else {
        fprintf(stderr, "%s%s",
                (mm->cpr_type == CPR_SURFACE) ? "surf, " : "air,  ",
                mm->cpr_odd ? "odd,  " : "even, ");
    }

    if (mm->sbs_in) {
        fprintf(stderr,
                "lat: %.6f,"
                "lon: %.6f",
                mm->decoded_lat,
                mm->decoded_lon);
    } else if (mm->cpr_decoded) {
        fprintf(stderr,"lat: %.6f (%u),"
                " lon: %.6f (%u),"
                " relative: %d,"
                " NIC: %u,"
                " Rc: %.3f km",
                mm->decoded_lat,
                mm->cpr_lat,
                mm->decoded_lon,
                mm->cpr_lon,
                mm->cpr_relative,
                mm->decoded_nic,
                mm->decoded_rc / 1000.0);
    } else {
        fprintf(stderr,"lat: (%u),"
                " lon: (%u),"
                " CPR decoding: none",
                mm->cpr_lat,
                mm->cpr_lon);
    }
    fprintf(stderr, "\n");
}

static void incrementReliable(struct aircraft *a, struct modesMessage *mm, uint64_t now, int odd) {
    a->seenPosGlobal = now;

    if (mm->source > SOURCE_JAERO && now > a->seenPosReliable + 2 * MINUTES
            && a->pos_reliable_odd <= 0 && a->pos_reliable_even <= 0) {
        double distance = greatcircle(a->latReliable, a->lonReliable, mm->decoded_lat, mm->decoded_lon);
        // if aircraft is close to last reliable position, treat new position as reliable immediately.
        // based on 2 minutes, 12 km equals 360 km/h or 194 knots
        if (distance < 12e3) {
            a->pos_reliable_odd = max(1, Modes.json_reliable);
            a->pos_reliable_even = max(1, Modes.json_reliable);
            if (a->addr == Modes.cpr_focus)
                fprintf(stderr, "%06x: fast track json_reliable\n", a->addr);
            return;
        }
    }

    if (a->pos_reliable_odd <= 0 || a->pos_reliable_even <= 0) {
        a->pos_reliable_odd = 1;
        a->pos_reliable_even = 1;
        return;
    }

    if (odd)
        a->pos_reliable_odd = min(a->pos_reliable_odd + 1, Modes.filter_persistence);

    if (!odd || odd == 2)
        a->pos_reliable_even = min(a->pos_reliable_even + 1, Modes.filter_persistence);
}
