# JSON output formats

readsb generates several json files with informaton about the receiver itself, currently known aircraft,
and general statistics. These are used by the webmap, but could also be used by other things feeds stats
about readsb operation to collectd for later graphing.

## Reading the json files

There are two ways to obtain the json files:

 * By HTTP from the external webserver that readsb is feeding. The json is served from the data/ path, e.g. http://somehost/readsb/data/aircraft.json
 * As a file in the directory specified by --write-json on readsb command line.

The HTTP versions are always up to date.
The file versions are written periodically; for aircraft, typically once a second, for stats, once a minute.
The file versions are updated to a temporary file, then atomically renamed to the right path, so you should never see partial copies.

Each file contains a single JSON object. The file formats are:

## receiver.json

This file has general metadata about readsb. It does not change often and you probably just want to read it once at startup.
The keys are:

 * version: the version of readsb in use
 * refresh: how often aircraft.json is updated (for the file version), in milliseconds. the webmap uses this to control its refresh interval.
 * history: the current number of valid history files (see below)
 * lat: the latitude of the receiver in decimal degrees. Optional, may not be present.
 * lon: the longitude of the receiver in decimal degrees. Optional, may not be present.

## aircraft.json

This file contains readsb list of recently seen aircraft. The keys are:

 * now: the time this file was generated, in seconds since Jan 1 1970 00:00:00 GMT (the Unix epoch).
 * messages: the total number of Mode S messages processed since readsb started.
 * aircraft: an array of JSON objects, one per known aircraft. Each aircraft has the following keys. Keys will be omitted if data is not available.
   * hex: the 24-bit ICAO identifier of the aircraft, as 6 hex digits. The identifier may start with '~', this means that the address is a non-ICAO address (e.g. from TIS-B).
   * type: type of underlying messages / best source of current data for this position / aircraft:
            (the following list is in order of which data is preferentially used)
     * adsb_icao: messages from a Mode S or ADS-B transponder, using a 24-bit ICAO address
     * adsb_icao_nt: messages from an ADS-B equipped "non-transponder" emitter e.g. a ground vehicle, using a 24-bit ICAO address
     * adsr_icao: rebroadcast of ADS-B messages originally sent via another data link e.g. UAT, using a 24-bit ICAO address
     * tisb_icao: traffic information about a non-ADS-B target identified by a 24-bit ICAO address, e.g. a Mode S target tracked by secondary radar

     * adsc: ADS-C (received by monitoring satellite downlinks)
     * mlat: MLAT, position calculated arrival time differences using multiple receivers, outliers and varying accuracy is expected.
     * other: miscellaneous data received via Basestation / SBS format, quality / source is unknown.
     * mode_s: ModeS data from the planes transponder (no position transmitted)

     * adsb_other: messages from an ADS-B transponder using a non-ICAO address, e.g. anonymized address
     * adsr_other: rebroadcast of ADS-B messages originally sent via another data link e.g. UAT, using a non-ICAO address
     * tisb_other: traffic information about a non-ADS-B target using a non-ICAO address
     * tisb_trackfile: traffic information about a non-ADS-B target using a track/file identifier, typically from primary or Mode A/C radar
   * flight: callsign, the flight name or aircraft registration as 8 chars (2.2.8.2.6)
   * alt_baro: the aircraft barometric altitude in feet
   * alt_geom: geometric (GNSS / INS) altitude in feet referenced to the WGS84 ellipsoid
   * gs: ground speed in knots
   * ias: indicated air speed in knots
   * tas: true air speed in knots
   * mach: Mach number
   * track: true track over ground in degrees (0-359)
   * track_rate: Rate of change of track, degrees/second
   * roll: Roll, degrees, negative is left roll
   * mag_heading: Heading, degrees clockwise from magnetic north
   * true_heading: Heading, degrees clockwise from true north (usually only transmitted on ground, in the air usually derived from the magnetic heading using magnetic model WMM2020)
   * baro_rate: Rate of change of barometric altitude, feet/minute
   * geom_rate: Rate of change of geometric (GNSS / INS) altitude, feet/minute
   * squawk: Mode A code (Squawk), encoded as 4 octal digits
   * emergency: ADS-B emergency/priority status, a superset of the 7x00 squawks (2.2.3.2.7.8.1.1)
   * category: emitter category to identify particular aircraft or vehicle classes (values A0 - D7) (2.2.3.2.5.2)
   * nav_qnh: altimeter setting (QFE or QNH/QNE), hPa
   * nav_altitude_mcp: selected altitude from the Mode Control Panel / Flight Control Unit (MCP/FCU) or equivalent equipment
   * nav_altitude_fms: selected altitude from the Flight Manaagement System (FMS) (2.2.3.2.7.1.3.3)
   * nav_heading: selected heading (True or Magnetic is not defined in DO-260B, mostly Magnetic as that is the de facto standard) (2.2.3.2.7.1.3.7)
   * nav_modes: set of engaged automation modes: 'autopilot', 'vnav', 'althold', 'approach', 'lnav', 'tcas'
   * lat, lon: the aircraft position in decimal degrees
   * nic: Navigation Integrity Category (2.2.3.2.7.2.6)
   * rc: Radius of Containment, meters; a measure of position integrity derived from NIC & supplementary bits. (2.2.3.2.7.2.6, Table 2-69)
   * seen_pos: how long ago (in seconds before "now") the position was last updated
   * track: true track over ground in degrees (0-359)
   * version: ADS-B Version Number 0, 1, 2 (3-7 are reserved) (2.2.3.2.7.5)
   * nic_baro: Navigation Integrity Category for Barometric Altitude (2.2.5.1.35)
   * nac_p: Navigation Accuracy for Position (2.2.5.1.35)
   * nac_v: Navigation Accuracy for Velocity (2.2.5.1.19)
   * sil: Source Integity Level (2.2.5.1.40)
   * sil_type: interpretation of SIL: unknown, perhour, persample
   * gva: Geometric Vertical Accuracy  (2.2.3.2.7.2.8)
   * sda: System Design Assurance (2.2.3.2.7.2.4.6)
   * mlat: list of fields derived from MLAT data
   * tisb: list of fields derived from TIS-B data
   * messages: total number of Mode S messages received from this aircraft
   * seen: how long ago (in seconds before "now") a message was last received from this aircraft
   * rssi: recent average RSSI (signal power), in dbFS; this will always be negative.
   * alert: Flight status alert bit (2.2.3.2.3.2)
   * spi: Flight status special position identification bit (2.2.3.2.3.2)
   * wd, ws: wind direction and wind speed are calculated from ground track, true heading, true airspeed and ground speed
   * oat, tat: outer/static air temperature and total air temperature are calculated from mach number and true airspeed (typically somewhat inaccurate at lower altitudes / mach numbers below 0.5, calculation is inhibited for mach < 0.395)

Section references (2.2.xyz) refer to DO-260B.

## history_0.json, history_1.json, ..., history_119.json

These files are historical copies of aircraft.json at (by default) 30 second intervals. They follow exactly the
same format as aircraft.json. To know how many are valid, see receiver.json ("history" value). They are written in
a cycle, with history_0 being overwritten after history_119 is generated, so history_0.json is not necessarily the
oldest history entry. To load history, you should:

 * read "history" from receiver.json.
 * load that many history_N.json files
 * sort the resulting files by their "now" values
 * process the files in order

## trace jsons

 * overall structure
```
{
    icao: "0123ac", // hex id of the aircraft
    timestamp: 1609275898.495, // unix timestamp in seconds since epoch (1970)
    trace: [
        [ seconds after timestamp,
            lat,
            lon,
            altitude in ft or "ground" or null,
            ground speed in knots or null,
            track in degrees or null, (if altitude == "ground", this will be true heading instead of track)
            flags as a bitfield: (use bitwise and to extract data)
                (flags & 1 > 0): position is stale (no position received for 20 seconds before this one)
                (flags & 2 > 0): start of a new leg (tries to detect a separation point between landing and takeoff that separates fligths)
                (flags & 4 > 0): vertical rate is geometric and not barometric
                (flags & 8 > 0): altitude is geometric and not barometric
             ,
            vertical rate in fpm or null,
            aircraft object with extra details or null (see aircraft.json documentation, note that not all fields are present as lat and lon for example arlready in the values above)
        ],
        [next entry like the one before],
        [next entry like the one before],
    ]
}
```

Example :
```json
{"icao":"0d8300",
    "r":"YV3382",
    "t":"LJ31",
    "desc":"Bombardier Learjet 31 A",
    "timestamp": 1609275898.495,
    "trace":[
        [0.0,30.404617,-86.476566,-300,0.7,0.0,1,0,
    {"type":"adsb_icao","flight":"YV3382  ","alt_geom":-75,"track":0.00,"baro_rate":0,"squawk":"1604","emergency":"none","category":"A1","nic":8,"rc":186,"version":2,"nic_baro":1,"nac_p":10,"nac_v":2,"sil":3,"sil_type":"perhour","gva":2,"sda":2,"alert":0,"spi":0}],
    [95.1,30.404617,-86.476575,-300,0.7,0.0,0,0,null],
    [136.3,30.404617,-86.476575,-300,0.7,0.0,0,0,null],
    [162.3,30.404846,-86.476782,-300,0.7,0.0,1,0,null],
    [254.4,30.375614,-86.448889,525,37.8,142.5,1,64,
    {"type":"adsb_icao","alt_geom":675,"track":142.52,"baro_rate":64,"category":"A1","nic":8,"rc":186,"version":2,"nac_v":2,"sil_type":"perhour","alert":0,"spi":1}],
    [303.2,30.347078,-86.418896,1275,171.9,136.7,1,448,null],
    [327.0,30.332932,-86.403647,1900,171.9,137.1,1,1664,null]
    ]
}
```

## stats.json

This file contains statistics about readsb operations.

There are 5 top level keys: "latest", "last1min", "last5min", "last15min", "total". Each key has statistics for a different period, defined by the "start" and "end" subkeys:

 * "total" covers the entire period from when readsb was started up to the current time
 * "last1min" covers a recent 1-minute period. This may be up to 1 minute out of date (i.e. "end" may be up to 1 minute old).
 * "last5min" covers a recent 5-minute period. As above, this may be up to 1 minute out of date.
 * "last15min" covers a recent 15-minute period. As above, this may be up to 1 minute out of date.
 * "latest" covers the time between the end of the "last1min" period and the current time.

Internally, live stats are collected into "latest". Once a minute, "latest" is copied to "last1min" and "latest" is reset. Then "last5min" and "last15min" are recalculated from a history of the last 5 or 15 1-minute periods.

Each period has the following subkeys:

 * start: the start time (in seconds-since-1-Jan-1970) of this statistics collection period.
 * end: the end time (in seconds-since-1-Jan-1970) of this statistics collection period.
 * local: statistics about messages received from a local SDR dongle. Not present in --net-only mode. Has subkeys:
   * blocks_processed: number of sample blocks processed
   * blocks_dropped: number of sample blocks dropped before processing. A nonzero value means CPU overload.
   * modeac: number of Mode A / C messages decoded
   * modes: number of Mode S preambles received. This is *not* the number of valid messages!
   * bad: number of Mode S preambles that didn't result in a valid message
   * unknown_icao: number of Mode S preambles which looked like they might be valid but we didn't recognize the ICAO address and it was one of the message types where we can't be sure it's valid in this case.
   * accepted: array. Index N has the number of valid Mode S messages accepted with N-bit errors corrected.
   * signal: mean signal power of successfully received messages, in dbFS; always negative.
   * peak_signal: peak signal power of a successfully received message, in dbFS; always negative.
   * strong_signals: number of messages received that had a signal power above -3dBFS.
 * remote: statistics about messages received from remote clients. Only present in --net or --net-only mode. Has subkeys:
   * modeac: number of Mode A / C messages received.
   * modes: number of Mode S messages received.
   * bad: number of Mode S messages that had bad CRC or were otherwise invalid.
   * unknown_icao: number of Mode S messages which looked like they might be valid but we didn't recognize the ICAO address and it was one of the message types where we can't be sure it's valid in this case.
   * accepted: array. Index N has the number of valid Mode S messages accepted with N-bit errors corrected.
   * http_requests: number of HTTP requests handled.
 * cpu: statistics about CPU use. Has subkeys:
   * demod: milliseconds spent doing demodulation and decoding in response to data from a SDR dongle
   * reader: milliseconds spent reading sample data over USB from a SDR dongle
   * background: milliseconds spent doing network I/O, processing received network messages, and periodic tasks.
 * cpr: statistics about Compact Position Report message decoding. Has subkeys:
   * surface: total number of surface CPR messages received
   * airborne: total number of airborne CPR messages received
   * global_ok: global positions successfuly derived
   * global_bad: global positions that were rejected because they were inconsistent
     * global_range: global positions that were rejected because they exceeded the receiver max range
     * global_speed: global positions that were rejected because they failed the inter-position speed check
   * global_skipped: global position attempts skipped because we did not have the right data (e.g. even/odd messages crossed a zone boundary)
   * local_ok: local (relative) positions successfully found
     * local_aircraft_relative: local positions found relative to a previous aircraft position
     * local_receiver_relative: local positions found relative to the receiver position
   * local_skipped: local (relative) positions not used because we did not have the right data
     * local_range: local positions not used because they exceeded the receiver max range or fell into the ambiguous part of the receiver range
     * local_speed: local positions not used because they failed the inter-position speed check
   * filtered: number of CPR messages ignored because they matched one of the heuristics for faulty transponder output
 * tracks: statistics on aircraft tracks. Each track represents a unique aircraft and persists for up to 5 minutes after the last message
   from the aircraft is heard. If messages from the same aircraft are subsequently heard after the 5 minute period, this will be counted
   as a new track.
   * all: total tracks created
   * single_message: tracks consisting of only a single message. These are usually due to message decoding errors that produce a bad aircraft address.
 * messages: total number of messages accepted by readsb from any source
